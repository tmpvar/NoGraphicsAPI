#include "test_support.h"

#define SEGMENT_COUNT 3u
#define WIDTH (SEGMENT_COUNT * 8)
#define HEIGHT 8u
#define PIXEL_BYTES ((uint64_t)WIDTH * HEIGHT * 4)
#define TIMESTAMP_BYTES (SEGMENT_COUNT * 2 * sizeof(uint64_t))
#define SENTINEL (~(uint64_t)0)

typedef struct Worker {
    GPUCommandPool* pool;
    GPUCommandBuffer* commands;
    GPUPSO* pso;
    const GPURenderingDesc* rendering;
    uint64_t* timestamps;
    uint32_t index;
    uint32_t iteration;
} Worker;

typedef struct Fixture {
    GPUDevice* device;
    GPUCommandPool* readbackPool;
    GPUPSO* pso;
    GPUTextureHeap textureHeap;
    GPUTexture* target;
    GPURenderView* view;
    GPUGpuHeap readback;
    GPUTimelinePoint completion;
    uint64_t previousTimestamp;
    Worker workers[SEGMENT_COUNT];
} Fixture;

static void drawSegment(GPUCommandBuffer* commands, const Worker* worker) {
    GPURenderingFlags flags = GPURenderingFlags_None;
    if (worker->index != 0) flags |= GPURenderingFlags_Resuming;
    if (worker->index != SEGMENT_COUNT - 1) flags |= GPURenderingFlags_Suspending;
    GPUCmdBeginRenderPass(commands, worker->rendering, flags);
    if ((worker->iteration & 1u) == 0) GPUCmdWriteTimestamp(commands, worker->timestamps, GPUStage_AllCommands);
    GPUCmdBindPSO(commands, worker->pso);
    GPUCmdSetScissor(
        commands,
        &(
            GPUScissor
        ){.x = (int32_t)(worker->index * 8 + (worker->iteration & 1u) * 4), .y = 0, .width = 4, .height = HEIGHT}
    );
    GPUCmdDraw(commands, NULL, 3, 1, 0, 0);
    if ((worker->iteration & 1u) == 0) GPUCmdWriteTimestamp(commands, worker->timestamps + 1, GPUStage_AllCommands);
    GPUCmdEndRenderPass(commands);
}

static void recordSegment(void* argument) {
    Worker* worker = argument;
    GPUCommandPoolReset(worker->pool);
    worker->commands = GPUCmdBegin(worker->pool);
    if (worker->index == 0)
        GPUCmdBarrier(
            worker->commands,
            GPUStage_Transfer,
            GPUAccess_TransferRead,
            GPUStage_ColorOutput,
            GPUAccess_ColorWrite
        );
    drawSegment(worker->commands, worker);
    GPUCmdEnd(worker->commands);
}

static bool recordWorkers(Worker* workers) {
    TestThread threads[SEGMENT_COUNT];
    for (uint32_t index = 0; index < SEGMENT_COUNT; ++index)
        threads[index] = (TestThread){.run = recordSegment, .argument = workers + index};
    const bool valid = TestRunThreads(threads, SEGMENT_COUNT);
    if (!valid) fprintf(stderr, "Render continuation worker creation or join failed.\n");
    return valid;
}

static bool initialize(Fixture* fixture) {
    const GPUByteSpan vertex = TestLoadShader(GPU_CONTINUATION_VERTEX_SPV);
    const GPUByteSpan fragment = TestLoadShader(GPU_CONTINUATION_FRAGMENT_SPV);
    if (!vertex.data || !fragment.data) {
        free((void*)vertex.data);
        free((void*)fragment.data);
        return false;
    }
    const GPUColorTargetDesc target = {.format = GPUFormat_Rgba8Unorm};
    const GPUGraphicsPSODesc psoDesc = {
        .vertex = {.code = vertex, .entryPoint = "vertexMain"},
        .fragment = {.code = fragment, .entryPoint = "fragmentMain"},
        .colorTargets = {&target, 1},
    };
    fixture->pso = GPUPSOCreateGraphics(fixture->device, &psoDesc);
    free((void*)vertex.data);
    free((void*)fragment.data);
    if (!fixture->pso) return false;
    fixture->readbackPool = GPUCommandPoolCreate(fixture->device, 0);
    if (!fixture->readbackPool) return false;
    for (uint32_t index = 0; index < SEGMENT_COUNT; ++index) {
        fixture->workers[index].pool = GPUCommandPoolCreate(fixture->device, 0);
        if (!fixture->workers[index].pool) return false;
    }
    fixture->completion.semaphore = GPUTimelineSemaphoreCreate(fixture->device, 0);
    if (!fixture->completion.semaphore) return false;
    fixture->readback = GPUGpuHeapCreate(fixture->device, PIXEL_BYTES + TIMESTAMP_BYTES, GPUMemoryType_Readback);
    const GPUTextureDesc targetDesc = {
        .extent = {WIDTH, HEIGHT, 1},
        .usage = GPUTextureUsage_ColorAttachment | GPUTextureUsage_TransferSource,
    };
    fixture->textureHeap =
        GPUTextureHeapCreate(fixture->device, GPUTextureGetSizeAlign(fixture->device, &targetDesc).size);
    GPUCommandBuffer* commands = GPUCmdBegin(fixture->readbackPool);
    fixture->target = GPUTextureCreate(commands, &targetDesc, &fixture->textureHeap, 0);
    if (!fixture->target) return false;
    const GPURenderViewDesc viewDesc = {0};
    fixture->view = GPURenderViewCreate(fixture->target, &viewDesc);
    if (!fixture->view) return false;
    GPUCmdEnd(commands);
    ++fixture->completion.value;
    GPUDeviceSubmit(
        fixture->device,
        &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = fixture->completion},
        0
    );
    GPUTimelineSemaphoreWait(fixture->completion);
    return true;
}

static bool renderIteration(Fixture* fixture, uint32_t iteration) {
    memset(fixture->readback.range.cpu, 0xff, PIXEL_BYTES + TIMESTAMP_BYTES);
    const GPUColorAttachment color = {.renderView = fixture->view, .load = GPULoadOp_Clear, .clear = {.w = 1.0f}};
    GPURenderingDesc rendering = {.colors = {&color, 1}};
    for (uint32_t index = 0; index < SEGMENT_COUNT; ++index) {
        fixture->workers[index].pso = fixture->pso;
        fixture->workers[index].rendering = &rendering;
        fixture->workers[index].timestamps = (uint64_t*)(fixture->readback.range.cpu + PIXEL_BYTES) + index * 2;
        fixture->workers[index].index = index;
        fixture->workers[index].iteration = iteration;
    }
    if (iteration < 4) {
        if (!recordWorkers(fixture->workers)) return false;
    } else {
        GPUCommandPoolReset(fixture->workers[0].pool);
        fixture->workers[0].commands = GPUCmdBegin(fixture->workers[0].pool);
        GPUCmdBarrier(
            fixture->workers[0].commands,
            GPUStage_Transfer,
            GPUAccess_TransferRead,
            GPUStage_ColorOutput,
            GPUAccess_ColorWrite
        );
        for (uint32_t index = 0; index < SEGMENT_COUNT; ++index)
            drawSegment(fixture->workers[0].commands, &fixture->workers[index]);
        GPUCmdEnd(fixture->workers[0].commands);
    }
    GPUCommandPoolReset(fixture->readbackPool);
    GPUCommandBuffer* readback = GPUCmdBegin(fixture->readbackPool);
    GPUCmdBarrier(readback, GPUStage_ColorOutput, GPUAccess_ColorWrite, GPUStage_Transfer, GPUAccess_TransferRead);
    const GPUTextureCopyDesc copy = {0};
    GPUCmdCopyTextureToMemory(
        readback,
        fixture->target,
        GPUGpuSubrange(GPUGpuRangeOfHeap(&fixture->readback), 0, PIXEL_BYTES),
        &copy
    );
    GPUCmdBarrier(readback, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Host, GPUAccess_HostRead);
    GPUCmdEnd(readback);
    ++fixture->completion.value;
    GPUCommandBuffer* commands[SEGMENT_COUNT + 1] =
        {fixture->workers[0].commands, fixture->workers[1].commands, fixture->workers[2].commands, readback};
    if (iteration >= 4) commands[1] = readback;
    GPUDeviceSubmit(
        fixture->device,
        &(
            GPUSubmitDesc
        ){.commands = {commands, iteration < 4 ? SEGMENT_COUNT + 1 : 2}, .completion = fixture->completion},
        0
    );
    GPUTimelineSemaphoreWait(fixture->completion);
    for (uint32_t index = 0; index < (iteration < 4 ? SEGMENT_COUNT : 1); ++index)
        GPUCommandPoolReadTimestamps(fixture->workers[index].pool);
    bool valid = true;
    for (uint32_t y = 0; y < HEIGHT; ++y)
        for (uint32_t x = 0; x < WIDTH; ++x) {
            const uint8_t* pixel = fixture->readback.range.cpu + (y * WIDTH + x) * 4;
            const bool covered = x % 8 / 4 == (iteration & 1u);
            valid &= pixel[0] == (covered ? 255 : 0) && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255;
        }
    const uint64_t* timestamps = (const uint64_t*)(fixture->readback.range.cpu + PIXEL_BYTES);
    for (uint32_t index = 0; index < SEGMENT_COUNT * 2; ++index) {
        if (iteration & 1u)
            valid &= timestamps[index] == SENTINEL;
        else
            valid &= timestamps[index] != SENTINEL && (index == 0 || timestamps[index] >= timestamps[index - 1]);
    }
    if ((iteration & 1u) == 0) {
        valid &= timestamps[0] != fixture->previousTimestamp;
        fixture->previousTimestamp = timestamps[0];
    }
    if (!valid) fprintf(stderr, "Render continuation pixel or timestamp readback failed in iteration %u.\n", iteration);
    return valid;
}

int main(void) {
    const GPUDeviceDesc desc = {.timestampQueryCount = SEGMENT_COUNT * 2};
    Fixture fixture = {.device = TestRequireDevice(&desc), .previousTimestamp = SENTINEL};
    bool valid = initialize(&fixture);
    for (uint32_t iteration = 0; valid && iteration < 6; ++iteration) valid = renderIteration(&fixture, iteration);
    GPUDeviceWaitIdle(fixture.device);
    for (uint32_t index = 0; index < SEGMENT_COUNT; ++index) GPUCommandPoolDestroy(fixture.workers[index].pool);
    GPUCommandPoolDestroy(fixture.readbackPool);
    GPURenderViewDestroy(fixture.view);
    GPUTextureDestroy(fixture.target);
    GPUTextureHeapDestroy(&fixture.textureHeap);
    GPUGpuHeapDestroy(&fixture.readback);
    GPUTimelineSemaphoreDestroy(fixture.completion.semaphore);
    GPUPSODestroy(fixture.pso);
    TestDestroyDevice(fixture.device);
    printf("Parallel render continuation, timestamp readback and pool reuse: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
