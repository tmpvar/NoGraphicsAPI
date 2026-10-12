#include "test_support.h"

#define THREAD_COUNT 4u
#define TEXTURES_PER_THREAD 8u
#define TEXTURE_BYTES (8u * 8u * 4u)
#define SENTINEL (~(uint64_t)0)

static GPUTextureDesc textureDesc(void) {
    const GPUTextureDesc desc = {
        .extent = {8, 8, 1},
        .usage = GPUTextureUsage_Sampled | GPUTextureUsage_ColorAttachment | GPUTextureUsage_TransferSource |
                 GPUTextureUsage_TransferDestination,
    };
    return desc;
}

typedef struct RecordingContext {
    GPUDevice* device;
    const GPUTextureHeap* textureHeap;
    uint64_t textureStride;
    uint32_t index;
    GPUCommandPool* pool;
    GPUCommandBuffer* uploadCommands;
    GPUCommandBuffer* readbackCommands;
    GPUTimelineSemaphore* timeline;
    GPUGpuHeap upload;
    GPUGpuHeap readback;
    GPUTextureDescriptorHeap* textureDescriptors;
    GPUSamplerDescriptorHeap* samplerDescriptors;
    GPUTexture* textures[TEXTURES_PER_THREAD];
    GPURenderView* views[TEXTURES_PER_THREAD];
} RecordingContext;

static void recordTextures(void* argument) {
    RecordingContext* context = argument;
    const GPUTextureDesc desc = textureDesc();
    context->pool = GPUCommandPoolCreate(context->device, 0);
    context->uploadCommands = GPUCmdBegin(context->pool);
    context->timeline = GPUTimelineSemaphoreCreate(context->device, 0);
    context->upload = GPUGpuHeapCreate(context->device, TEXTURES_PER_THREAD * TEXTURE_BYTES, GPUMemoryType_CpuVisible);
    context->readback = GPUGpuHeapCreate(
        context->device,
        TEXTURES_PER_THREAD * TEXTURE_BYTES + sizeof(uint64_t) * 2,
        GPUMemoryType_Readback
    );
    context->textureDescriptors = GPUTextureDescriptorHeapCreate(context->device, TEXTURES_PER_THREAD);
    context->samplerDescriptors = GPUSamplerDescriptorHeapCreate(context->device, TEXTURES_PER_THREAD);
    for (uint32_t index = 0; index != TEXTURES_PER_THREAD * TEXTURE_BYTES / sizeof(uint32_t); ++index)
        ((uint32_t*)context->upload.range.cpu)[index] = 0x12340000u + context->index * 0x1000u + index;
    memset(context->readback.range.cpu, 0xcd, TEXTURES_PER_THREAD * TEXTURE_BYTES);
    memset(context->readback.range.cpu + TEXTURES_PER_THREAD * TEXTURE_BYTES, 0xff, sizeof(uint64_t) * 2);

    const GPURenderViewDesc viewDesc = {0};
    const GPUTextureDescriptorDesc descriptorDesc = {0};
    const GPUSamplerDesc samplerDesc = {0};
    const GPUTextureCopyDesc copy = {0};
    for (uint32_t index = 0; index != TEXTURES_PER_THREAD; ++index) {
        context->textures[index] = GPUTextureCreate(
            context->uploadCommands,
            &desc,
            context->textureHeap,
            (context->index * TEXTURES_PER_THREAD + index) * context->textureStride
        );
        context->views[index] = GPURenderViewCreate(context->textures[index], &viewDesc);
        GPUTextureDescriptorHeapWrite(
            context->textureDescriptors,
            index,
            context->textures[index],
            GPUTextureDescriptorType_Sampled,
            &descriptorDesc
        );
        GPUSamplerDescriptorHeapWrite(context->samplerDescriptors, index, &samplerDesc);
        GPUCmdCopyMemoryToTexture(
            context->uploadCommands,
            GPUGpuSubrange(GPUGpuRangeOfHeap(&context->upload), index * TEXTURE_BYTES, TEXTURE_BYTES),
            context->textures[index],
            &copy
        );
    }
    GPUCmdWriteTimestamp(
        context->uploadCommands,
        (uint64_t*)(context->readback.range.cpu + TEXTURES_PER_THREAD * TEXTURE_BYTES),
        GPUStage_AllCommands
    );
    GPUCmdEnd(context->uploadCommands);
    context->readbackCommands = GPUCmdBegin(context->pool);
    for (uint32_t index = 0; index != TEXTURES_PER_THREAD; ++index) {
        GPUCmdCopyTextureToMemory(
            context->readbackCommands,
            context->textures[index],
            GPUGpuSubrange(GPUGpuRangeOfHeap(&context->readback), index * TEXTURE_BYTES, TEXTURE_BYTES),
            &copy
        );
    }
    GPUCmdBarrier(
        context->readbackCommands,
        GPUStage_Transfer,
        GPUAccess_TransferWrite,
        GPUStage_Host,
        GPUAccess_HostRead
    );
    GPUCmdWriteTimestamp(
        context->readbackCommands,
        (uint64_t*)(context->readback.range.cpu + TEXTURES_PER_THREAD * TEXTURE_BYTES) + 1,
        GPUStage_AllCommands
    );
    GPUCmdEnd(context->readbackCommands);
}

static void destroyRecordingResources(RecordingContext* context) {
    GPUCommandPoolDestroy(context->pool);
    for (uint32_t index = 0; index != TEXTURES_PER_THREAD; ++index) {
        GPURenderViewDestroy(context->views[index]);
        GPUTextureDestroy(context->textures[index]);
    }
    GPUSamplerDescriptorHeapDestroy(context->samplerDescriptors);
    GPUTextureDescriptorHeapDestroy(context->textureDescriptors);
    GPUGpuHeapDestroy(&context->readback);
    GPUGpuHeapDestroy(&context->upload);
    GPUTimelineSemaphoreDestroy(context->timeline);
}

static void waitForSubmission(void* argument) { GPUTimelineSemaphoreWait(*(const GPUTimelinePoint*)argument); }

static bool testParallelRecording(GPUDevice* device) {
    const GPUTextureDesc desc = textureDesc();
    const GPUSizeAlign requirements = GPUTextureGetSizeAlign(device, &desc);
    const uint64_t textureStride =
        (requirements.size + requirements.align - 1) / requirements.align * requirements.align;
    const GPUTextureHeap textureHeap = GPUTextureHeapCreate(device, THREAD_COUNT * TEXTURES_PER_THREAD * textureStride);
    RecordingContext contexts[THREAD_COUNT];
    TestThread threads[THREAD_COUNT];
    for (uint32_t index = 0; index != THREAD_COUNT; ++index) {
        contexts[index] = (RecordingContext){
            .device = device,
            .textureHeap = &textureHeap,
            .textureStride = textureStride,
            .index = index
        };
        threads[index] = (TestThread){.run = recordTextures, .argument = contexts + index};
    }
    bool valid = TestRunThreads(threads, THREAD_COUNT);
    if (valid) {
        GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
        GPUCommandBuffer* heldCommands = GPUCmdBegin(pool);
        const GPUTimelinePoint completion = {.semaphore = contexts[THREAD_COUNT - 1].timeline, .value = 3};
        GPUTimelinePoint waiterCompletion = {.semaphore = contexts[0].timeline, .value = 2};
        TestThread waiter = {.run = waitForSubmission, .argument = &waiterCompletion};
        const bool waiterStarted = TestThreadStart(&waiter);
        valid = waiterStarted;
        for (uint32_t index = 0; index != THREAD_COUNT; ++index) {
            // Submit independent ended buffers while heldCommands is still recording.
            GPUDeviceSubmit(
                device,
                &(GPUSubmitDesc){
                    .commands = GPUCommandBuffers(contexts[index].uploadCommands),
                    .completion = {contexts[index].timeline, 1}
                },
                0
            );
            GPUDeviceSubmit(
                device,
                &(GPUSubmitDesc){
                    .commands = GPUCommandBuffers(contexts[index].readbackCommands),
                    .waits = GPUTimelinePoints({.semaphore = contexts[index].timeline, .value = 1}),
                    .completion = {contexts[index].timeline, 2}
                },
                0
            );
        }
        GPUCmdEnd(heldCommands);
        GPUDeviceSubmit(
            device,
            &(GPUSubmitDesc){.commands = GPUCommandBuffers(heldCommands), .completion = completion},
            0
        );
        if (waiterStarted) valid = TestThreadJoin(&waiter) && valid;
        GPUTimelineSemaphoreWait(completion);
        GPUCommandPoolDestroy(pool);
        for (uint32_t index = 0; index != THREAD_COUNT; ++index) {
            GPUTimelineSemaphoreWait((GPUTimelinePoint){.semaphore = contexts[index].timeline, .value = 2});
            GPUCommandPoolReadTimestamps(contexts[index].pool);
            const uint64_t* timestamps =
                (const uint64_t*)(contexts[index].readback.range.cpu + TEXTURES_PER_THREAD * TEXTURE_BYTES);
            if (timestamps[0] == SENTINEL || timestamps[1] == SENTINEL || timestamps[0] > timestamps[1]) {
                fprintf(stderr, "Worker %u timestamp readback failed.\n", index);
                valid = false;
            }
            const uint32_t* uploaded = (const uint32_t*)contexts[index].upload.range.cpu;
            const uint32_t* received = (const uint32_t*)contexts[index].readback.range.cpu;
            for (uint32_t word = 0; word != TEXTURES_PER_THREAD * TEXTURE_BYTES / sizeof(uint32_t); ++word) {
                if (uploaded[word] == received[word]) continue;
                fprintf(
                    stderr,
                    "Worker %u texture %llu word %u: expected %08x, received %08x.\n",
                    index,
                    (unsigned long long)(word * sizeof(uint32_t) / TEXTURE_BYTES),
                    word,
                    uploaded[word],
                    received[word]
                );
                fprintf(
                    stderr,
                    "Readback words 0=%08x 64=%08x last=%08x.\n",
                    received[0],
                    received[64],
                    received[TEXTURES_PER_THREAD * TEXTURE_BYTES / sizeof(uint32_t) - 1]
                );
                valid = false;
                break;
            }
        }
    }
    for (uint32_t index = 0; index != THREAD_COUNT; ++index) destroyRecordingResources(&contexts[index]);
    GPUTextureHeapDestroy(&textureHeap);
    return valid;
}

typedef struct QueueContext {
    GPUDevice* device;
    uint32_t index;
    bool valid;
} QueueContext;

static void submitCopies(void* argument) {
    QueueContext* context = argument;
    GPUCommandPool* pool = GPUCommandPoolCreate(context->device, context->index);
    GPUTimelineSemaphore* timeline = GPUTimelineSemaphoreCreate(context->device, 0);
    const GPUGpuHeap upload = GPUGpuHeapCreate(context->device, TEXTURE_BYTES, GPUMemoryType_CpuVisible);
    const GPUGpuHeap scratch = GPUGpuHeapCreate(context->device, TEXTURE_BYTES, GPUMemoryType_GpuOnly);
    const GPUGpuHeap readback =
        GPUGpuHeapCreate(context->device, TEXTURE_BYTES + sizeof(uint64_t), GPUMemoryType_Readback);
    uint64_t* timestamp = (uint64_t*)(readback.range.cpu + TEXTURE_BYTES);
    for (uint32_t iteration = 1; iteration <= 32; ++iteration) {
        for (uint32_t index = 0; index != TEXTURE_BYTES / sizeof(uint32_t); ++index)
            ((uint32_t*)upload.range.cpu)[index] = context->index * 0x10000u + iteration * 0x100u + index;
        *timestamp = SENTINEL;
        GPUCommandBuffer* commands = GPUCmdBegin(pool);
        GPUCmdCopyMemory(commands, GPUGpuRangeOfHeap(&upload), GPUGpuRangeOfHeap(&scratch));
        GPUCmdBarrier(commands, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Transfer, GPUAccess_TransferRead);
        GPUCmdCopyMemory(
            commands,
            GPUGpuRangeOfHeap(&scratch),
            GPUGpuSubrange(GPUGpuRangeOfHeap(&readback), 0, TEXTURE_BYTES)
        );
        GPUCmdBarrier(commands, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Host, GPUAccess_HostRead);
        GPUCmdWriteTimestamp(commands, timestamp, GPUStage_AllCommands);
        GPUCmdEnd(commands);
        GPUDeviceSubmit(
            context->device,
            &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = {timeline, iteration}},
            context->index
        );
        GPUTimelineSemaphoreWait((GPUTimelinePoint){timeline, iteration});
        GPUCommandPoolReadTimestamps(pool);
        context->valid = memcmp(upload.range.cpu, readback.range.cpu, TEXTURE_BYTES) == 0 && context->valid;
        context->valid = *timestamp != SENTINEL && context->valid;
        GPUCommandPoolReset(pool);
    }
    GPUCommandPoolDestroy(pool);
    GPUGpuHeapDestroy(&readback);
    GPUGpuHeapDestroy(&scratch);
    GPUGpuHeapDestroy(&upload);
    GPUTimelineSemaphoreDestroy(timeline);
}

static bool testMultipleQueues(GPUDevice* device, bool queueFamilies) {
    QueueContext contexts[3] = {
        {.device = device, .index = 0, .valid = true},
        {.device = device, .index = 1, .valid = true},
        {.device = device, .index = 2, .valid = true},
    };
    TestThread threads[3] = {
        {.run = submitCopies, .argument = contexts},
        {.run = submitCopies, .argument = contexts + 1},
        {.run = submitCopies, .argument = contexts + 2}
    };
    bool valid = TestRunThreads(threads, queueFamilies ? 3u : 2u);
    for (uint32_t index = 0; index < 3; ++index) valid = contexts[index].valid && valid;

    GPUCommandPool* producerPool = GPUCommandPoolCreate(device, 0);
    GPUCommandPool* consumerPool = GPUCommandPoolCreate(device, 1);
    GPUTimelineSemaphore* produced = GPUTimelineSemaphoreCreate(device, 0);
    GPUTimelineSemaphore* consumed = GPUTimelineSemaphoreCreate(device, 0);
    const GPUGpuHeap upload = GPUGpuHeapCreate(device, TEXTURE_BYTES, GPUMemoryType_CpuVisible);
    const GPUGpuHeap scratch = GPUGpuHeapCreate(device, TEXTURE_BYTES, GPUMemoryType_GpuOnly);
    const GPUGpuHeap readback = GPUGpuHeapCreate(device, TEXTURE_BYTES, GPUMemoryType_Readback);
    for (uint32_t index = 0; index != TEXTURE_BYTES / sizeof(uint32_t); ++index)
        ((uint32_t*)upload.range.cpu)[index] = 0xcafe0000u + index;
    memset(readback.range.cpu, 0, TEXTURE_BYTES);
    GPUCommandBuffer* producer = GPUCmdBegin(producerPool);
    GPUCmdCopyMemory(producer, GPUGpuRangeOfHeap(&upload), GPUGpuRangeOfHeap(&scratch));
    GPUCmdEnd(producer);
    GPUCommandBuffer* consumer = GPUCmdBegin(consumerPool);
    GPUCmdCopyMemory(consumer, GPUGpuRangeOfHeap(&scratch), GPUGpuRangeOfHeap(&readback));
    GPUCmdBarrier(consumer, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Host, GPUAccess_HostRead);
    GPUCmdEnd(consumer);
    // Submit the consumer first: its cross-queue wait supplies the memory dependency.
    GPUDeviceSubmit(
        device,
        &(GPUSubmitDesc){
            .commands = GPUCommandBuffers(consumer),
            .waits = GPUTimelinePoints({.semaphore = produced, .value = 1}),
            .completion = {consumed, 1}
        },
        1
    );
    GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = GPUCommandBuffers(producer), .completion = {produced, 1}}, 0);
    GPUTimelineSemaphoreWait((GPUTimelinePoint){consumed, 1});
    GPUTimelineSemaphoreWait((GPUTimelinePoint){produced, 1});
    valid = memcmp(upload.range.cpu, readback.range.cpu, TEXTURE_BYTES) == 0 && valid;
    GPUCommandPoolDestroy(consumerPool);
    GPUCommandPoolDestroy(producerPool);
    GPUGpuHeapDestroy(&readback);
    GPUGpuHeapDestroy(&scratch);
    GPUGpuHeapDestroy(&upload);
    GPUTimelineSemaphoreDestroy(consumed);
    GPUTimelineSemaphoreDestroy(produced);
    return valid;
}

int main(int argc, char** argv) {
    const bool queueFamilies = argc == 2 && strcmp(argv[1], "--queue-families") == 0;
    const bool multipleQueues = queueFamilies || (argc == 2 && strcmp(argv[1], "--multiple-queues") == 0);
    GPUDeviceDesc desc = {.timestampQueryCount = 256};
    desc.desiredQueueCount = queueFamilies ? 1u : 32u;
    desc.desiredComputeQueueCount = queueFamilies ? 1u : 0u;
    desc.desiredCopyQueueCount = queueFamilies ? 1u : 0u;
    GPUDevice* device = TestRequireDevice(&desc);
    const uint32_t queueCount = GPUDeviceGetCaps(device)->queueCount;
    printf("Device exposes %u queue(s).\n", queueCount);
    if (multipleQueues && queueCount < 2) {
        fprintf(
            stderr,
            "Multiple-queue test skipped: device exposes %u compatible queue(s); parallel recording is tested "
            "separately.\n",
            queueCount
        );
        GPUDeviceWaitIdle(device);
        TestDestroyDevice(device);
        return TEST_SKIPPED;
    }
    const bool valid = queueCount >= 1 && queueCount <= 32 &&
                       (multipleQueues ? testMultipleQueues(device, queueFamilies) : testParallelRecording(device));
    GPUDeviceWaitIdle(device);
    TestDestroyDevice(device);
    if (!valid) fprintf(stderr, "%s test failed.\n", multipleQueues ? "Multiple-queue" : "Parallel recording");
    return valid ? 0 : 1;
}
