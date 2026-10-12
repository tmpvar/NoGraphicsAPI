#include "test_support.h"

#define BATCH_COMMAND_COUNT 40u
#define BATCH_SUBMISSION_COUNT 4u
#define TEST_GPU_HEAP_SIZE (1024u * 1024u)
#define SENTINEL (~(uint64_t)0)

static uint64_t elementCount(uint64_t byteCount, uint64_t elementSize) { return 1 + (byteCount - 1) / elementSize; }

static bool validSizeAlign(GPUSizeAlign sizeAlign, uint64_t commonAlignment) {
    return sizeAlign.size != 0 && sizeAlign.align != 0 && (sizeAlign.align & (sizeAlign.align - 1)) == 0 &&
           commonAlignment >= sizeAlign.align && commonAlignment % sizeAlign.align == 0;
}

static bool testGpuHeaps(GPUDevice* device) {
    const uint64_t sizes[] = {1, 15, 16, 17, TEST_GPU_HEAP_SIZE};
    GPUGpuHeap heaps[GPUArrayCount(sizes)];
    bool valid = true;
    for (size_t index = 0; index < GPUArrayCount(sizes); ++index) {
        heaps[index] = GPUGpuHeapCreate(device, sizes[index], GPUMemoryType_CpuVisible);
        const GPUGpuRange range = GPUGpuRangeOfHeap(&heaps[index]);
        const GPUGpuRange nestedRange = GPUGpuRangeOf(heaps[index].range);
        valid = valid && heaps[index].range.cpu && heaps[index].range.gpu && heaps[index].range.size == sizes[index] &&
                heaps[index].owner && (uintptr_t)heaps[index].range.cpu % 16 == 0 &&
                (uintptr_t)heaps[index].range.gpu % 16 == 0 && range.gpu == heaps[index].range.gpu &&
                range.size == heaps[index].range.size && nestedRange.gpu == range.gpu && nestedRange.size == range.size;
    }
    const GPUGpuHeap readback = GPUGpuHeapCreate(device, 1, GPUMemoryType_Readback);
    const GPUGpuHeap gpuOnly = GPUGpuHeapCreate(device, 1, GPUMemoryType_GpuOnly);
    valid = valid && readback.range.cpu && readback.range.gpu && (uintptr_t)readback.range.cpu % 16 == 0 &&
            (uintptr_t)readback.range.gpu % 16 == 0 && readback.range.size == 1 && readback.owner &&
            !gpuOnly.range.cpu && gpuOnly.range.gpu && (uintptr_t)gpuOnly.range.gpu % 16 == 0 &&
            gpuOnly.range.size == 1 && gpuOnly.owner;
    for (size_t index = 0; index < GPUArrayCount(sizes); ++index) GPUGpuHeapDestroy(&heaps[index]);
    GPUGpuHeapDestroy(&readback);
    GPUGpuHeapDestroy(&gpuOnly);
    return valid;
}

static bool testDescriptorHeaps(GPUDevice* device, GPUTimelineSemaphore* timeline, uint64_t* nextTimelineValue) {
    GPUTextureDescriptorHeap* textureHeap = GPUTextureDescriptorHeapCreate(device, 2);
    GPUSamplerDescriptorHeap* samplerHeap = GPUSamplerDescriptorHeapCreate(device, 2);
    const GPUSamplerDesc sampler = {
        .minFilter = GPUFilter_Nearest,
        .mipFilter = GPUFilter_Nearest,
        .addressU = GPUAddressMode_ClampToEdge,
        .addressW = GPUAddressMode_ClampToEdge,
        .anisotropic = true,
        .compareEnabled = true,
        .compare = GPUCompareOp_GreaterEqual,
    };
    GPUSamplerDescriptorHeapWrite(samplerHeap, 0, &sampler);

    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    GPUCommandBuffer* commands = GPUCmdBegin(pool);
    GPUSamplerDescriptorHeapCopy(samplerHeap, 0, samplerHeap, 1, 1);
    GPUCmdSetTextureDescriptorHeap(commands, textureHeap);
    GPUCmdSetSamplerDescriptorHeap(commands, samplerHeap);
    GPUCmdSetViewport(
        commands,
        &(GPUViewport){.x = 1.0f, .y = 2.0f, .width = 3.0f, .height = 4.0f, .minDepth = 0.25f, .maxDepth = 0.75f}
    );
    GPUCmdSetScissor(commands, &(GPUScissor){.x = 5, .y = 6, .width = 7, .height = 8});
    const GPUDepthStencilState depthStencil = {
        .depthTest = true,
        .depthWrite = true,
        .depthCompare = GPUCompareOp_GreaterEqual,
        .stencilTest = true,
        .stencilReadMask = 0x7f,
        .stencilWriteMask = 0x3f,
        .front =
            {
                .compare = GPUCompareOp_Equal,
                .fail = GPUStencilOp_Replace,
                .pass = GPUStencilOp_IncrementWrap,
                .depthFail = GPUStencilOp_DecrementClamp,
                .reference = 11,
            },
        .back = {
            .compare = GPUCompareOp_NotEqual,
            .fail = GPUStencilOp_Zero,
            .pass = GPUStencilOp_Invert,
            .depthFail = GPUStencilOp_IncrementClamp,
            .reference = 17,
        },
    };
    GPUCmdSetDepthStencil(commands, &depthStencil);
    const GPUTimelinePoint completion = {.semaphore = timeline, .value = ++*nextTimelineValue};
    GPUCmdEnd(commands);
    GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = completion}, 0);
    GPUTimelineSemaphoreWait(completion);
    GPUCommandPoolDestroy(pool);
    GPUTextureDescriptorHeapDestroy(textureHeap);
    GPUSamplerDescriptorHeapDestroy(samplerHeap);
    return true;
}

static bool testBatchGrowthAndReuse(GPUDevice* device, GPUTimelineSemaphore* timeline, uint64_t* nextTimelineValue) {
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    GPUCommandBuffer* highWaterCommands[BATCH_COMMAND_COUNT] = {NULL};
    bool valid = true;
    for (uint32_t submission = 0; submission < BATCH_SUBMISSION_COUNT; ++submission) {
        GPUCommandBuffer* commands[BATCH_COMMAND_COUNT] = {NULL};
        for (size_t index = 0; index < BATCH_COMMAND_COUNT; ++index) {
            commands[index] = GPUCmdBegin(pool);
            if (submission == 0)
                highWaterCommands[index] = commands[index];
            else if (submission == 1)
                valid = valid && commands[index] == highWaterCommands[index];
            GPUCmdEnd(commands[index]);
        }

        const GPUTimelinePoint completion = {.semaphore = timeline, .value = ++*nextTimelineValue};
        GPUDeviceSubmit(
            device,
            &(GPUSubmitDesc){.commands = {commands, BATCH_COMMAND_COUNT}, .completion = completion},
            0
        );
        GPUTimelineSemaphoreWait(completion);
        GPUCommandPoolReset(pool);
    }
    GPUCommandPoolDestroy(pool);
    return valid;
}

#define CONTEXT_COUNT 3u
#define SLOT_WORDS 514u
#define WORD_COUNT (CONTEXT_COUNT * SLOT_WORDS)

static bool testTimestampReadback(GPUDevice* device, GPUTimelineSemaphore* timeline, uint64_t* nextTimelineValue) {
    static const uint32_t counts[4][CONTEXT_COUNT] = {{256, 4, 17}, {1, 0, 3}, {9, 256, 2}, {0, 2, 1}};
    static uint64_t cpu[WORD_COUNT];
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    uint64_t secondCpu[4];
    uint64_t discarded = SENTINEL;
    uint64_t previousBatchEnd = 0;
    bool valid = true;
    for (uint32_t batch = 0; batch < 4; ++batch) {
        for (uint32_t word = 0; word < WORD_COUNT; ++word) cpu[word] = SENTINEL;
        GPUCommandBuffer* unsubmitted = GPUCmdBegin(pool);
        GPUCmdWriteTimestamp(unsubmitted, &discarded, GPUStage_AllCommands);
        if (batch & 1u) GPUCmdEnd(unsubmitted);
        GPUCommandBuffer* submitted[CONTEXT_COUNT] = {NULL};
        const uint32_t first = 1 + (batch & 1u);
        for (uint32_t context = 0; context < CONTEXT_COUNT; ++context) {
            GPUCommandBuffer* commands = GPUCmdBegin(pool);
            submitted[CONTEXT_COUNT - context - 1] = commands;
            const uint32_t stride = 1 + ((batch + context) & 1u);
            for (uint32_t index = 0; index < counts[batch][context]; ++index)
                GPUCmdWriteTimestamp(
                    commands,
                    cpu + context * SLOT_WORDS + first + index * stride,
                    GPUStage_AllCommands
                );
            GPUCmdEnd(commands);
        }
        // All contexts remain recorded together, and execute in the opposite order. No caller query object or host
        // barrier is needed.
        const GPUTimelinePoint completion = {.semaphore = timeline, .value = ++*nextTimelineValue};
        GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = {submitted, CONTEXT_COUNT}, .completion = completion}, 0);
        GPUTimelineSemaphoreWait(completion);
        bool batchValid = discarded == SENTINEL;
        for (uint32_t word = 0; word < WORD_COUNT; ++word) batchValid = cpu[word] == SENTINEL && batchValid;
        GPUCommandPoolReadTimestamps(pool);
        batchValid = discarded == SENTINEL && batchValid;
        for (uint32_t word = 0; word < WORD_COUNT; ++word) {
            const uint32_t context = word / SLOT_WORDS;
            const uint32_t offset = word % SLOT_WORDS;
            const uint32_t stride = 1 + ((batch + context) & 1u);
            const bool written =
                offset >= first && (offset - first) % stride == 0 && (offset - first) / stride < counts[batch][context];
            batchValid = batchValid && (written ? cpu[word] != SENTINEL : cpu[word] == SENTINEL);
        }
        uint64_t firstTick = 0;
        uint64_t previousTick = 0;
        bool haveTick = false;
        for (uint32_t order = 0; order < CONTEXT_COUNT; ++order) {
            const uint32_t context = CONTEXT_COUNT - order - 1;
            const uint32_t stride = 1 + ((batch + context) & 1u);
            for (uint32_t index = 0; index < counts[batch][context]; ++index) {
                const uint64_t tick = cpu[context * SLOT_WORDS + first + index * stride];
                if (haveTick)
                    batchValid = batchValid && tick >= previousTick;
                else
                    firstTick = tick;
                previousTick = tick;
                haveTick = true;
            }
        }
        batchValid = batchValid && previousTick > firstTick && (batch == 0 || firstTick != previousBatchEnd);
        previousBatchEnd = previousTick;
        if (!batchValid) fprintf(stderr, "Timestamp readback failed in batch %u.\n", batch);
        valid = valid && batchValid;
        for (uint32_t word = 0; word < WORD_COUNT; ++word) cpu[word] = SENTINEL;
        GPUCommandPoolReadTimestamps(pool);
        for (uint32_t word = 0; word < WORD_COUNT; ++word) valid = cpu[word] == SENTINEL && valid;
        GPUCommandPoolReset(pool);
    }
    for (uint32_t word = 0; word < WORD_COUNT; ++word) cpu[word] = SENTINEL;
    for (uint32_t word = 0; word < 4; ++word) secondCpu[word] = SENTINEL;
    GPUCommandBuffer* commands = GPUCmdBegin(pool);
    GPUCmdWriteTimestamp(commands, cpu, GPUStage_AllCommands);
    GPUCmdWriteTimestamp(commands, secondCpu, GPUStage_AllCommands);
    GPUCmdWriteTimestamp(commands, cpu + 1, GPUStage_AllCommands);
    const GPUTimelinePoint completion = {.semaphore = timeline, .value = ++*nextTimelineValue};
    GPUCmdEnd(commands);
    GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = completion}, 0);
    GPUTimelineSemaphoreWait(completion);
    GPUCommandPoolReadTimestamps(pool);
    bool separateDestinationsValid = cpu[0] != SENTINEL && secondCpu[0] != SENTINEL && cpu[1] != SENTINEL &&
                                     cpu[0] <= secondCpu[0] && secondCpu[0] <= cpu[1];
    for (uint32_t word = 2; word < WORD_COUNT; ++word)
        separateDestinationsValid = separateDestinationsValid && cpu[word] == SENTINEL;
    for (uint32_t word = 1; word < 4; ++word)
        separateDestinationsValid = separateDestinationsValid && secondCpu[word] == SENTINEL;
    if (!separateDestinationsValid) fprintf(stderr, "Timestamp readback failed across separate CPU arrays.\n");
    valid = valid && separateDestinationsValid;
    GPUCommandPoolReset(pool);
    commands = GPUCmdBegin(pool);
    GPUCmdWriteTimestamp(commands, &discarded, GPUStage_AllCommands);
    GPUCmdEnd(commands);
    const GPUTimelinePoint unreadCompletion = {.semaphore = timeline, .value = ++*nextTimelineValue};
    GPUDeviceSubmit(
        device,
        &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = unreadCompletion},
        0
    );
    GPUTimelineSemaphoreWait(unreadCompletion);
    GPUCommandPoolReset(pool);
    GPUCommandPoolReadTimestamps(pool);
    valid = discarded == SENTINEL && valid;
    GPUCommandPoolDestroy(pool);
    return valid;
}

static bool testTimestampCapacity(uint32_t count) {
    // Timestamp storage is reserved for every command buffer of every pool, so size the device for the single pool and
    // buffer this test records.
    const GPUDeviceDesc desc = {
        .timestampQueryCount = count,
        .limits = {.maxCommandPools = 1, .maxCommandBuffersPerPool = 1},
    };
    const TestDeviceInit initialized = TestCreateDevice(&desc);
    if (initialized.error != GPUError_None) return false;
    GPUDevice* device = initialized.device;
    GPUTimelineSemaphore* timeline = GPUTimelineSemaphoreCreate(device, 0);
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    uint64_t* cpu = malloc(sizeof(uint64_t) * (count + 2));
    uint64_t previousResult = 0;
    bool valid = true;
    for (uint32_t batch = 0; batch < 4; ++batch) {
        for (uint32_t index = 0; index < count + 2; ++index) cpu[index] = SENTINEL;
        GPUCommandBuffer* commands = GPUCmdBegin(pool);
        for (uint32_t index = 0; index < count; ++index)
            GPUCmdWriteTimestamp(commands, cpu + index + 1, GPUStage_AllCommands);
        const GPUTimelinePoint completion = {.semaphore = timeline, .value = batch + 1};
        GPUCmdEnd(commands);
        GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = completion}, 0);
        GPUTimelineSemaphoreWait(completion);
        GPUCommandPoolReadTimestamps(pool);
        GPUCommandPoolReset(pool);
        bool batchValid = cpu[0] == SENTINEL && cpu[count + 1] == SENTINEL && (batch == 0 || cpu[1] != previousResult);
        for (uint32_t index = 1; index <= count; ++index)
            batchValid = batchValid && cpu[index] != SENTINEL && (index == 1 || cpu[index] >= cpu[index - 1]);
        previousResult = cpu[1];
        if (!batchValid) fprintf(stderr, "Timestamp capacity %u failed in batch %u.\n", count, batch);
        valid = valid && batchValid;
        GPUDeviceWaitIdle(device);
    }
    GPUCommandPoolDestroy(pool);
    free(cpu);
    GPUTimelineSemaphoreDestroy(timeline);
    TestDestroyDevice(device);
    return valid;
}

static bool testWithoutTimestamps(uint32_t queryCount) {
    const GPUDeviceDesc desc = {.timestampQueryCount = queryCount};
    const TestDeviceInit initialized = TestCreateDevice(&desc);
    if (initialized.error != GPUError_None) return false;
    GPUDevice* device = initialized.device;
    GPUTimelineSemaphore* timeline = GPUTimelineSemaphoreCreate(device, 0);
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    const GPUGpuHeap source =
        GPUGpuHeapCreate(device, sizeof(uint64_t) * BATCH_COMMAND_COUNT, GPUMemoryType_CpuVisible);
    const GPUGpuHeap readback =
        GPUGpuHeapCreate(device, sizeof(uint64_t) * (BATCH_COMMAND_COUNT * 2 + 1), GPUMemoryType_Readback);
    uint64_t* sourceCpu = (uint64_t*)source.range.cpu;
    uint64_t* readbackCpu = (uint64_t*)readback.range.cpu;
    GPUCommandBuffer* firstCommands[BATCH_COMMAND_COUNT] = {NULL};
    bool valid = true;
    for (uint32_t batch = 0; batch < BATCH_SUBMISSION_COUNT; ++batch) {
        for (size_t index = 0; index < BATCH_COMMAND_COUNT; ++index)
            sourceCpu[index] = (uint64_t)(batch + 1) * 1000 + index;
        for (size_t index = 0; index < BATCH_COMMAND_COUNT * 2 + 1; ++index) readbackCpu[index] = SENTINEL;
        GPUCommandBuffer* commands[BATCH_COMMAND_COUNT] = {NULL};
        bool batchValid = true;
        for (size_t index = 0; index < BATCH_COMMAND_COUNT; ++index) {
            commands[index] = GPUCmdBegin(pool);
            if (batch == 0)
                firstCommands[index] = commands[index];
            else
                batchValid = batchValid && commands[index] == firstCommands[index];
            GPUCmdWriteTimestamp(commands[index], readbackCpu + index * 2, GPUStage_AllCommands);
            GPUCmdCopyMemory(
                commands[index],
                GPUGpuSubrange(GPUGpuRangeOfHeap(&source), index * sizeof(uint64_t), sizeof(uint64_t)),
                GPUGpuSubrange(GPUGpuRangeOfHeap(&readback), (index * 2 + 1) * sizeof(uint64_t), sizeof(uint64_t))
            );
            GPUCmdWriteTimestamp(commands[index], readbackCpu + index * 2 + 2, GPUStage_AllCommands);
            GPUCmdBarrier(
                commands[index],
                GPUStage_Transfer,
                GPUAccess_TransferWrite,
                GPUStage_Host,
                GPUAccess_HostRead
            );
            GPUCmdEnd(commands[index]);
        }
        const GPUTimelinePoint completion = {.semaphore = timeline, .value = batch + 1};
        GPUDeviceSubmit(
            device,
            &(GPUSubmitDesc){.commands = {commands, BATCH_COMMAND_COUNT}, .completion = completion},
            0
        );
        GPUTimelineSemaphoreWait(completion);
        GPUCommandPoolReadTimestamps(pool);
        GPUCommandPoolReset(pool);
        for (size_t index = 0; index < BATCH_COMMAND_COUNT; ++index)
            batchValid =
                batchValid && readbackCpu[index * 2] == SENTINEL && readbackCpu[index * 2 + 1] == sourceCpu[index];
        batchValid = batchValid && readbackCpu[BATCH_COMMAND_COUNT * 2] == SENTINEL;
        if (!batchValid) fprintf(stderr, "Command readback with timestamps disabled failed in batch %u.\n", batch);
        valid = valid && batchValid;
        GPUDeviceWaitIdle(device);
    }
    GPUCommandPoolDestroy(pool);
    GPUGpuHeapDestroy(&readback);
    GPUGpuHeapDestroy(&source);
    GPUTimelineSemaphoreDestroy(timeline);
    TestDestroyDevice(device);
    return valid;
}

static GPUFormat combinedDepthStencilFormat(GPUDevice* device) {
    const GPUTextureUsage usage = GPUTextureUsage_DepthStencilAttachment;
    if (GPUDeviceSupportsTextureFormat(device, GPUFormat_D24UnormS8Uint, usage)) return GPUFormat_D24UnormS8Uint;
    if (GPUDeviceSupportsTextureFormat(device, GPUFormat_D32FloatS8Uint, usage)) return GPUFormat_D32FloatS8Uint;
    return GPUFormat_Undefined;
}

static void recordAttachmentSubresourcePasses(
    GPUCommandBuffer* commands,
    GPURenderView* colorView,
    GPURenderView* depthStencilView
) {
    if (colorView) {
        const GPUColorAttachment attachment =
            {.renderView = colorView, .load = GPULoadOp_Clear, .clear = {.x = 0.1f, .y = 0.2f, .z = 0.3f, .w = 1.0f}};
        const GPURenderingDesc rendering = {.colors = {&attachment, 1}};
        GPUCmdBeginRenderPass(commands, &rendering, GPURenderingFlags_None);
        GPUCmdEndRenderPass(commands);
    }
    if (depthStencilView) {
        const GPURenderingDesc rendering = {
            .depth = {.renderView = depthStencilView, .load = GPULoadOp_Clear, .clear = 0.5f},
            .stencil = {.renderView = depthStencilView, .load = GPULoadOp_Clear, .clear = 7},
        };
        GPUCmdBeginRenderPass(commands, &rendering, GPURenderingFlags_None);
        GPUCmdEndRenderPass(commands);
    }
}

static bool testPlacedTextures(
    GPUDevice* device,
    const GPUDeviceCaps* caps,
    GPUTimelineSemaphore* timeline,
    uint64_t* nextTimelineValue
) {
    const uint64_t textureHeapAlignment = caps->textureHeapAlignment;
    const GPUTextureDesc firstDesc = {.extent = {17, 9, 1}, .usage = GPUTextureUsage_Sampled};
    const GPUTextureDesc secondDesc = {.extent = {31, 15, 1}, .mipLevels = 3, .usage = GPUTextureUsage_Sampled};
    const GPUSizeAlign firstSizeAlign = GPUTextureGetSizeAlign(device, &firstDesc);
    const GPUSizeAlign secondSizeAlign = GPUTextureGetSizeAlign(device, &secondDesc);
    if (!validSizeAlign(firstSizeAlign, textureHeapAlignment) || !validSizeAlign(secondSizeAlign, textureHeapAlignment))
        return false;

    uint64_t heapElements = elementCount(firstSizeAlign.size, textureHeapAlignment);
    const uint64_t secondOffset = heapElements * textureHeapAlignment;
    heapElements += elementCount(secondSizeAlign.size, textureHeapAlignment);

    const GPUTextureUsage broadUsage =
        GPUTextureUsage_Sampled | GPUTextureUsage_Storage | GPUTextureUsage_TransferDestination;
    const bool hasBroad3d = GPUDeviceSupportsTextureFormat(device, GPUFormat_Rgba32Float, broadUsage);
    const GPUTextureDesc broad3dDesc = {
        .type = GPUTextureType_3D,
        .extent = {16, 8, 4},
        .mipLevels = 4,
        .format = GPUFormat_Rgba32Float,
        .usage = broadUsage,
    };
    GPUSizeAlign broad3dSizeAlign = {0, 0};
    uint64_t broad3dOffset = 0;
    if (hasBroad3d) {
        broad3dSizeAlign = GPUTextureGetSizeAlign(device, &broad3dDesc);
        if (!validSizeAlign(broad3dSizeAlign, textureHeapAlignment)) return false;
        broad3dOffset = heapElements * textureHeapAlignment;
        heapElements += elementCount(broad3dSizeAlign.size, textureHeapAlignment);
    }

    const GPUTextureDesc mutableDesc = {
        .extent = {19, 11, 1},
        .mipLevels = 3,
        .mutableFormat = true,
        .usage = GPUTextureUsage_Sampled,
    };
    const GPUSizeAlign mutableSizeAlign = GPUTextureGetSizeAlign(device, &mutableDesc);
    if (!validSizeAlign(mutableSizeAlign, textureHeapAlignment)) return false;
    const uint64_t mutableOffset = heapElements * textureHeapAlignment;
    heapElements += elementCount(mutableSizeAlign.size, textureHeapAlignment);

    GPUFormat compressedFormat = GPUFormat_Undefined;
    if (caps->textureCompressionBc &&
        GPUDeviceSupportsTextureFormat(device, GPUFormat_Bc7Unorm, GPUTextureUsage_Sampled))
        compressedFormat = GPUFormat_Bc7Unorm;
    else if (
        caps->textureCompressionAstc &&
        GPUDeviceSupportsTextureFormat(device, GPUFormat_Astc4x4Unorm, GPUTextureUsage_Sampled)
    )
        compressedFormat = GPUFormat_Astc4x4Unorm;
    const bool hasCompressed = compressedFormat != GPUFormat_Undefined;
    const GPUTextureDesc compressedDesc = {
        .extent = {20, 12, 1},
        .mipLevels = 4,
        .format = compressedFormat,
        .usage = GPUTextureUsage_Sampled,
    };
    GPUSizeAlign compressedSizeAlign = {0, 0};
    uint64_t compressedOffset = 0;
    if (hasCompressed) {
        compressedSizeAlign = GPUTextureGetSizeAlign(device, &compressedDesc);
        if (!validSizeAlign(compressedSizeAlign, textureHeapAlignment)) return false;
        compressedOffset = heapElements * textureHeapAlignment;
        heapElements += elementCount(compressedSizeAlign.size, textureHeapAlignment);
    }

    const GPUTextureUsage colorUsage = GPUTextureUsage_ColorAttachment;
    const bool hasColor = GPUDeviceSupportsTextureFormat(device, GPUFormat_Rgba8Unorm, colorUsage);
    const GPUTextureDesc colorDesc = {
        .type = GPUTextureType_Cube,
        .extent = {8, 8, 1},
        .mipLevels = 3,
        .layerCount = 6,
        .usage = colorUsage,
    };
    GPUSizeAlign colorSizeAlign = {0, 0};
    uint64_t colorOffset = 0;
    if (hasColor) {
        colorSizeAlign = GPUTextureGetSizeAlign(device, &colorDesc);
        if (!validSizeAlign(colorSizeAlign, textureHeapAlignment)) return false;
        colorOffset = heapElements * textureHeapAlignment;
        heapElements += elementCount(colorSizeAlign.size, textureHeapAlignment);
    }

    const GPUFormat depthStencilFormat = combinedDepthStencilFormat(device);
    const bool hasDepthStencil = depthStencilFormat != GPUFormat_Undefined;
    const GPUTextureUsage sampledDepthStencilUsage = GPUTextureUsage_Sampled | GPUTextureUsage_DepthStencilAttachment;
    const bool sampledDepthStencil =
        hasDepthStencil && GPUDeviceSupportsTextureFormat(device, depthStencilFormat, sampledDepthStencilUsage);
    const GPUTextureDesc depthStencilDesc = {
        .type = GPUTextureType_2DArray,
        .extent = {8, 8, 1},
        .mipLevels = 3,
        .layerCount = 2,
        .format = depthStencilFormat,
        .usage = sampledDepthStencil ? sampledDepthStencilUsage : GPUTextureUsage_DepthStencilAttachment,
    };
    GPUSizeAlign depthStencilSizeAlign = {0, 0};
    uint64_t depthStencilOffset = 0;
    if (hasDepthStencil) {
        depthStencilSizeAlign = GPUTextureGetSizeAlign(device, &depthStencilDesc);
        if (!validSizeAlign(depthStencilSizeAlign, textureHeapAlignment)) return false;
        depthStencilOffset = heapElements * textureHeapAlignment;
        heapElements += elementCount(depthStencilSizeAlign.size, textureHeapAlignment);
    }

    const uint64_t heapSize = heapElements * textureHeapAlignment;
    const GPUTextureHeap textureHeap = GPUTextureHeapCreate(device, heapSize);
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    GPUCommandBuffer* commands = GPUCmdBegin(pool);
    GPUTexture* first = GPUTextureCreate(commands, &firstDesc, &textureHeap, 0);
    GPUTexture* second = GPUTextureCreate(commands, &secondDesc, &textureHeap, secondOffset);
    GPUTexture* broad3d = hasBroad3d ? GPUTextureCreate(commands, &broad3dDesc, &textureHeap, broad3dOffset) : NULL;
    GPUTexture* mutableTexture = GPUTextureCreate(commands, &mutableDesc, &textureHeap, mutableOffset);
    GPUTexture* compressed =
        hasCompressed ? GPUTextureCreate(commands, &compressedDesc, &textureHeap, compressedOffset) : NULL;
    GPUTexture* color = hasColor ? GPUTextureCreate(commands, &colorDesc, &textureHeap, colorOffset) : NULL;
    GPUTexture* depthStencil =
        hasDepthStencil ? GPUTextureCreate(commands, &depthStencilDesc, &textureHeap, depthStencilOffset) : NULL;
#if !defined(GPU_DEBUG)
    if (sampledDepthStencil) {
        GPUTextureDescriptorHeap* descriptorHeap = GPUTextureDescriptorHeapCreate(device, 1);
        GPUTextureDescriptorDesc descriptorDesc = {0};
        GPUTextureDescriptorHeapWrite(
            descriptorHeap,
            0,
            depthStencil,
            GPUTextureDescriptorType_Sampled,
            &descriptorDesc
        );
        descriptorDesc.aspect = GPUTextureAspect_Stencil;
        GPUTextureDescriptorHeapWrite(
            descriptorHeap,
            0,
            depthStencil,
            GPUTextureDescriptorType_Sampled,
            &descriptorDesc
        );
        GPUTextureDescriptorHeapDestroy(descriptorHeap);
    }
#endif
    const GPUTextureHeap recordingHeap = GPUTextureHeapCreate(device, firstSizeAlign.size);
    GPURenderView* colorRenderView =
        color ? GPURenderViewCreate(color, &(GPURenderViewDesc){.mipLevel = 2, .slice = 5}) : NULL;
    GPURenderView* depthStencilRenderView =
        depthStencil ? GPURenderViewCreate(depthStencil, &(GPURenderViewDesc){.mipLevel = 1, .slice = 1}) : NULL;
    recordAttachmentSubresourcePasses(commands, colorRenderView, depthStencilRenderView);
    const GPUTimelinePoint completion = {.semaphore = timeline, .value = ++*nextTimelineValue};
    GPUCmdEnd(commands);
    GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = completion}, 0);
    GPUTimelineSemaphoreWait(completion);
    GPUCommandPoolDestroy(pool);
    if (depthStencilRenderView) GPURenderViewDestroy(depthStencilRenderView);
    if (colorRenderView) GPURenderViewDestroy(colorRenderView);
    if (depthStencil) GPUTextureDestroy(depthStencil);
    if (color) GPUTextureDestroy(color);
    if (compressed) GPUTextureDestroy(compressed);
    GPUTextureDestroy(mutableTexture);
    if (broad3d) GPUTextureDestroy(broad3d);
    GPUTextureDestroy(second);
    GPUTextureDestroy(first);
    GPUTextureHeapDestroy(&recordingHeap);
    GPUTextureHeapDestroy(&textureHeap);
    return true;
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--timestamps-unavailable") == 0) return testWithoutTimestamps(256) ? 0 : 1;

    const GPUDeviceDesc desc = {.timestampQueryCount = 256};
    GPUDevice* device = TestRequireDevice(&desc);
    const GPUDeviceCaps* caps = GPUDeviceGetCaps(device);
    GPUTimelineSemaphore* timeline = GPUTimelineSemaphoreCreate(device, 0);
    uint64_t nextTimelineValue = 0;
    const bool valid = caps->textureHeapAlignment != 0 &&
                       (caps->textureHeapAlignment & (caps->textureHeapAlignment - 1)) == 0 && testGpuHeaps(device) &&
                       testDescriptorHeaps(device, timeline, &nextTimelineValue) &&
                       testPlacedTextures(device, caps, timeline, &nextTimelineValue) &&
                       testTimestampReadback(device, timeline, &nextTimelineValue) &&
                       testBatchGrowthAndReuse(device, timeline, &nextTimelineValue);
    GPUDeviceWaitIdle(device);
    GPUTimelineSemaphoreDestroy(timeline);
    TestDestroyDevice(device);
    return valid && testTimestampCapacity(1) && testTimestampCapacity(513) && testTimestampCapacity(4097) &&
                   testWithoutTimestamps(0)
               ? 0
               : 1;
}
