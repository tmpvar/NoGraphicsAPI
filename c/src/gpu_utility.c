#include "gpu_api.h"
#include "gpu_internal.h"

static uint8_t* GPU_UtilOffsetPointer(uint8_t* pointer, uint64_t offset) { return pointer ? pointer + offset : NULL; }

GPUBumpAllocator GPUBumpAllocatorCreate(GPUGpuCpuRange storage) {
    GPUAssert(storage.size != 0, "storage.size != 0");
    GPUAssert(storage.cpu || storage.gpu, "storage.cpu || storage.gpu");
    GPUAssert(
        (!storage.cpu || (size_t)storage.cpu % GPU_ALLOCATION_ALIGNMENT == 0) &&
            (!storage.gpu || (size_t)storage.gpu % GPU_ALLOCATION_ALIGNMENT == 0),
        "(!storage.cpu || (size_t)storage.cpu % GPU_ALLOCATION_ALIGNMENT == 0) && (!storage.gpu || (size_t)storage.gpu "
        "% GPU_ALLOCATION_ALIGNMENT == 0)"
    );
    return (GPUBumpAllocator){.storage = storage, .offset = 0};
}

GPUGpuCpuRange GPUBumpAllocatorAllocate(GPUBumpAllocator* allocator, uint64_t byteSize) {
    GPUAssert(byteSize != 0, "byteSize != 0");
    if (byteSize > allocator->storage.size - allocator->offset) return (GPUGpuCpuRange){0};

    const GPUGpuCpuRange allocation = {
        .cpu = GPU_UtilOffsetPointer(allocator->storage.cpu, allocator->offset),
        .gpu = GPU_UtilOffsetPointer(allocator->storage.gpu, allocator->offset),
        .size = byteSize,
    };
    const uint64_t remaining = allocator->storage.size - allocator->offset;
    const uint64_t alignedSize = GPU_AlignUp(byteSize, GPU_ALLOCATION_ALIGNMENT);
    allocator->offset += alignedSize < remaining ? alignedSize : remaining;
    return allocation;
}

GPUGpuCpuRange GPUBumpAllocatorAllocateAtomic(GPUBumpAllocator* allocator, uint64_t byteSize) {
    GPUAssert(byteSize != 0, "byteSize != 0");
    uint64_t allocationOffset = GPU_AtomicLoadU64(&allocator->offset);
    for (;;) {
        if (byteSize > allocator->storage.size - allocationOffset) return (GPUGpuCpuRange){0};

        const uint64_t remaining = allocator->storage.size - allocationOffset;
        const uint64_t alignedSize = GPU_AlignUp(byteSize, GPU_ALLOCATION_ALIGNMENT);
        const uint64_t nextOffset = allocationOffset + (alignedSize < remaining ? alignedSize : remaining);
        if (GPU_AtomicCompareExchangeU64(&allocator->offset, &allocationOffset, nextOffset)) {
            return (GPUGpuCpuRange){
                .cpu = GPU_UtilOffsetPointer(allocator->storage.cpu, allocationOffset),
                .gpu = GPU_UtilOffsetPointer(allocator->storage.gpu, allocationOffset),
                .size = byteSize,
            };
        }
    }
}

void GPUBumpAllocatorReset(GPUBumpAllocator* allocator) { allocator->offset = 0; }

GPUDeleteQueue GPUDeleteQueueCreate(GPUTimelineSemaphore* timeline, GPUDeleteEntry* entries, uint32_t capacity) {
    GPUAssert(timeline, "timeline");
    GPUAssert(entries, "entries");
    GPUAssert(capacity != 0, "capacity != 0");
    return (GPUDeleteQueue){
        .timeline = timeline,
        .entries = entries,
        .capacity = capacity,
        .head = 0,
        .tail = 0,
        .count = 0,
        .ticking = false
    };
}

void GPUDeleteQueueDefer(GPUDeleteQueue* queue, uint64_t retireValue, GPUDeleteCallback callback, void* user) {
    GPUAssert(callback, "callback");
    GPUAssert(queue->count < queue->capacity, "delete queue capacity exhausted");
    GPUAssert(
        queue->count == 0 ||
            queue->entries[queue->tail == 0 ? queue->capacity - 1 : queue->tail - 1].retireValue <= retireValue,
        "delete queue retire values must be nondecreasing"
    );

    queue->entries[queue->tail] = (GPUDeleteEntry){.retireValue = retireValue, .callback = callback, .user = user};
    queue->tail = queue->tail + 1 == queue->capacity ? 0 : queue->tail + 1;
    ++queue->count;
}

// Callbacks cannot recursively collect the same queue.
static void GPU_UtilDeleteQueueCollect(GPUDeleteQueue* queue, uint64_t completedValue) {
    GPUAssert(!queue->ticking, "delete queue callbacks cannot recursively collect the same queue");
    queue->ticking = true;
    while (queue->count != 0) {
        GPUDeleteEntry* entry = &queue->entries[queue->head];
        if (entry->retireValue > completedValue) break;

        entry->callback(entry->user);
        entry->callback = NULL;
        entry->user = NULL;
        queue->head = queue->head + 1 == queue->capacity ? 0 : queue->head + 1;
        --queue->count;
    }
    queue->ticking = false;
}

void GPUDeleteQueueTick(GPUDeleteQueue* queue) {
    if (queue->count == 0) return;

    GPUAssert(queue->timeline, "queue->timeline");
    GPU_UtilDeleteQueueCollect(queue, GPUTimelineSemaphoreGetCompletedValue(queue->timeline));
}

void GPUDeleteQueueDrain(GPUDeleteQueue* queue) { GPU_UtilDeleteQueueCollect(queue, ~0ull); }
