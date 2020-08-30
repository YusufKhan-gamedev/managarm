#include <string.h>

#include <async/algorithm.hpp>
#include <async/cancellation.hpp>
#include <frg/container_of.hpp>
#include <thor-internal/event.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/ipc-queue.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/kernlet.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/random.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/timer.hpp>
#ifdef __x86_64__
#include <thor-internal/arch/debug.hpp>
#include <thor-internal/arch/ept.hpp>
#include <thor-internal/arch/vmx.hpp>
#endif
#include "../../hel/include/hel.h"

using namespace thor;

extern "C" int doCopyFromUser(void *dest, const void *src, size_t size);
extern "C" int doCopyToUser(void *dest, const void *src, size_t size);
extern "C" int doAtomicUserLoad(unsigned int *out, const unsigned int *p);

bool readUserMemory(void *kernelPtr, const void *userPtr, size_t size) {
	uintptr_t limit;
	if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(userPtr), size, &limit))
		return false;
	if(inHigherHalf(limit))
		return false;
	enableUserAccess();
	int e = doCopyFromUser(kernelPtr, userPtr, size);
	disableUserAccess();
	return !e;
}

bool writeUserMemory(void *userPtr, const void *kernelPtr, size_t size) {
	uintptr_t limit;
	if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(userPtr), size, &limit))
		return false;
	if(inHigherHalf(limit))
		return false;
	enableUserAccess();
	int e = doCopyToUser(userPtr, kernelPtr, size);
	disableUserAccess();
	return !e;
}

template<typename T>
bool readUserObject(const T *pointer, T &object) {
	return readUserMemory(&object, pointer, sizeof(T));
}

template<typename T>
bool writeUserObject(T *pointer, T object) {
	return writeUserMemory(pointer, &object, sizeof(T));
}

template<typename T>
bool readUserArray(const T *pointer, T *array, size_t count) {
	size_t size;
	if(__builtin_mul_overflow(sizeof(T), count, &size))
		return false;
	return readUserMemory(array, pointer, size);
}

template<typename T>
bool writeUserArray(T *pointer, const T *array, size_t count) {
	size_t size;
	if(__builtin_mul_overflow(sizeof(T), count, &size))
		return false;
	return writeUserMemory(pointer, array, size);
}

size_t ipcSourceSize(size_t size) {
	return (size + 7) & ~size_t(7);
}

// TODO: one translate function per error source?
HelError translateError(Error error) {
	switch(error) {
	case Error::success: return kHelErrNone;
	case Error::threadExited: return kHelErrThreadTerminated;
	case Error::transmissionMismatch: return kHelErrTransmissionMismatch;
	case Error::laneShutdown: return kHelErrLaneShutdown;
	case Error::endOfLane: return kHelErrEndOfLane;
	case Error::bufferTooSmall: return kHelErrBufferTooSmall;
	case Error::fault: return kHelErrFault;
	default:
		assert(!"Unexpected error");
		__builtin_unreachable();
	}
}

HelError helLog(const char *string, size_t length) {
	size_t offset = 0;
	while(offset < length) {
		auto chunk = frigg::min(length - offset, size_t{100});

		char buffer[100];
		if(!readUserArray(string + offset, buffer, chunk))
			return kHelErrFault;

		auto p = infoLogger();
		for(size_t i = 0; i < chunk; i++)
			p << frg::char_fmt(buffer[i]);
		p << frg::endlog;

		offset += chunk;
	}

	return kHelErrNone;
}


HelError helCreateUniverse(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto new_universe = frigg::makeShared<Universe>(*kernelAlloc);

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				UniverseDescriptor(std::move(new_universe)));
	}

	return kHelErrNone;
}

HelError helTransferDescriptor(HelHandle handle, HelHandle universe_handle,
		HelHandle *out_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	AnyDescriptor descriptor;
	frigg::SharedPtr<Universe> universe;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard lock(this_universe->lock);

		auto descriptor_it = this_universe->getDescriptor(lock, handle);
		if(!descriptor_it)
			return kHelErrNoDescriptor;
		descriptor = *descriptor_it;

		if(universe_handle == kHelThisUniverse) {
			universe = this_universe.toShared();
		}else{
			auto universe_it = this_universe->getDescriptor(lock, universe_handle);
			if(!universe_it)
				return kHelErrNoDescriptor;
			if(!universe_it->is<UniverseDescriptor>())
				return kHelErrBadDescriptor;
			universe = universe_it->get<UniverseDescriptor>().universe;
		}
	}

	// TODO: make sure the descriptor is copyable.

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard lock(universe->lock);

		*out_handle = universe->attachDescriptor(lock, std::move(descriptor));
	}
	return kHelErrNone;
}

HelError helDescriptorInfo(HelHandle handle, HelDescriptorInfo *info) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto irq_lock = frg::guard(&irqMutex());
	Universe::Guard universe_guard(this_universe->lock);

	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	switch(wrapper->tag()) {
	default:
		assert(!"Illegal descriptor");
	}

	return kHelErrNone;
}

HelError helGetCredentials(HelHandle handle, uint32_t flags, char *credentials) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();
	assert(!flags);

	frigg::SharedPtr<Thread> thread;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(thisUniverse->lock);

		if(handle == kHelThisThread) {
			thread = thisThread.toShared();
		}else{
			auto threadWrapper = thisUniverse->getDescriptor(universe_guard, handle);
			if(!threadWrapper)
				return kHelErrNoDescriptor;
			if(!threadWrapper->is<ThreadDescriptor>())
				return kHelErrBadDescriptor;
			thread = threadWrapper->get<ThreadDescriptor>().thread;
		}
	}

	if(!writeUserMemory(credentials, thread->credentials(), 16))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helCloseDescriptor(HelHandle universeHandle, HelHandle handle) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	frigg::SharedPtr<Universe> universe;
	if(universeHandle == kHelThisUniverse) {
		universe = thisUniverse.toShared();
	}else{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeLock(thisUniverse->lock);

		auto universeIt = thisUniverse->getDescriptor(universeLock, universeHandle);
		if(!universeIt)
			return kHelErrNoDescriptor;
		if(!universeIt->is<UniverseDescriptor>())
			return kHelErrBadDescriptor;
		universe = universeIt->get<UniverseDescriptor>().universe;
	}

	auto irqLock = frg::guard(&irqMutex());
	Universe::Guard otherUniverseLock(universe->lock);

	if(!universe->detachDescriptor(otherUniverseLock, handle))
		return kHelErrNoDescriptor;

	return kHelErrNone;
}

HelError helCreateQueue(HelQueue *head, uint32_t flags,
		unsigned int size_shift, size_t element_limit, HelHandle *handle) {
	assert(!flags);
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto queue = frigg::makeShared<IpcQueue>(*kernelAlloc,
			this_thread->getAddressSpace().lock(), head,
			size_shift, element_limit);
	queue->setupSelfPtr(queue);
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				QueueDescriptor(std::move(queue)));
	}

	return kHelErrNone;
}

HelError helSetupChunk(HelHandle queue_handle, int index, HelChunk *chunk, uint32_t flags) {
	assert(!flags);
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	queue->setupChunk(index, this_thread->getAddressSpace().lock(), chunk);

	return kHelErrNone;
}

HelError helCancelAsync(HelHandle handle, uint64_t async_id) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	queue->cancel(async_id);

	return kHelErrNone;
}

HelError helAllocateMemory(size_t size, uint32_t flags,
		HelAllocRestrictions *restrictions, HelHandle *handle) {
	if(!size)
		return kHelErrIllegalArgs;
	if(size & (kPageSize - 1))
		return kHelErrIllegalArgs;

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

//	auto pressure = physicalAllocator->numUsedPages() * kPageSize;
//	infoLogger() << "Allocate " << (void *)size
//			<< ", sum of allocated memory: " << (void *)pressure << frg::endlog;

	HelAllocRestrictions effective{
		.addressBits = 64
	};
	if(restrictions)
		if(!readUserMemory(&effective, restrictions, sizeof(HelAllocRestrictions)))
			return kHelErrFault;

	frigg::SharedPtr<MemoryView> memory;
	if(flags & kHelAllocContinuous) {
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, effective.addressBits,
				size, kPageSize);
	}else if(flags & kHelAllocOnDemand) {
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, effective.addressBits);
	}else{
		// TODO: 
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, effective.addressBits);
	}

	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		*handle = thisUniverse->attachDescriptor(universeGuard,
				MemoryViewDescriptor(std::move(memory)));
	}

	return kHelErrNone;
}

HelError helResizeMemory(HelHandle handle, size_t newSize) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryViewDescriptor>().memory;
	}

	Thread::asyncBlockCurrent([] (frigg::SharedPtr<MemoryView> memory, size_t newSize)
			-> coroutine<void> {
		co_await memory->resize(newSize);
	}(std::move(memory), newSize));

	return kHelErrNone;
}

HelError helCreateManagedMemory(size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle) {
	(void)flags;
	assert(!(size & (kPageSize - 1)));

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto managed = frigg::makeShared<ManagedSpace>(*kernelAlloc, size);
	auto backing_memory = frigg::makeShared<BackingMemory>(*kernelAlloc, managed);
	auto frontal_memory = frigg::makeShared<FrontalMemory>(*kernelAlloc, std::move(managed));

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*backing_handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(backing_memory)));
		*frontal_handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(frontal_memory)));
	}

	return kHelErrNone;
}

HelError helCopyOnWrite(HelHandle memoryHandle,
		uintptr_t offset, size_t size, HelHandle *outHandle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> view;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, memoryHandle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		view = wrapper->get<MemoryViewDescriptor>().memory;
	}

	auto slice = frigg::makeShared<CopyOnWriteMemory>(*kernelAlloc, std::move(view),
			offset, size);
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*outHandle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(slice)));
	}

	return kHelErrNone;
}

HelError helAccessPhysical(uintptr_t physical, size_t size, HelHandle *handle) {
	assert((physical % kPageSize) == 0);
	assert((size % kPageSize) == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memory = frigg::makeShared<HardwareMemory>(*kernelAlloc, physical, size,
			CachingMode::null);
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(memory)));
	}

	return kHelErrNone;
}

HelError helCreateIndirectMemory(size_t numSlots, HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memory = frigg::makeShared<IndirectMemory>(*kernelAlloc, numSlots);
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(memory)));
	}

	return kHelErrNone;
}

HelError helAlterMemoryIndirection(HelHandle indirectHandle, size_t slot,
		HelHandle memoryHandle, uintptr_t offset, size_t size) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	frigg::SharedPtr<MemoryView> indirectView;
	frigg::SharedPtr<MemoryView> memoryView;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeLock(thisUniverse->lock);

		auto indirectWrapper = thisUniverse->getDescriptor(universeLock, indirectHandle);
		if(!indirectWrapper)
			return kHelErrNoDescriptor;
		if(!indirectWrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		indirectView = indirectWrapper->get<MemoryViewDescriptor>().memory;

		auto memoryWrapper = thisUniverse->getDescriptor(universeLock, memoryHandle);
		if(!memoryWrapper)
			return kHelErrNoDescriptor;
		if(!memoryWrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memoryView = memoryWrapper->get<MemoryViewDescriptor>().memory;
	}

	if(auto e = indirectView->setIndirection(slot, std::move(memoryView), offset, size);
			e != Error::success) {
		if(e == Error::illegalObject) {
			return kHelErrUnsupportedOperation;
		}else{
			assert(e == Error::outOfBounds);
			return kHelErrOutOfBounds;
		}
	}
	return kHelErrNone;
}

HelError helCreateSliceView(HelHandle memoryHandle,
		uintptr_t offset, size_t size, uint32_t flags, HelHandle *handle) {
	assert(!flags);
	assert((offset % kPageSize) == 0);
	assert((size % kPageSize) == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> view;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, memoryHandle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		view = wrapper->get<MemoryViewDescriptor>().memory;
	}

	auto slice = frigg::makeShared<MemorySlice>(*kernelAlloc,
			std::move(view), offset, size);
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemorySliceDescriptor(std::move(slice)));
	}

	return kHelErrNone;
}

HelError helForkMemory(HelHandle handle, HelHandle *forkedHandle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> view;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto viewWrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!viewWrapper)
			return kHelErrNoDescriptor;
		if(!viewWrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		view = viewWrapper->get<MemoryViewDescriptor>().memory;
	}

	auto [error, forkedView] = Thread::asyncBlockCurrent(view->fork());

	if(error == Error::illegalObject)
		return kHelErrUnsupportedOperation;
	assert(error == Error::success);

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*forkedHandle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(forkedView));
	}

	return kHelErrNone;
}

HelError helCreateSpace(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto space = AddressSpace::create();

	auto irq_lock = frg::guard(&irqMutex());
	Universe::Guard universe_guard(this_universe->lock);

	*handle = this_universe->attachDescriptor(universe_guard,
			AddressSpaceDescriptor(std::move(space)));

	return kHelErrNone;
}

HelError helCreateVirtualizedSpace(HelHandle *handle) {
#ifdef __x86_64__
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto this_thread = getCurrentThread();
	auto irq_lock = frg::guard(&irqMutex());
	auto this_universe = this_thread->getUniverse();

	PhysicalAddr pml4e = physicalAllocator->allocate(kPageSize);
	if(pml4e == static_cast<PhysicalAddr>(-1)) {
		return kHelErrNoMemory;
	}
	PageAccessor paccessor{pml4e};
	memset(paccessor.get(), 0, kPageSize);
	auto vspace = thor::vmx::EptSpace::create(pml4e);
	Universe::Guard universe_guard(this_universe->lock);
	*handle = this_universe->attachDescriptor(universe_guard,
			VirtualizedSpaceDescriptor(std::move(vspace)));
	return kHelErrNone;
#else
	return kHelErrNoHardwareSupport;
#endif
}

HelError helCreateVirtualizedCpu(HelHandle handle, HelHandle *out) {
#ifdef __x86_64__
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto irq_lock = frg::guard(&irqMutex());
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	Universe::Guard universe_guard(this_universe->lock);

	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	if(!wrapper->is<VirtualizedSpaceDescriptor>())
		return kHelErrBadDescriptor;
	auto space = wrapper->get<VirtualizedSpaceDescriptor>();

	smarter::shared_ptr<vmx::Vmcs> vcpu = smarter::allocate_shared<vmx::Vmcs>(Allocator{}, (smarter::static_pointer_cast<thor::vmx::EptSpace>(space.space)));

	*out = this_universe->attachDescriptor(universe_guard,
			VirtualizedCpuDescriptor(std::move(vcpu)));
	return kHelErrNone;
#else
	return kHelErrNoHardwareSupport;
#endif
}

HelError helRunVirtualizedCpu(HelHandle handle, HelVmexitReason *exitInfo) {
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	Universe::Guard universe_guard(this_universe->lock);

	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	if(!wrapper->is<VirtualizedCpuDescriptor>())
		return kHelErrBadDescriptor;
	auto cpu = wrapper->get<VirtualizedCpuDescriptor>();
	auto info = cpu.vcpu->run();
	if(!writeUserObject(exitInfo, info))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helGetRandomBytes(void *buffer, size_t wantedSize, size_t *actualSize) {
	char bounceBuffer[128];
	size_t generatedSize = generateRandomBytes(bounceBuffer,
			frg::min(wantedSize, size_t{128}));

	if(!writeUserMemory(buffer, bounceBuffer, generatedSize))
		return kHelErrFault;

	*actualSize = generatedSize;
	return kHelErrNone;
}

HelError helMapMemory(HelHandle memory_handle, HelHandle space_handle,
		void *pointer, uintptr_t offset, size_t length, uint32_t flags, void **actualPointer) {
	if(length == 0)
		return kHelErrIllegalArgs;
	if((uintptr_t)pointer % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(offset % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(length % kPageSize != 0)
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	uint32_t map_flags = 0;
	if(pointer != nullptr) {
		map_flags |= AddressSpace::kMapFixed;
	}else{
		map_flags |= AddressSpace::kMapPreferTop;
	}

	if(flags & kHelMapProtRead)
		map_flags |= AddressSpace::kMapProtRead;
	if(flags & kHelMapProtWrite)
		map_flags |= AddressSpace::kMapProtWrite;
	if(flags & kHelMapProtExecute)
		map_flags |= AddressSpace::kMapProtExecute;

	if(flags & kHelMapDontRequireBacking)
		map_flags |= AddressSpace::kMapDontRequireBacking;

	frigg::SharedPtr<MemorySlice> slice;
	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	smarter::shared_ptr<VirtualSpace> vspace;
	bool isVspace = false;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, memory_handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(memory_wrapper->is<MemorySliceDescriptor>()) {
			slice = memory_wrapper->get<MemorySliceDescriptor>().slice;
		}else if(memory_wrapper->is<MemoryViewDescriptor>()) {
			auto memory = memory_wrapper->get<MemoryViewDescriptor>().memory;
			auto bundle_length = memory->getLength();
			slice = frigg::makeShared<MemorySlice>(*kernelAlloc,
					std::move(memory), 0, bundle_length);
		}else{
			return kHelErrBadDescriptor;
		}

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().lock();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(space_wrapper->is<AddressSpaceDescriptor>()) {
				space = space_wrapper->get<AddressSpaceDescriptor>().space;
			} else if(space_wrapper->is<VirtualizedSpaceDescriptor>()) {
				isVspace = true;
				vspace = space_wrapper->get<VirtualizedSpaceDescriptor>().space;
			} else {
				return kHelErrBadDescriptor;
			}
		}
	}

	// TODO: check proper alignment

	frg::expected<Error, VirtualAddr> mapResult;
	if(!isVspace) {
		mapResult = Thread::asyncBlockCurrent(space->map(slice,
				(VirtualAddr)pointer, offset, length, map_flags));
	} else {
		mapResult = Thread::asyncBlockCurrent(vspace->map(slice,
				(VirtualAddr)pointer, offset, length, map_flags));
	}

	if(!mapResult) {
		assert(mapResult.error() == Error::bufferTooSmall);
		return kHelErrBufferTooSmall;
	}

	*actualPointer = (void *)mapResult.value();
	return kHelErrNone;
}

HelError helSubmitProtectMemory(HelHandle space_handle,
		void *pointer, size_t length, uint32_t flags,
		HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	uint32_t protectFlags = 0;
	if(flags & kHelMapProtRead)
		protectFlags |= AddressSpace::kMapProtRead;
	if(flags & kHelMapProtWrite)
		protectFlags |= AddressSpace::kMapProtWrite;
	if(flags & kHelMapProtExecute)
		protectFlags |= AddressSpace::kMapProtExecute;

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().lock();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelSimpleResult))))
		return kHelErrQueueTooSmall;

	async::detach_with_allocator(*kernelAlloc, [](
				smarter::shared_ptr<AddressSpace, BindableHandle> space,
				frigg::SharedPtr<IpcQueue> queue,
				VirtualAddr pointer, size_t length,
				uint32_t protectFlags, uintptr_t context) -> coroutine<void> {
			co_await space->protect(pointer, length, protectFlags);

			HelSimpleResult helResult{kHelErrNone};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
	}(
		std::move(space), std::move(queue), reinterpret_cast<VirtualAddr>(pointer),
		length, protectFlags, context)
	);

	return kHelErrNone;
}

HelError helUnmapMemory(HelHandle space_handle, void *pointer, size_t length) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().lock();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	Thread::asyncBlockCurrent(space->unmap((VirtualAddr)pointer, length));

	return kHelErrNone;
}

HelError helSubmitSynchronizeSpace(HelHandle spaceHandle, void *pointer, size_t length,
		HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		if(spaceHandle == kHelNullHandle) {
			space = thisThread->getAddressSpace().lock();
		}else{
			auto spaceWrapper = thisUniverse->getDescriptor(universeGuard, spaceHandle);
			if(!spaceWrapper)
				return kHelErrNoDescriptor;
			if(!spaceWrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = spaceWrapper->get<AddressSpaceDescriptor>().space;
		}

		auto queueWrapper = thisUniverse->getDescriptor(universeGuard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	async::detach_with_allocator(*kernelAlloc, [] (smarter::shared_ptr<AddressSpace, BindableHandle> space,
			void *pointer, size_t length,
			frigg::SharedPtr<IpcQueue> queue, uintptr_t context) -> coroutine<void> {
		co_await space->synchronize((VirtualAddr)pointer, length);

		HelSimpleResult helResult{kHelErrNone};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(space), pointer, length, std::move(queue), context));

	return kHelErrNone;
}

HelError helPointerPhysical(void *pointer, uintptr_t *physical) {
	auto this_thread = getCurrentThread();

	auto space = this_thread->getAddressSpace().lock();

	auto disp = (reinterpret_cast<uintptr_t>(pointer) & (kPageSize - 1));
	auto accessor = AddressSpaceLockHandle{std::move(space),
			reinterpret_cast<char *>(pointer) - disp, kPageSize};

	// FIXME: The physical page can change after we destruct the accessor!
	// We need a better hel API to properly handle that case.
	Thread::asyncBlockCurrent(accessor.acquire());

	auto page_physical = accessor.getPhysical(0);

	*physical = page_physical + disp;

	return kHelErrNone;
}

HelError helSubmitReadMemory(HelHandle handle, uintptr_t address,
		size_t length, void *buffer,
		HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	AnyDescriptor descriptor;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		auto wrapper = thisUniverse->getDescriptor(universeGuard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		descriptor = *wrapper;

		auto queueWrapper = thisUniverse->getDescriptor(universeGuard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	auto readMemoryView = [] (frigg::SharedPtr<Thread> submitThread,
			frigg::SharedPtr<MemoryView> view,
			uintptr_t address, size_t length, void *buffer,
			frigg::SharedPtr<IpcQueue> queue, uintptr_t context) -> coroutine<void> {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(buffer), length, &limit)) {
			HelSimpleResult helResult{kHelErrIllegalArgs};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[128];
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frigg::min(length - progress, size_t{128});
				co_await copyFromView(view.get(), address + progress, temp, chunk);

				// Enter the submitter's work-queue so that we can access memory directly.
				co_await submitThread->mainWorkQueue()->schedule();

				if(!writeUserMemory(reinterpret_cast<char *>(buffer) + progress, temp, chunk)) {
					error = Error::fault;
					break;
				}
				progress += chunk;
			}
		}

		HelSimpleResult helResult{translateError(error)};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto readAddressSpace = [] (frigg::SharedPtr<Thread> submitThread,
			smarter::shared_ptr<AddressSpace, BindableHandle> space,
			uintptr_t address, size_t length, void *buffer,
			frigg::SharedPtr<IpcQueue> queue, uintptr_t context) -> coroutine<void> {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(buffer), length, &limit)) {
			HelSimpleResult helResult{kHelErrIllegalArgs};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			auto lockHandle = AddressSpaceLockHandle{std::move(space),
					(void *)address, length};
			co_await lockHandle.acquire();

			// Enter the submitter's work-queue so that we can access memory directly.
			co_await submitThread->mainWorkQueue()->schedule();

			char temp[128];
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frigg::min(length - progress, size_t{128});
				lockHandle.load(progress, temp, chunk);
				if(!writeUserMemory(reinterpret_cast<char *>(buffer) + progress, temp, chunk)) {
					error = Error::fault;
					break;
				}
				progress += chunk;
			}
		}

		HelSimpleResult helResult{translateError(error)};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto readVirtualizedSpace = [] (frigg::SharedPtr<Thread> submitThread,
			smarter::shared_ptr<VirtualizedPageSpace> space,
			uintptr_t address, size_t length, void *buffer,
			frigg::SharedPtr<IpcQueue> queue, uintptr_t context) -> coroutine<void> {
		// Enter the submitter's work-queue so that we can access memory directly.
		co_await submitThread->mainWorkQueue()->schedule();

		enableUserAccess();
		auto error = space->load(address, length, buffer);
		disableUserAccess();
		assert(error == Error::success || error == Error::fault);

		HelSimpleResult helResult{translateError(error)};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	if(descriptor.is<MemoryViewDescriptor>()) {
		auto view = descriptor.get<MemoryViewDescriptor>().memory;
		async::detach_with_allocator(*kernelAlloc, readMemoryView(thisThread.toShared(),
				std::move(view), address, length, buffer, std::move(queue), context));
	}else if(descriptor.is<AddressSpaceDescriptor>()) {
		auto space = descriptor.get<AddressSpaceDescriptor>().space;
		async::detach_with_allocator(*kernelAlloc, readAddressSpace(thisThread.toShared(),
				std::move(space), address, length, buffer, std::move(queue), context));
	}else if(descriptor.is<ThreadDescriptor>()) {
		auto thread = descriptor.get<ThreadDescriptor>().thread;
		auto space = thread->getAddressSpace().lock();
		async::detach_with_allocator(*kernelAlloc, readAddressSpace(thisThread.toShared(),
				std::move(space), address, length, buffer, std::move(queue), context));
	}else if(descriptor.is<VirtualizedSpaceDescriptor>()) {
		auto space = descriptor.get<VirtualizedSpaceDescriptor>().space;
		async::detach_with_allocator(*kernelAlloc, readVirtualizedSpace(thisThread.toShared(),
				std::move(space), address, length, buffer, std::move(queue), context));
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helSubmitWriteMemory(HelHandle handle, uintptr_t address,
		size_t length, const void *buffer,
		HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	AnyDescriptor descriptor;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		auto wrapper = thisUniverse->getDescriptor(universeGuard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		descriptor = *wrapper;

		auto queueWrapper = thisUniverse->getDescriptor(universeGuard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	auto writeMemoryView = [] (frigg::SharedPtr<Thread> submitThread,
			frigg::SharedPtr<MemoryView> view,
			uintptr_t address, size_t length, const void *buffer,
			frigg::SharedPtr<IpcQueue> queue, uintptr_t context) -> coroutine<void> {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(buffer), length, &limit)) {
			HelSimpleResult helResult{kHelErrIllegalArgs};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[128];
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frigg::min(length - progress, size_t{128});

				// Enter the submitter's work-queue so that we can access memory directly.
				co_await submitThread->mainWorkQueue()->schedule();

				if(!readUserMemory(temp,
						reinterpret_cast<const char *>(buffer) + progress, chunk)) {
					error = Error::fault;
					break;
				}

				co_await copyToView(view.get(), address + progress, temp, chunk);
				progress += chunk;
			}
		}

		HelSimpleResult helResult{translateError(error)};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto writeAddressSpace = [] (frigg::SharedPtr<Thread> submitThread,
			smarter::shared_ptr<AddressSpace, BindableHandle> space,
			uintptr_t address, size_t length, const void *buffer,
			frigg::SharedPtr<IpcQueue> queue, uintptr_t context) -> coroutine<void> {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(buffer), length, &limit)) {
			HelSimpleResult helResult{kHelErrIllegalArgs};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			auto lockHandle = AddressSpaceLockHandle{std::move(space),
					(void *)address, length};
			co_await lockHandle.acquire();

			// Enter the submitter's work-queue so that we can access memory directly.
			co_await submitThread->mainWorkQueue()->schedule();

			char temp[128];
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frigg::min(length - progress, size_t{128});
				if(!readUserMemory(temp,
						reinterpret_cast<const char *>(buffer) + progress, chunk)) {
					error = Error::fault;
					break;
				}
				lockHandle.write(progress, temp, chunk);
				progress += chunk;
			}
		}

		HelSimpleResult helResult{translateError(error)};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto writeVirtualizedSpace = [] (frigg::SharedPtr<Thread> submitThread,
			smarter::shared_ptr<VirtualizedPageSpace> space,
			uintptr_t address, size_t length, const void *buffer,
			frigg::SharedPtr<IpcQueue> queue, uintptr_t context) -> coroutine<void> {
		// Enter the submitter's work-queue so that we can access memory directly.
		co_await submitThread->mainWorkQueue()->schedule();

		enableUserAccess();
		auto error = space->store(address, length, buffer);
		disableUserAccess();
		assert(error == Error::success || error == Error::fault);

		HelSimpleResult helResult{translateError(error)};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	if(descriptor.is<MemoryViewDescriptor>()) {
		auto view = descriptor.get<MemoryViewDescriptor>().memory;
		async::detach_with_allocator(*kernelAlloc, writeMemoryView(thisThread.toShared(),
				std::move(view), address, length, buffer, std::move(queue), context));
	}else if(descriptor.is<AddressSpaceDescriptor>()) {
		auto space = descriptor.get<AddressSpaceDescriptor>().space;
		async::detach_with_allocator(*kernelAlloc, writeAddressSpace(thisThread.toShared(),
				std::move(space), address, length, buffer, std::move(queue), context));
	}else if(descriptor.is<ThreadDescriptor>()) {
		auto thread = descriptor.get<ThreadDescriptor>().thread;
		auto space = thread->getAddressSpace().lock();
		async::detach_with_allocator(*kernelAlloc, writeAddressSpace(thisThread.toShared(),
				std::move(space), address, length, buffer, std::move(queue), context));
	}else if(descriptor.is<VirtualizedSpaceDescriptor>()) {
		auto space = descriptor.get<VirtualizedSpaceDescriptor>().space;
		async::detach_with_allocator(*kernelAlloc, writeVirtualizedSpace(thisThread.toShared(),
				std::move(space), address, length, buffer, std::move(queue), context));
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryViewDescriptor>().memory;
	}

	*size = memory->getLength();
	return kHelErrNone;
}

HelError helSubmitManageMemory(HelHandle handle, HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryViewDescriptor>().memory;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelManageResult))))
		return kHelErrQueueTooSmall;

	async::detach_with_allocator(*kernelAlloc, [](
				frigg::SharedPtr<IpcQueue> queue,
				frigg::SharedPtr<MemoryView> memory,
				uintptr_t context) -> coroutine<void> {
			auto [error, type, offset, size] = co_await memory->submitManage();

			int helType;
			switch (type) {
				case ManageRequest::initialize: helType = kHelManageInitialize; break;
				case ManageRequest::writeback: helType = kHelManageWriteback; break;
				default:
					assert(!"unexpected ManageRequest");
					__builtin_trap();
			}

			HelManageResult helResult{translateError(error),
					helType, offset, size};
			QueueSource ipcSource{&helResult, sizeof(HelManageResult), nullptr};
			co_await queue->submit(&ipcSource, context);
	}(std::move(queue), std::move(memory), context));

	return kHelErrNone;
}

HelError helUpdateMemory(HelHandle handle, int type,
		uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryViewDescriptor>().memory;
	}

	Error error;
	switch(type) {
	case kHelManageInitialize:
		error = memory->updateRange(ManageRequest::initialize, offset, length);
		break;
	case kHelManageWriteback:
		error = memory->updateRange(ManageRequest::writeback, offset, length);
		break;
	default:
		return kHelErrIllegalArgs;
	}

	if(error == Error::illegalObject)
		return kHelErrUnsupportedOperation;

	assert(error == Error::success);
	return kHelErrNone;
}

HelError helSubmitLockMemoryView(HelHandle handle, uintptr_t offset, size_t size,
		HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryViewDescriptor>().memory;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelHandleResult))))
		return kHelErrQueueTooSmall;

	async::detach_with_allocator(*kernelAlloc, [](
				frigg::UnsafePtr<thor::Universe, frigg::SharedControl> universe,
				frigg::SharedPtr<MemoryView> memory,
				frigg::SharedPtr<IpcQueue> queue,
				uintptr_t offset, size_t size,
				uintptr_t context) -> coroutine<void> {
			auto initiateError = co_await memory->submitInitiateLoad(ManageRequest::initialize, offset, size);

			MemoryViewLockHandle lock_handle{memory, offset, size};
			co_await lock_handle.acquire();
			if(!lock_handle) {
				// TODO: Return a better error.
				HelHandleResult helResult{kHelErrFault, 0, 0};
				QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
				co_await queue->submit(&ipcSource, context);
				co_return;
			}

			// Attach the descriptor.
			HelHandle handle;
			{
				auto irq_lock = frg::guard(&irqMutex());
				Universe::Guard lock(universe->lock);

				handle = universe->attachDescriptor(lock,
						MemoryViewLockDescriptor{
							frigg::makeShared<NamedMemoryViewLock>(
								*kernelAlloc, std::move(lock_handle))});
			}

			HelHandleResult helResult{translateError(initiateError), 0, handle};
			QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
	}(
		std::move(this_universe), std::move(memory), std::move(queue),
		offset, size, context
	));

	return kHelErrNone;
}

HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryViewDescriptor>().memory;
	}

/*	auto handle_load = frigg::makeShared<AsyncInitiateLoad>(*kernelAlloc,
			NullCompleter(), offset, length);
	{
		// TODO: protect memory object with a guard
		memory->submitInitiateLoad(std::move(handle_load));
	}*/

	return kHelErrNone;
}

std::atomic<unsigned int> globalNextCpu = 0;

HelError helCreateThread(HelHandle universe_handle, HelHandle space_handle,
		int abi, void *ip, void *sp, uint32_t flags, HelHandle *handle) {
	(void)abi;
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	if(flags & ~(kHelThreadStopped))
		return kHelErrIllegalArgs;

	frigg::SharedPtr<Universe> universe;
	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		if(universe_handle == kHelNullHandle) {
			universe = this_thread->getUniverse().toShared();
		}else{
			auto universe_wrapper = this_universe->getDescriptor(universe_guard, universe_handle);
			if(!universe_wrapper)
				return kHelErrNoDescriptor;
			if(!universe_wrapper->is<UniverseDescriptor>())
				return kHelErrBadDescriptor;
			universe = universe_wrapper->get<UniverseDescriptor>().universe;
		}

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().lock();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	AbiParameters params;
	params.ip = (uintptr_t)ip;
	params.sp = (uintptr_t)sp;

	auto new_thread = Thread::create(std::move(universe), std::move(space), params);
	new_thread->self = new_thread;

	// Adding a large prime (coprime to getCpuCount()) should yield a good distribution.
	auto cpu = globalNextCpu.fetch_add(4099, std::memory_order_relaxed) % getCpuCount();
//	infoLogger() << "thor: New thread on CPU #" << cpu << frg::endlog;
	Scheduler::associate(new_thread.get(), &getCpuData(cpu)->scheduler);
//	Scheduler::associate(new_thread.get(), localScheduler());
	if(!(flags & kHelThreadStopped))
		Thread::resumeOther(new_thread);

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				ThreadDescriptor(std::move(new_thread)));
	}

	return kHelErrNone;
}

HelError helQueryThreadStats(HelHandle handle, HelThreadStats *user_stats) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	if(handle == kHelThisThread) {
		thread = this_thread.toShared();
	}else{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	HelThreadStats stats;
	memset(&stats, 0, sizeof(HelThreadStats));
	stats.userTime = thread->runTime();

	if(!writeUserObject(user_stats, stats))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helSetPriority(HelHandle handle, int priority) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	if(handle == kHelThisThread) {
		thread = this_thread.toShared();
	}else{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	Scheduler::setPriority(thread.get(), priority);

	return kHelErrNone;
}

HelError helYield() {
	Thread::deferCurrent();

	return kHelErrNone;
}

HelError helSubmitObserve(HelHandle handle, uint64_t inSeq,
		HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		auto threadWrapper = thisUniverse->getDescriptor(universeGuard, handle);
		if(!threadWrapper)
			return kHelErrNoDescriptor;
		if(!threadWrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = threadWrapper->get<ThreadDescriptor>().thread;

		auto queueWrapper = thisUniverse->getDescriptor(universeGuard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelObserveResult))))
		return kHelErrQueueTooSmall;

	async::detach_with_allocator(*kernelAlloc, [] (
			frigg::SharedPtr<Thread> thread, uint64_t inSeq,
			frigg::SharedPtr<IpcQueue> queue, uintptr_t context) -> coroutine<void> {
		auto [error, sequence, interrupt] = co_await thread->observe(inSeq);

		HelObserveResult helResult{translateError(error), 0, sequence};
		if(interrupt == kIntrNull) {
			helResult.observation = kHelObserveNull;
		}else if(interrupt == kIntrRequested) {
			helResult.observation = kHelObserveInterrupt;
		}else if(interrupt == kIntrPanic) {
			helResult.observation = kHelObservePanic;
		}else if(interrupt == kIntrBreakpoint) {
			helResult.observation = kHelObserveBreakpoint;
		}else if(interrupt == kIntrPageFault) {
			helResult.observation = kHelObservePageFault;
		}else if(interrupt == kIntrGeneralFault) {
			helResult.observation = kHelObserveGeneralFault;
		}else if(interrupt == kIntrIllegalInstruction) {
			helResult.observation = kHelObserveIllegalInstruction;
		}else if(interrupt >= kIntrSuperCall) {
			helResult.observation = kHelObserveSuperCall + (interrupt - kIntrSuperCall);
		}else{
			frigg::panicLogger() << "Unexpected interrupt" << frigg::endLog;
			__builtin_unreachable();
		}
		QueueSource ipcSource{&helResult, sizeof(HelObserveResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(thread), inSeq, std::move(queue), context));
	return kHelErrNone;
}

HelError helKillThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	Thread::killOther(thread);

	return kHelErrNone;
}

HelError helInterruptThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	Thread::interruptOther(thread);

	return kHelErrNone;
}

HelError helResume(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	if(auto e = Thread::resumeOther(thread); e != Error::success) {
		if(e == Error::threadExited)
			return kHelErrThreadTerminated;
		assert(e == Error::illegalState);
		return kHelErrIllegalState;
	}

	return kHelErrNone;
}

HelError helLoadRegisters(HelHandle handle, int set, void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	VirtualizedCpuDescriptor vcpu;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(thread_wrapper->is<ThreadDescriptor>()) {
			thread = thread_wrapper->get<ThreadDescriptor>().thread;
		} else if(thread_wrapper->is<VirtualizedCpuDescriptor>()) {
			vcpu = thread_wrapper->get<VirtualizedCpuDescriptor>();
		}else{
			return kHelErrBadDescriptor;
		}
	}

	// TODO: Make sure that the thread is actually suspenend!

	if(set == kHelRegsProgram) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		uintptr_t regs[2];
		regs[0] = *thread->_executor.ip();
		regs[1] = *thread->_executor.sp();
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2))
			return kHelErrFault;
	}else if(set == kHelRegsGeneral) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#ifdef __x86_64__
		uintptr_t regs[15];
		regs[0] = thread->_executor.general()->rax;
		regs[1] = thread->_executor.general()->rbx;
		regs[2] = thread->_executor.general()->rcx;
		regs[3] = thread->_executor.general()->rdx;
		regs[4] = thread->_executor.general()->rdi;
		regs[5] = thread->_executor.general()->rsi;
		regs[6] = thread->_executor.general()->r8;
		regs[7] = thread->_executor.general()->r9;
		regs[8] = thread->_executor.general()->r10;
		regs[9] = thread->_executor.general()->r11;
		regs[10] = thread->_executor.general()->r12;
		regs[11] = thread->_executor.general()->r13;
		regs[12] = thread->_executor.general()->r14;
		regs[13] = thread->_executor.general()->r15;
		regs[14] = thread->_executor.general()->rbp;
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 15))
			return kHelErrFault;
#endif
	}else if(set == kHelRegsThread) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#ifdef __x86_64__
		uintptr_t regs[2];
		regs[0] = thread->_executor.general()->clientFs;
		regs[1] = thread->_executor.general()->clientGs;
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2))
			return kHelErrFault;
#endif
	}else if(set == kHelRegsVirtualization) {
		if(!vcpu.vcpu) {
			return kHelErrIllegalArgs;
		}
#ifdef __x86_64__
		HelX86VirtualizationRegs regs;
		memset(&regs, 0, sizeof(HelX86VirtualizationRegs));
		vcpu.vcpu->loadRegs(&regs);
		if(!writeUserObject(reinterpret_cast<HelX86VirtualizationRegs *>(image), regs))
			return kHelErrFault;
#endif
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helStoreRegisters(HelHandle handle, int set, const void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	VirtualizedCpuDescriptor vcpu{0};
	if(handle == kHelThisThread) {
		// FIXME: Properly handle this below.
		thread = this_thread.toShared();
	}else{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(thread_wrapper->is<ThreadDescriptor>()) {
			thread = thread_wrapper->get<ThreadDescriptor>().thread;
		}else if(thread_wrapper->is<VirtualizedCpuDescriptor>()) {
			vcpu = thread_wrapper->get<VirtualizedCpuDescriptor>();
		}else{
			return kHelErrBadDescriptor;
		}
	}

	// TODO: Make sure that the thread is actually suspenend!

	if(set == kHelRegsProgram) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		uintptr_t regs[2];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2))
			return kHelErrFault;
		*thread->_executor.ip() = regs[0];
		*thread->_executor.sp() = regs[1];
	}else if(set == kHelRegsGeneral) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#ifdef __x86_64__
		uintptr_t regs[15];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 15))
			return kHelErrFault;
		thread->_executor.general()->rax = regs[0];
		thread->_executor.general()->rbx = regs[1];
		thread->_executor.general()->rcx = regs[2];
		thread->_executor.general()->rdx = regs[3];
		thread->_executor.general()->rdi = regs[4];
		thread->_executor.general()->rsi = regs[5];
		thread->_executor.general()->r8 = regs[6];
		thread->_executor.general()->r9 = regs[7];
		thread->_executor.general()->r10 = regs[8];
		thread->_executor.general()->r11 = regs[9];
		thread->_executor.general()->r12 = regs[10];
		thread->_executor.general()->r13 = regs[11];
		thread->_executor.general()->r14 = regs[12];
		thread->_executor.general()->r15 = regs[13];
		thread->_executor.general()->rbp = regs[14];
#endif
	}else if(set == kHelRegsThread) {
#ifdef __x86_64__
		uintptr_t regs[2];
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2))
			return kHelErrFault;
		thread->_executor.general()->clientFs = regs[0];
		thread->_executor.general()->clientGs = regs[1];
#endif
	}else if(set == kHelRegsDebug) {
#ifdef __x86_64__
		// FIXME: Make those registers thread-specific.
		uint32_t *reg;
		readUserObject(reinterpret_cast<uint32_t *const *>(image), reg);
		breakOnWrite(reg);
#endif
	}else if(set == kHelRegsVirtualization) {
#ifdef __x86_64__
		if(!vcpu.vcpu) {
			return kHelErrIllegalArgs;
		}
		HelX86VirtualizationRegs regs;
		if(!readUserObject(reinterpret_cast<const HelX86VirtualizationRegs *>(image), regs))
			return kHelErrFault;
		vcpu.vcpu->storeRegs(&regs);
#endif
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helWriteFsBase(void *pointer) {
#ifdef __x86_64__
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, (uintptr_t)pointer);
	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helGetClock(uint64_t *counter) {
	*counter = systemClockSource()->currentNanos();
	return kHelErrNone;
}

HelError helSubmitAwaitClock(uint64_t counter, HelHandle queue_handle, uintptr_t context,
		uint64_t *async_id) {
#ifdef __x86_64__
	struct Closure final : CancelNode, PrecisionTimerNode, IpcNode {
		static void issue(uint64_t nanos, frigg::SharedPtr<IpcQueue> queue,
				uintptr_t context, uint64_t *async_id) {
			auto closure = frg::construct<Closure>(*kernelAlloc, nanos,
					std::move(queue), context);
			closure->queue->registerNode(closure);
			*async_id = closure->asyncId();
			generalTimerEngine()->installTimer(closure);
		}

		static void elapsed(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &Closure::worklet);
			if(closure->wasCancelled())
				closure->result.error = kHelErrCancelled;
			closure->queue->unregisterNode(closure);
			closure->queue->submit(closure);
		}

		explicit Closure(uint64_t nanos, frigg::SharedPtr<IpcQueue> the_queue,
				uintptr_t context)
		: queue{std::move(the_queue)},
				source{&result, sizeof(HelSimpleResult), nullptr},
				result{translateError(Error::success), 0} {
			setupContext(context);
			setupSource(&source);

			worklet.setup(&Closure::elapsed, getCurrentThread()->mainWorkQueue());
			PrecisionTimerNode::setup(nanos, cancelEvent, &worklet);
		}

		void handleCancellation() override {
			cancelEvent.cancel();
		}

		void complete() override {
			frg::destruct(*kernelAlloc, this);
		}

		Worklet worklet;
		async::cancellation_event cancelEvent;
		frigg::SharedPtr<IpcQueue> queue;
		QueueSource source;
		HelSimpleResult result;
	};

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelSimpleResult))))
		return kHelErrQueueTooSmall;

	Closure::issue(counter, std::move(queue), context, async_id);

	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helCreateStream(HelHandle *lane1_handle, HelHandle *lane2_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto lanes = createStream();
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*lane1_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(std::move(lanes.get<0>())));
		*lane2_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(std::move(lanes.get<1>())));
	}

	return kHelErrNone;
}

HelError helSubmitAsync(HelHandle handle, const HelAction *actions, size_t count,
		HelHandle queue_handle, uintptr_t context, uint32_t flags) {
	(void)flags;
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	// TODO: check userspace page access rights

	LaneHandle lane;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		if(handle == kHelThisThread) {
			lane = this_thread->inferiorLane();
		}else{
			auto wrapper = this_universe->getDescriptor(universe_guard, handle);
			if(!wrapper)
				return kHelErrNoDescriptor;
			if(wrapper->is<LaneDescriptor>()) {
				lane = wrapper->get<LaneDescriptor>().handle;
			}else if(wrapper->is<ThreadDescriptor>()) {
				lane = wrapper->get<ThreadDescriptor>().thread->superiorLane();
			}else{
				return kHelErrBadDescriptor;
			}
		}

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	size_t node_size = 0;
	for(size_t i = 0; i < count; i++) {
		HelAction action;
		readUserObject(actions + i, action);

		switch(action.type) {
		case kHelActionOffer:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionAccept:
			node_size += ipcSourceSize(sizeof(HelHandleResult));
			break;
		case kHelActionImbueCredentials:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionExtractCredentials:
			node_size += ipcSourceSize(sizeof(HelCredentialsResult));
			break;
		case kHelActionSendFromBuffer:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionSendFromBufferSg:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionRecvInline:
			// TODO: For now, we hardcode a size of 128 bytes.
			node_size += ipcSourceSize(sizeof(HelLengthResult));
			node_size += ipcSourceSize(128);
			break;
		case kHelActionRecvToBuffer:
			node_size += ipcSourceSize(sizeof(HelLengthResult));
			break;
		case kHelActionPushDescriptor:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionPullDescriptor:
			node_size += ipcSourceSize(sizeof(HelHandleResult));
			break;
		default:
			// TODO: Turn this into an error return.
			assert(!"Fix error handling here");
		}
	}

	if(!queue->validSize(node_size))
		return kHelErrQueueTooSmall;

	struct Item {
		StreamNode transmit;
		frigg::UniqueMemory<KernelAlloc> buffer;
		QueueSource mainSource;
		QueueSource dataSource;
		union {
			HelSimpleResult helSimpleResult;
			HelHandleResult helHandleResult;
			HelCredentialsResult helCredentialsResult;
			HelInlineResultNoFlex helInlineResult;
			HelLengthResult helLengthResult;
		};
	};

	struct Closure final : StreamPacket, IpcNode {
		static void transmitted(Closure *closure) {
			QueueSource *tail = nullptr;
			auto link = [&] (QueueSource *source) {
				if(tail)
					tail->link = source;
				tail = source;
			};

			for(size_t i = 0; i < closure->count; i++) {
				auto item = &closure->items[i];
				if(item->transmit.tag() == kTagOffer) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagAccept) {
					// TODO: This condition should be replaced. Just test if lane is valid.
					HelHandle handle = kHelNullHandle;
					if(item->transmit.error() == Error::success) {
						auto universe = closure->weakUniverse.grab();
						assert(universe);

						auto irq_lock = frg::guard(&irqMutex());
						Universe::Guard lock(universe->lock);

						handle = universe->attachDescriptor(lock,
								LaneDescriptor{item->transmit.lane()});
					}

					item->helHandleResult = {translateError(item->transmit.error()), 0, handle};
					item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagImbueCredentials) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagExtractCredentials) {
					item->helCredentialsResult = {translateError(item->transmit.error()), 0};
					memcpy(item->helCredentialsResult.credentials,
							item->transmit.credentials().data(), 16);
					item->mainSource.setup(&item->helCredentialsResult,
							sizeof(HelCredentialsResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagSendFromBuffer) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagRecvInline) {
					item->buffer = item->transmit.transmitBuffer();

					item->helInlineResult = {translateError(item->transmit.error()),
							0, item->buffer.size()};
					item->mainSource.setup(&item->helInlineResult, sizeof(HelInlineResultNoFlex));
					item->dataSource.setup(item->buffer.data(), item->buffer.size());
					link(&item->mainSource);
					link(&item->dataSource);
				}else if(item->transmit.tag() == kTagRecvToBuffer) {
					item->helLengthResult = {translateError(item->transmit.error()),
							0, item->transmit.actualLength()};
					item->mainSource.setup(&item->helLengthResult, sizeof(HelLengthResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagPushDescriptor) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagPullDescriptor) {
					// TODO: This condition should be replaced. Just test if lane is valid.
					HelHandle handle = kHelNullHandle;
					if(item->transmit.error() == Error::success) {
						auto universe = closure->weakUniverse.grab();
						assert(universe);

						auto irq_lock = frg::guard(&irqMutex());
						Universe::Guard lock(universe->lock);

						handle = universe->attachDescriptor(lock, item->transmit.descriptor());
					}

					item->helHandleResult = {translateError(item->transmit.error()), 0, handle};
					item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
					link(&item->mainSource);
				}else{
					panicLogger() << "thor: Unexpected transmission tag" << frg::endlog;
				}
			}

			assert(closure->count);
			closure->setupSource(&closure->items[0].mainSource);
			closure->ipcQueue->submit(closure);
		}

		void completePacket() override {
			transmitted(this);
		}

		void complete() override {
			// TODO: Turn items into a unique_ptr.
			frg::destruct_n(*kernelAlloc, items, count);
			frg::destruct(*kernelAlloc, this);
		}

		size_t count;
		frigg::WeakPtr<Universe> weakUniverse;
		frigg::SharedPtr<IpcQueue> ipcQueue;

		Item *items;
	} *closure = frg::construct<Closure>(*kernelAlloc);

	closure->count = count;
	closure->weakUniverse = this_universe.toWeak();
	closure->ipcQueue = std::move(queue);

	closure->setup(count);
	closure->setupContext(context);
	closure->items = frg::construct_n<Item>(*kernelAlloc, count);

	StreamList root_chain;
	frg::vector<StreamNode *, KernelAlloc> ancillary_stack(*kernelAlloc);

	// We use this as a marker that the root chain has not ended.
	ancillary_stack.push_back(nullptr);

	for(size_t i = 0; i < count; i++) {
		HelAction action;
		readUserObject(actions + i, action);

		// TODO: Turn this into an error return.
		assert(!ancillary_stack.empty() && "expected end of chain");

		switch(action.type) {
		case kHelActionOffer: {
			closure->items[i].transmit.setup(kTagOffer, closure);
		} break;
		case kHelActionAccept: {
			closure->items[i].transmit.setup(kTagAccept, closure);
		} break;
		case kHelActionImbueCredentials: {
			closure->items[i].transmit.setup(kTagImbueCredentials, closure);
			memcpy(closure->items[i].transmit._inCredentials.data(),
					this_thread->credentials(), 16);
		} break;
		case kHelActionExtractCredentials: {
			closure->items[i].transmit.setup(kTagExtractCredentials, closure);
		} break;
		case kHelActionSendFromBuffer: {
			frigg::UniqueMemory<KernelAlloc> buffer(*kernelAlloc, action.length);
			if(!readUserMemory(buffer.data(), action.buffer, action.length))
				return kHelErrFault;

			closure->items[i].transmit.setup(kTagSendFromBuffer, closure);
			closure->items[i].transmit._inBuffer = std::move(buffer);
		} break;
		case kHelActionSendFromBufferSg: {
			size_t length = 0;
			auto sglist = reinterpret_cast<HelSgItem *>(action.buffer);
			for(size_t j = 0; j < action.length; j++) {
				HelSgItem item;
				readUserObject(sglist + j, item);
				length += item.length;
			}

			frigg::UniqueMemory<KernelAlloc> buffer(*kernelAlloc, length);
			size_t offset = 0;
			for(size_t j = 0; j < action.length; j++) {
				HelSgItem item;
				readUserObject(sglist + j, item);
				if(!readUserMemory(reinterpret_cast<char *>(buffer.data()) + offset,
						reinterpret_cast<char *>(item.buffer), item.length))
					return kHelErrFault;
				offset += item.length;
			}

			closure->items[i].transmit.setup(kTagSendFromBuffer, closure);
			closure->items[i].transmit._inBuffer = std::move(buffer);
		} break;
		case kHelActionRecvInline: {
			// TODO: For now, we hardcode a size of 128 bytes.
			auto space = this_thread->getAddressSpace().lock();
			closure->items[i].transmit.setup(kTagRecvInline, closure);
			closure->items[i].transmit._maxLength = 128;
		} break;
		case kHelActionRecvToBuffer: {
			auto space = this_thread->getAddressSpace().lock();
			auto accessor = AddressSpaceLockHandle{std::move(space),
					action.buffer, action.length};
			Thread::asyncBlockCurrent(accessor.acquire());

			closure->items[i].transmit.setup(kTagRecvToBuffer, closure);
			closure->items[i].transmit._inAccessor = std::move(accessor);
		} break;
		case kHelActionPushDescriptor: {
			AnyDescriptor operand;
			{
				auto irq_lock = frg::guard(&irqMutex());
				Universe::Guard universe_guard(this_universe->lock);

				auto wrapper = this_universe->getDescriptor(universe_guard, action.handle);
				if(!wrapper)
					return kHelErrNoDescriptor;
				operand = *wrapper;
			}

			closure->items[i].transmit.setup(kTagPushDescriptor, closure);
			closure->items[i].transmit._inDescriptor = std::move(operand);
		} break;
		case kHelActionPullDescriptor: {
			closure->items[i].transmit.setup(kTagPullDescriptor, closure);
		} break;
		default:
			// TODO: Turn this into an error return.
			assert(!"Fix error handling here");
		}

		// Here, we make sure of our marker on the ancillary_stack.
		if(!ancillary_stack.back()) {
			// Add the item to the root list.
			root_chain.push_back(&closure->items[i].transmit);
		}else{
			// Add the item to an ancillary list.
			ancillary_stack.back()->ancillaryChain.push_back(&closure->items[i].transmit);
		}

		if(!(action.flags & kHelItemChain))
			ancillary_stack.pop();
		if(action.flags & kHelItemAncillary)
			ancillary_stack.push(&closure->items[i].transmit);
	}

	if(!ancillary_stack.empty())
		return kHelErrIllegalArgs;

	Stream::transmit(lane, root_chain);

	return kHelErrNone;
}

HelError helShutdownLane(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	LaneHandle lane;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<LaneDescriptor>())
			return kHelErrBadDescriptor;
		lane = wrapper->get<LaneDescriptor>().handle;
	}

	lane.getStream()->shutdownLane(lane.getLane());

	return kHelErrNone;
}

HelError helFutexWait(int *pointer, int expected, int64_t deadline) {
	auto thisThread = getCurrentThread();
	auto space = thisThread->getAddressSpace();

	auto condition = [&] () -> bool {
		enableUserAccess();
		unsigned int v;
		auto e = doAtomicUserLoad(&v, reinterpret_cast<unsigned int *>(pointer));
		disableUserAccess();
		if(e)
			return false;
		return expected == v;
	};

	if(deadline < 0) {
		if(deadline != -1)
			return kHelErrIllegalArgs;

		Thread::asyncBlockCurrent(
			space->futexSpace.wait(reinterpret_cast<uintptr_t>(pointer), condition)
		);
	}else{
		Thread::asyncBlockCurrent(
			async::race_and_cancel(
				[=] (async::cancellation_token cancellation) {
					return space->futexSpace.wait(
						reinterpret_cast<uintptr_t>(pointer), condition, cancellation);
				},
				[=] (async::cancellation_token cancellation) {
					return generalTimerEngine()->sleep(deadline, cancellation);
				}
			)
		);
	}

	return kHelErrNone;
}

HelError helFutexWake(int *pointer) {
	auto this_thread = getCurrentThread();
	auto space = this_thread->getAddressSpace();

	{
		// TODO: Support physical (i.e. non-private) futexes.
		space->futexSpace.wake(VirtualAddr(pointer));
	}

	return kHelErrNone;
}

HelError helCreateOneshotEvent(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto event = frigg::makeShared<OneshotEvent>(*kernelAlloc);

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				OneshotEventDescriptor(std::move(event)));
	}

	return kHelErrNone;
}

HelError helCreateBitsetEvent(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto event = frigg::makeShared<BitsetEvent>(*kernelAlloc);

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				BitsetEventDescriptor(std::move(event)));
	}

	return kHelErrNone;
}

HelError helRaiseEvent(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	AnyDescriptor descriptor;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		descriptor = *wrapper;
	}

	if(descriptor.is<OneshotEventDescriptor>()) {
		auto event = descriptor.get<OneshotEventDescriptor>().event;
		event->trigger();
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helAccessIrq(int number, HelHandle *handle) {
#ifdef __x86_64__
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto irq = frigg::makeShared<IrqObject>(*kernelAlloc,
			frg::string<KernelAlloc>{*kernelAlloc, "generic-irq-object"});
	IrqPin::attachSink(getGlobalSystemIrq(number), irq.get());

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				IrqDescriptor(std::move(irq)));
	}

	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence) {
	assert(!(flags & ~(kHelAckAcknowledge | kHelAckNack | kHelAckKick)));

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto mode = flags & (kHelAckAcknowledge | kHelAckNack | kHelAckKick);
	if(mode != kHelAckAcknowledge && mode != kHelAckNack && mode != kHelAckKick)
		return kHelErrIllegalArgs;

	frigg::SharedPtr<IrqObject> irq;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto irq_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq = irq_wrapper->get<IrqDescriptor>().irq;
	}

	Error error;
	if(mode == kHelAckAcknowledge) {
		error = IrqPin::ackSink(irq.get(), sequence);
	}else if(mode == kHelAckNack) {
		error = IrqPin::nackSink(irq.get(), sequence);
	}else{
 		assert(mode == kHelAckKick);
		error = IrqPin::kickSink(irq.get());
	}

	if(error == Error::illegalArgs) {
		return kHelErrIllegalArgs;
	}else{
		assert(error == Error::success);
		return kHelErrNone;
	}
}

HelError helSubmitAwaitEvent(HelHandle handle, uint64_t sequence,
		HelHandle queue_handle, uintptr_t context) {
	struct IrqClosure final : IpcNode {
		static void issue(frigg::SharedPtr<IrqObject> irq, uint64_t sequence,
				frigg::SharedPtr<IpcQueue> queue, intptr_t context) {
			auto closure = frg::construct<IrqClosure>(*kernelAlloc,
					std::move(queue), context);
			irq->submitAwait(&closure->irqNode, sequence);
		}

		static void awaited(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &IrqClosure::worklet);
			closure->result.error = translateError(closure->irqNode.error());
			closure->result.sequence = closure->irqNode.sequence();
			closure->_queue->submit(closure);
		}

	public:
		explicit IrqClosure(frigg::SharedPtr<IpcQueue> the_queue, uintptr_t context)
		: _queue{std::move(the_queue)},
				source{&result, sizeof(HelEventResult), nullptr} {
			memset(&result, 0, sizeof(HelEventResult));
			setupContext(context);
			setupSource(&source);
			worklet.setup(&IrqClosure::awaited, getCurrentThread()->mainWorkQueue());
			irqNode.setup(&worklet);
		}

		void complete() override {
			frg::destruct(*kernelAlloc, this);
		}

	private:
		Worklet worklet;
		AwaitIrqNode irqNode;
		frigg::SharedPtr<IpcQueue> _queue;
		QueueSource source;
		HelEventResult result;
	};

	struct EventClosure final : IpcNode {
		static void issue(frigg::SharedPtr<OneshotEvent> event, uint64_t sequence,
				frigg::SharedPtr<IpcQueue> queue, intptr_t context) {
			auto closure = frg::construct<EventClosure>(*kernelAlloc,
					std::move(queue), context);
			event->submitAwait(&closure->eventNode, sequence);
		}

		static void issue(frigg::SharedPtr<BitsetEvent> event, uint64_t sequence,
				frigg::SharedPtr<IpcQueue> queue, intptr_t context) {
			auto closure = frg::construct<EventClosure>(*kernelAlloc,
					std::move(queue), context);
			event->submitAwait(&closure->eventNode, sequence);
		}

		static void awaited(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &EventClosure::worklet);
			closure->result.error = translateError(closure->eventNode.error());
			closure->result.sequence = closure->eventNode.sequence();
			closure->result.bitset = closure->eventNode.bitset();
			closure->_queue->submit(closure);
		}

	public:
		explicit EventClosure(frigg::SharedPtr<IpcQueue> the_queue, uintptr_t context)
		: _queue{std::move(the_queue)},
				source{&result, sizeof(HelEventResult), nullptr} {
			memset(&result, 0, sizeof(HelEventResult));
			setupContext(context);
			setupSource(&source);
			worklet.setup(&EventClosure::awaited, getCurrentThread()->mainWorkQueue());
			eventNode.setup(&worklet);
		}

		void complete() override {
			frg::destruct(*kernelAlloc, this);
		}

	private:
		Worklet worklet;
		AwaitEventNode eventNode;
		frigg::SharedPtr<IpcQueue> _queue;
		QueueSource source;
		HelEventResult result;
	};

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IrqObject> irq;
	AnyDescriptor descriptor;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		descriptor = *wrapper;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelEventResult))))
		return kHelErrQueueTooSmall;

	if(descriptor.is<IrqDescriptor>()) {
		auto irq = descriptor.get<IrqDescriptor>().irq;
		IrqClosure::issue(std::move(irq), sequence,
				std::move(queue), context);
	}else if(descriptor.is<OneshotEventDescriptor>()) {
		auto event = descriptor.get<OneshotEventDescriptor>().event;
		EventClosure::issue(std::move(event), sequence,
				std::move(queue), context);
	}else if(descriptor.is<BitsetEventDescriptor>()) {
		auto event = descriptor.get<BitsetEventDescriptor>().event;
		EventClosure::issue(std::move(event), sequence,
				std::move(queue), context);
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helAutomateIrq(HelHandle handle, uint32_t flags, HelHandle kernlet_handle) {
	assert(!flags);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IrqObject> irq;
	frigg::SharedPtr<BoundKernlet> kernlet;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto irq_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq = irq_wrapper->get<IrqDescriptor>().irq;

		auto kernlet_wrapper = this_universe->getDescriptor(universe_guard, kernlet_handle);
		if(!kernlet_wrapper)
			return kHelErrNoDescriptor;
		if(!kernlet_wrapper->is<BoundKernletDescriptor>())
			return kHelErrBadDescriptor;
		kernlet = kernlet_wrapper->get<BoundKernletDescriptor>().boundKernlet;
	}

	irq->automate(std::move(kernlet));

	return kHelErrNone;
}

HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	// TODO: check userspace page access rights
	auto io_space = frigg::makeShared<IoSpace>(*kernelAlloc);
	for(size_t i = 0; i < num_ports; i++) {
		uintptr_t port;
		readUserObject<uintptr_t>(port_array + i, port);
		io_space->addPort(port);
	}

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				IoDescriptor(std::move(io_space)));
	}

	return kHelErrNone;
}

HelError helEnableIo(HelHandle handle) {
#ifdef __x86_64__
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IoSpace> io_space;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<IoDescriptor>())
			return kHelErrBadDescriptor;
		io_space = wrapper->get<IoDescriptor>().ioSpace;
	}

	io_space->enableInThread(this_thread);

	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helEnableFullIo() {
#ifdef __x86_64__
	auto this_thread = getCurrentThread();

	for(uintptr_t port = 0; port < 0x10000; port++)
		this_thread->getContext().enableIoPort(port);

	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helBindKernlet(HelHandle handle, const HelKernletData *data, size_t num_data,
		HelHandle *bound_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<KernletObject> kernlet;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto kernlet_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!kernlet_wrapper)
			return kHelErrNoDescriptor;
		if(!kernlet_wrapper->is<KernletObjectDescriptor>())
			return kHelErrBadDescriptor;
		kernlet = kernlet_wrapper->get<KernletObjectDescriptor>().kernletObject;
	}

	auto object = kernlet.get();
	assert(num_data == object->numberOfBindParameters());

	auto bound = frigg::makeShared<BoundKernlet>(*kernelAlloc,
			std::move(kernlet));
	for(size_t i = 0; i < object->numberOfBindParameters(); i++) {
		const auto &defn = object->defnOfBindParameter(i);

		HelKernletData d;
		if(!readUserObject(data + i, d))
			return kHelErrFault;

		if(defn.type == KernletParameterType::offset) {
			bound->setupOffsetBinding(i, d.handle);
		}else if(defn.type == KernletParameterType::memoryView) {
			frigg::SharedPtr<MemoryView> memory;
			{
				auto irq_lock = frg::guard(&irqMutex());
				Universe::Guard universe_guard(this_universe->lock);

				auto wrapper = this_universe->getDescriptor(universe_guard, d.handle);
				if(!wrapper)
					return kHelErrNoDescriptor;
				if(!wrapper->is<MemoryViewDescriptor>())
					return kHelErrBadDescriptor;
				memory = wrapper->get<MemoryViewDescriptor>().memory;
			}

			auto window = reinterpret_cast<char *>(KernelVirtualMemory::global().allocate(0x10000));
			assert(memory->getLength() <= 0x10000);

			for(size_t off = 0; off < memory->getLength(); off += kPageSize) {
				auto range = memory->peekRange(off);
				assert(range.get<0>() != PhysicalAddr(-1));
				KernelPageSpace::global().mapSingle4k(reinterpret_cast<uintptr_t>(window + off),
						range.get<0>(), page_access::write, range.get<1>());
			}

			bound->setupMemoryViewBinding(i, window);
		}else{
			assert(defn.type == KernletParameterType::bitsetEvent);

			frigg::SharedPtr<BitsetEvent> event;
			{
				auto irq_lock = frg::guard(&irqMutex());
				Universe::Guard universe_guard(this_universe->lock);

				auto wrapper = this_universe->getDescriptor(universe_guard, d.handle);
				if(!wrapper)
					return kHelErrNoDescriptor;
				if(!wrapper->is<BitsetEventDescriptor>())
					return kHelErrBadDescriptor;
				event = wrapper->get<BitsetEventDescriptor>().event;
			}

			bound->setupBitsetEventBinding(i, std::move(event));
		}
	}

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*bound_handle = this_universe->attachDescriptor(universe_guard,
				BoundKernletDescriptor(std::move(bound)));
	}

	return kHelErrNone;
}

HelError helSetAffinity(HelHandle thread, uint8_t *mask, size_t size) {
	if (thread != kHelThisThread)
		return kHelErrIllegalArgs;

	frg::vector<uint8_t, KernelAlloc> buf{*kernelAlloc};
	buf.resize(size);

	if (!readUserArray(mask, buf.data(), size))
		return kHelErrFault;

	size_t n = 0;
	for (auto i : buf) {
		n += __builtin_popcount(i);
	}

	// TODO: support allowing to run on multiple CPUs
	if (n != 1) {
		return kHelErrIllegalArgs;
	}

	auto this_thread = getCurrentThread();

	this_thread->setAffinityMask(std::move(buf));
	Thread::migrateCurrent();

	return kHelErrNone;
}

