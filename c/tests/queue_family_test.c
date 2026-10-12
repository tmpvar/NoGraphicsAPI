#include "shaders/queue_family_shared.h"
#include "test_support.h"

#define SLOT_BYTES 2048u
#define DATA_BYTES ((uint64_t)queue_test_width * queue_test_height * 4)
#define TIMESTAMP_OFFSET (3 * SLOT_BYTES)

typedef struct Fixture {
    GPUDevice* device;
    uint32_t queues[3];
    GPUCommandPool* pools[3];
    GPUTimelineSemaphore* signals[4];
    uint64_t value;
    GPUPSO* compute;
    GPUGpuHeap upload;
    GPUGpuHeap data;
    GPUGpuHeap readback;
    GPUTextureDescriptorHeap* descriptors;
    GPUTextureHeap textureHeap;
    GPUTexture* textures[2];
    GPURenderView* target;
} Fixture;

static bool initialize(Fixture* fixture) {
    const GPUByteSpan code = TestLoadShader(GPU_QUEUE_FAMILY_SPV);
    if (!code.data) return false;
    const GPUShaderStage stage = {.code = code, .entryPoint = "computeMain", .threadgroupSize = {8, 8, 1}};
    fixture->compute = GPUPSOCreateCompute(fixture->device, &stage);
    free((void*)code.data);
    if (!fixture->compute) return false;
    const GPUDeviceCaps* caps = GPUDeviceGetCaps(fixture->device);
    fixture->queues[0] = caps->copyQueueCount ? caps->generalQueueCount + caps->computeQueueCount : 0;
    fixture->queues[1] = caps->computeQueueCount ? caps->generalQueueCount : 0;
    for (uint32_t index = 0; index < 3; ++index)
        fixture->pools[index] = GPUCommandPoolCreate(fixture->device, fixture->queues[index]);
    for (uint32_t index = 0; index < 4; ++index)
        fixture->signals[index] = GPUTimelineSemaphoreCreate(fixture->device, 0);
    fixture->upload =
        GPUGpuHeapCreate(fixture->device, 2 * SLOT_BYTES + sizeof(QueueFamilyRoot), GPUMemoryType_CpuVisible);
    fixture->data = GPUGpuHeapCreate(fixture->device, 2 * SLOT_BYTES, GPUMemoryType_GpuOnly);
    fixture->readback = GPUGpuHeapCreate(fixture->device, 4 * SLOT_BYTES, GPUMemoryType_Readback);
    fixture->descriptors = GPUTextureDescriptorHeapCreate(fixture->device, 2);
    const GPUTextureDesc description = {
        .extent = {queue_test_width, queue_test_height, 1},
        .usage = GPUTextureUsage_Sampled | GPUTextureUsage_Storage | GPUTextureUsage_ColorAttachment |
                 GPUTextureUsage_TransferSource | GPUTextureUsage_TransferDestination,
    };
    const GPUSizeAlign placement = GPUTextureGetSizeAlign(fixture->device, &description);
    const uint64_t secondOffset = (placement.size + placement.align - 1) / placement.align * placement.align;
    fixture->textureHeap = GPUTextureHeapCreate(fixture->device, secondOffset + placement.size);
    GPUCommandBuffer* commands = GPUCmdBegin(fixture->pools[0]);
    fixture->textures[0] = GPUTextureCreate(commands, &description, &fixture->textureHeap, 0);
    fixture->textures[1] = GPUTextureCreate(commands, &description, &fixture->textureHeap, secondOffset);
    const GPURenderViewDesc viewDesc = {0};
    fixture->target = GPURenderViewCreate(fixture->textures[1], &viewDesc);
    const GPUTextureDescriptorDesc descriptorDesc = {0};
    GPUTextureDescriptorHeapWrite(
        fixture->descriptors,
        0,
        fixture->textures[0],
        GPUTextureDescriptorType_Sampled,
        &descriptorDesc
    );
    GPUTextureDescriptorHeapWrite(
        fixture->descriptors,
        1,
        fixture->textures[1],
        GPUTextureDescriptorType_Storage,
        &descriptorDesc
    );
    GPUCmdEnd(commands);
    GPUDeviceSubmit(
        fixture->device,
        &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = {fixture->signals[0], fixture->value}},
        fixture->queues[0]
    );
    GPUTimelineSemaphoreWait((GPUTimelinePoint){fixture->signals[0], fixture->value});
    return true;
}

static bool runCase(Fixture* fixture, uint32_t iteration) {
    ++fixture->value;
    for (uint32_t index = 0; index < 3; ++index) GPUCommandPoolReset(fixture->pools[index]);
    for (uint32_t index = 0; index < DATA_BYTES / 4; ++index) {
        ((uint32_t*)fixture->upload.range.cpu)[index] = iteration * 1000 + index;
        uint8_t* pixel = fixture->upload.range.cpu + SLOT_BYTES + index * 4;
        pixel[0] = (uint8_t)(index + iteration);
        pixel[1] = (uint8_t)(index * 3 + iteration);
        pixel[2] = (uint8_t)(index * 7 + iteration);
        pixel[3] = 255;
    }
    memset(fixture->readback.range.cpu, 0xa5, (size_t)fixture->readback.range.size);
    uint64_t* timestampCpu = (uint64_t*)(fixture->readback.range.cpu + TIMESTAMP_OFFSET);
    const GPUGpuRange uploadRange = GPUGpuRangeOfHeap(&fixture->upload);
    const GPUGpuRange dataRange = GPUGpuRangeOfHeap(&fixture->data);
    const GPUGpuRange readbackRange = GPUGpuRangeOfHeap(&fixture->readback);
    const GPUTextureCopyDesc wholeTexture = {0};
    GPUCommandBuffer* producer = GPUCmdBegin(fixture->pools[0]);
    GPUCmdWriteTimestamp(producer, timestampCpu, GPUStage_AllCommands);
    GPUCmdCopyMemory(producer, GPUGpuSubrange(uploadRange, 0, DATA_BYTES), GPUGpuSubrange(dataRange, 0, DATA_BYTES));
    GPUCmdCopyMemoryToTexture(
        producer,
        GPUGpuSubrange(uploadRange, SLOT_BYTES, DATA_BYTES),
        fixture->textures[0],
        &wholeTexture
    );
    GPUCmdWriteTimestamp(producer, timestampCpu + 1, GPUStage_AllCommands);
    GPUCmdEnd(producer);

    GPUCommandBuffer* compute = GPUCmdBegin(fixture->pools[1]);
    GPUCmdWriteTimestamp(compute, timestampCpu + 2, GPUStage_AllCommands);
    GPUCmdBindPSO(compute, fixture->compute);
    GPUCmdSetTextureDescriptorHeap(compute, fixture->descriptors);
    const QueueFamilyRoot root = {
        .source = (uint32_t*)fixture->data.range.gpu,
        .destination = (uint32_t*)(fixture->data.range.gpu + SLOT_BYTES),
        .source_texture = 0,
        .destination_texture = 1,
    };
    memcpy(fixture->upload.range.cpu + 2 * SLOT_BYTES, &root, sizeof(root));
    GPUCmdDispatch(
        compute,
        fixture->upload.range.gpu + 2 * SLOT_BYTES,
        (GPUExtent3){(queue_test_width + 7) / 8, (queue_test_height + 7) / 8, 1}
    );
    GPUCmdWriteTimestamp(compute, timestampCpu + 3, GPUStage_AllCommands);
    GPUCmdEnd(compute);

    GPUCommandBuffer* graphics = GPUCmdBegin(fixture->pools[2]);
    GPUCmdWriteTimestamp(graphics, timestampCpu + 4, GPUStage_AllCommands);
    GPUCmdCopyMemory(
        graphics,
        GPUGpuSubrange(dataRange, SLOT_BYTES, DATA_BYTES),
        GPUGpuSubrange(readbackRange, 0, DATA_BYTES)
    );
    GPUCmdCopyTextureToMemory(
        graphics,
        fixture->textures[1],
        GPUGpuSubrange(readbackRange, SLOT_BYTES, DATA_BYTES),
        &wholeTexture
    );
    GPUCmdBarrier(graphics, GPUStage_Transfer, GPUAccess_TransferRead, GPUStage_ColorOutput, GPUAccess_ColorWrite);
    const GPUColorAttachment color =
        {.renderView = fixture->target, .load = GPULoadOp_Clear, .clear = {.x = 1.0f, .w = 1.0f}};
    const GPURenderingDesc rendering = {.colors = {&color, 1}};
    GPUCmdBeginRenderPass(graphics, &rendering, GPURenderingFlags_None);
    GPUCmdEndRenderPass(graphics);
    GPUCmdWriteTimestamp(graphics, timestampCpu + 5, GPUStage_AllCommands);
    GPUCmdEnd(graphics);

    GPUCommandBuffer* readback = GPUCmdBegin(fixture->pools[0]);
    GPUCmdWriteTimestamp(readback, timestampCpu + 6, GPUStage_AllCommands);
    GPUCmdCopyTextureToMemory(
        readback,
        fixture->textures[1],
        GPUGpuSubrange(readbackRange, 2 * SLOT_BYTES, DATA_BYTES),
        &wholeTexture
    );
    GPUCmdBarrier(readback, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Host, GPUAccess_HostRead);
    GPUCmdWriteTimestamp(readback, timestampCpu + 7, GPUStage_AllCommands);
    GPUCmdEnd(readback);

    GPUCommandBuffer* commands[] = {producer, compute, graphics};
    const GPUTimelinePoint waits[] = {
        {.semaphore = fixture->signals[iteration == 1 ? 0 : 3], .value = fixture->value - 1},
        {.semaphore = fixture->signals[0], .value = fixture->value},
        {.semaphore = fixture->signals[1], .value = fixture->value},
    };
    const GPUSubmitDesc produced = {
        .commands = {commands, 1},
        .waits = {waits, 1},
        .completion = {.semaphore = fixture->signals[0], .value = fixture->value}
    };
    const GPUSubmitDesc computed = {
        .commands = {commands + 1, 1},
        .waits = {waits + 1, 1},
        .completion = {.semaphore = fixture->signals[1], .value = fixture->value}
    };
    const GPUSubmitDesc rendered = {
        .commands = {commands + 2, 1},
        .waits = {waits + 2, 1},
        .completion = {.semaphore = fixture->signals[2], .value = fixture->value}
    };
    // All three families can submit consumers before their producer without blocking the producer's queue.
    if (fixture->queues[0] != 0 && fixture->queues[1] != 0) {
        GPUDeviceSubmit(fixture->device, &rendered, 0);
        GPUDeviceSubmit(fixture->device, &computed, fixture->queues[1]);
        GPUDeviceSubmit(fixture->device, &produced, fixture->queues[0]);
    } else {
        GPUDeviceSubmit(fixture->device, &produced, fixture->queues[0]);
        GPUDeviceSubmit(fixture->device, &computed, fixture->queues[1]);
        GPUDeviceSubmit(fixture->device, &rendered, 0);
    }
    GPUDeviceSubmit(
        fixture->device,
        &(GPUSubmitDesc){
            .commands = GPUCommandBuffers(readback),
            .waits = GPUTimelinePoints({.semaphore = fixture->signals[2], .value = fixture->value}),
            .completion = {.semaphore = fixture->signals[3], .value = fixture->value}
        },
        fixture->queues[0]
    );
    GPUTimelineSemaphoreWait((GPUTimelinePoint){fixture->signals[3], fixture->value});
    for (uint32_t index = 0; index < 3; ++index) GPUCommandPoolReadTimestamps(fixture->pools[index]);

    bool valid = true;
    for (uint32_t index = 0; index < DATA_BYTES / 4; ++index) {
        valid = ((const uint32_t*)fixture->readback.range.cpu)[index] == (iteration * 1000 + index) * 3 + 7 && valid;
        const uint8_t* input = fixture->upload.range.cpu + SLOT_BYTES + index * 4;
        const uint8_t* pixel = fixture->readback.range.cpu + SLOT_BYTES + index * 4;
        valid = pixel[0] == input[2] && pixel[1] == input[1] && pixel[2] == input[0] && pixel[3] == input[3] && valid;
        const uint8_t* cleared = fixture->readback.range.cpu + 2 * SLOT_BYTES + index * 4;
        valid = cleared[0] == 255 && cleared[1] == 0 && cleared[2] == 0 && cleared[3] == 255 && valid;
    }
    const uint64_t* timestamps = (const uint64_t*)(fixture->readback.range.cpu + TIMESTAMP_OFFSET);
    for (uint32_t index = 0; index < 8; index += 2) {
        valid = timestamps[index] != 0xa5a5a5a5a5a5a5a5ull && timestamps[index + 1] != 0xa5a5a5a5a5a5a5a5ull &&
                timestamps[index + 1] >= timestamps[index] && valid;
    }
    for (uint64_t index = 0; index < fixture->readback.range.size; ++index) {
        if (index < TIMESTAMP_OFFSET ? index % SLOT_BYTES < DATA_BYTES
                                     : index < TIMESTAMP_OFFSET + 8 * sizeof(uint64_t))
            continue;
        valid = fixture->readback.range.cpu[index] == 0xa5 && valid;
    }
    if (!valid)
        fprintf(stderr, "Queue-family data, texture, timestamp, or guard check failed at iteration %u.\n", iteration);
    return valid;
}

static void shutdown(Fixture* fixture) {
    GPUDeviceWaitIdle(fixture->device);
    for (uint32_t index = 0; index < 3; ++index) GPUCommandPoolDestroy(fixture->pools[index]);
    for (uint32_t index = 0; index < 4; ++index) GPUTimelineSemaphoreDestroy(fixture->signals[index]);
    GPURenderViewDestroy(fixture->target);
    for (uint32_t index = 0; index < 2; ++index) GPUTextureDestroy(fixture->textures[index]);
    GPUTextureHeapDestroy(&fixture->textureHeap);
    GPUTextureDescriptorHeapDestroy(fixture->descriptors);
    GPUGpuHeapDestroy(&fixture->readback);
    GPUGpuHeapDestroy(&fixture->data);
    GPUGpuHeapDestroy(&fixture->upload);
    GPUPSODestroy(fixture->compute);
    TestDestroyDevice(fixture->device);
}

int main(int argc, char** argv) {
    const bool compute = argc == 1 || strcmp(argv[1], "--compute") == 0;
    const bool copy = argc == 1 || strcmp(argv[1], "--copy") == 0;
    const GPUDeviceDesc desc = {
        .desiredComputeQueueCount = compute ? 32u : 0u,
        .desiredCopyQueueCount = copy ? 32u : 0u,
        .timestampQueryCount = 2,
    };
    const TestDeviceInit initialized = TestCreateDevice(&desc);
    if (initialized.error != GPUError_None) {
        fprintf(stderr, "Dedicated queue device creation: %u.\n", (uint32_t)initialized.error);
        return initialized.error == GPUError_Unsupported ? TEST_SKIPPED : 1;
    }
    Fixture fixture = {.device = initialized.device, .value = 1};
    const GPUDeviceCaps* caps = GPUDeviceGetCaps(fixture.device);
    printf(
        "%s: %u general, %u compute, %u copy queues; copy granularity %u x %u x %u.\n",
        caps->deviceName,
        caps->generalQueueCount,
        caps->computeQueueCount,
        caps->copyQueueCount,
        caps->copyTextureGranularity.x,
        caps->copyTextureGranularity.y,
        caps->copyTextureGranularity.z
    );
    bool valid =
        caps->generalQueueCount == 1 && caps->queueCount == 1 + caps->computeQueueCount + caps->copyQueueCount &&
        (compute ? caps->computeQueueCount >= 1 && caps->computeQueueCount <= 32 : caps->computeQueueCount == 0) &&
        (copy ? caps->copyQueueCount >= 1 && caps->copyQueueCount <= 32 : caps->copyQueueCount == 0);
    valid = initialize(&fixture) && valid;
    if (valid)
        for (uint32_t iteration = 1; iteration <= 8; ++iteration) valid = runCase(&fixture, iteration) && valid;
    shutdown(&fixture);
    return valid ? 0 : 1;
}
