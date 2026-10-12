#include "shaders/task_mesh_shared.h"
#include "shaders/task_shader_shared.h"
#include "test_support.h"

#include <stddef.h>

GPUStaticAssert(sizeof(TaskShaderRoot) == 32 && sizeof(TaskMeshRoot) == 32, task_root_sizes);
GPUStaticAssert(offsetof(TaskShaderRoot, data) == 0 && offsetof(TaskMeshRoot, data) == 0, task_root_data_offset);
GPUStaticAssert(offsetof(TaskShaderRoot, count) == 8 && offsetof(TaskMeshRoot, count) == 8, task_root_count_offset);
GPUStaticAssert(
    offsetof(TaskShaderRoot, visible_mask) == 12 && offsetof(TaskMeshRoot, visible_mask) == 12,
    task_root_mask_offset
);
GPUStaticAssert(offsetof(TaskShaderRoot, color) == 16 && offsetof(TaskMeshRoot, color) == 16, task_root_color_offset);
GPUStaticAssert(sizeof(TaskTestData) == 784 && sizeof(TaskTestPayload) == 128, task_data_sizes);

#define PIXEL_OFFSET 1024u
#define PIXEL_BYTES ((uint64_t)task_test_width * task_test_height * 4)
#define DATA_BYTES (sizeof(TaskTestData) + sizeof(GPUExtent3))
#define ROOT_OFFSET ((DATA_BYTES + 15) & ~(uint64_t)15)

typedef struct ShaderCode {
    uint32_t words[8192];
    uint32_t size;
} ShaderCode;

typedef struct Fixture {
    GPUDevice* device;
    GPUCommandPool* pool;
    GPUPSO* pso;
    GPUTextureHeap textureHeap;
    GPUTexture* target;
    GPURenderView* view;
    GPUGpuHeap seed;
    GPUGpuHeap data;
    GPUGpuHeap readback;
    GPUTimelinePoint completion;
} Fixture;

static bool loadShader(const char* path, ShaderCode* code) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Cannot open task test shader: %s\n", path);
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long bytes = ftell(file);
    rewind(file);
    if (bytes < 20 || (uint64_t)bytes > sizeof(code->words)) {
        fclose(file);
        return false;
    }
    const bool read = fread(code->words, 1, (size_t)bytes, file) == (size_t)bytes;
    fclose(file);
    if (!read) return false;
    if ((bytes & 3) || code->words[0] != 0x07230203u) return false;
    code->size = (uint32_t)bytes;
    return true;
}

static bool initialize(Fixture* fixture) {
    static ShaderCode task, mesh, fragment;
    if (!loadShader(GPU_TASK_TEST_TASK_SHADER, &task) || !loadShader(GPU_TASK_TEST_MESH_SHADER, &mesh) ||
        !loadShader(GPU_TASK_TEST_FRAGMENT_SHADER, &fragment))
        return false;
    const GPUColorTargetDesc target = {.format = GPUFormat_Rgba8Unorm};
    const GPUMeshPSODesc psoDesc = {
        .task =
            {
                .code = GPUByteSpanOf(task.words, task.size),
                .entryPoint = "taskMain",
                .threadgroupSize = {task_test_batch_size, 1, 1},
            },
        .mesh =
            {
                .code = GPUByteSpanOf(mesh.words, mesh.size),
                .entryPoint = "meshMain",
                .threadgroupSize = {task_test_batch_size, 1, 1},
            },
        .fragment = {.code = GPUByteSpanOf(fragment.words, fragment.size), .entryPoint = "fragmentMain"},
        .colorTargets = {&target, 1},
    };
    fixture->pso = GPUPSOCreateMesh(fixture->device, &psoDesc);
    if (!fixture->pso) return false;
    const GPUTextureDesc targetDesc = {
        .extent = {task_test_width, task_test_height, 1},
        .usage = GPUTextureUsage_ColorAttachment | GPUTextureUsage_TransferSource,
    };
    fixture->textureHeap =
        GPUTextureHeapCreate(fixture->device, GPUTextureGetSizeAlign(fixture->device, &targetDesc).size);
    fixture->pool = GPUCommandPoolCreate(fixture->device, 0);
    GPUCommandBuffer* commands = GPUCmdBegin(fixture->pool);
    fixture->target = GPUTextureCreate(commands, &targetDesc, &fixture->textureHeap, 0);
    if (!fixture->target) return false;
    const GPURenderViewDesc viewDesc = {0};
    fixture->view = GPURenderViewCreate(fixture->target, &viewDesc);
    if (!fixture->view) return false;
    fixture->completion.semaphore = GPUTimelineSemaphoreCreate(fixture->device, 0);
    if (!fixture->completion.semaphore) return false;
    fixture->seed = GPUGpuHeapCreate(fixture->device, ROOT_OFFSET + sizeof(TaskShaderRoot), GPUMemoryType_CpuVisible);
    fixture->data = GPUGpuHeapCreate(fixture->device, DATA_BYTES, GPUMemoryType_GpuOnly);
    fixture->readback = GPUGpuHeapCreate(fixture->device, PIXEL_OFFSET + PIXEL_BYTES, GPUMemoryType_Readback);
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

static bool renderCase(Fixture* fixture, uint32_t count, uint32_t visibleMask, uint32_t scenario, bool indirect) {
    TaskTestData* seed = (TaskTestData*)fixture->seed.range.cpu;
    memset(seed, 0, sizeof(*seed));
    for (uint32_t id = 0; id < task_test_capacity; ++id) seed->visibility[id] = 1u << (id % 3);
    for (uint32_t batch = 0; batch < task_test_batches; ++batch) seed->batch_counts[batch] = ~0u;
    const uint32_t batches = count ? (count + task_test_batch_size - 1) / task_test_batch_size : 1;
    const GPUExtent3 groupCount = {batches, 1, 1};
    memcpy(fixture->seed.range.cpu + sizeof(TaskTestData), &groupCount, sizeof(groupCount));
    memset(fixture->readback.range.cpu, 0xa5, (size_t)fixture->readback.range.size);
    const TaskShaderRoot root = {
        .data = (TaskTestData*)fixture->data.range.gpu,
        .count = count,
        .visible_mask = visibleMask,
        .color = {.x = 1.0f, .y = (float)(scenario & 1u), .z = (float)indirect, .w = 1.0f},
    };
    GPUCommandPoolReset(fixture->pool);
    GPUCommandBuffer* commands = GPUCmdBegin(fixture->pool);
    GPUCmdBarrier(
        commands,
        GPUStage_Task | GPUStage_Mesh | GPUStage_Transfer,
        GPUAccess_ShaderWrite | GPUAccess_TransferRead,
        GPUStage_Transfer,
        GPUAccess_TransferWrite
    );
    memcpy(fixture->seed.range.cpu + ROOT_OFFSET, &root, sizeof(root));
    GPUCmdCopyMemory(
        commands,
        GPUGpuSubrange(GPUGpuRangeOfHeap(&fixture->seed), 0, DATA_BYTES),
        GPUGpuRangeOfHeap(&fixture->data)
    );
    GPUCmdBarrier(
        commands,
        GPUStage_Transfer,
        GPUAccess_TransferWrite,
        GPUStage_Task | GPUStage_Mesh | GPUStage_Indirect,
        GPUAccess_ShaderRead | GPUAccess_ShaderWrite | GPUAccess_IndirectRead
    );
    GPUCmdBarrier(commands, GPUStage_Transfer, GPUAccess_TransferRead, GPUStage_ColorOutput, GPUAccess_ColorWrite);
    const GPUColorAttachment color = {.renderView = fixture->view, .load = GPULoadOp_Clear, .clear = {.w = 1.0f}};
    const GPURenderingDesc rendering = {.colors = {&color, 1}};
    GPUCmdBeginRenderPass(commands, &rendering, GPURenderingFlags_None);
    const GPUViewport viewport = {.width = (float)task_test_width, .height = (float)task_test_height};
    GPUCmdSetViewport(commands, &viewport);
    const GPUScissor scissor = {.width = task_test_width, .height = task_test_height};
    GPUCmdSetScissor(commands, &scissor);
    GPUCmdBindPSO(commands, fixture->pso);
    if (indirect)
        GPUCmdDrawMeshletsIndirect(
            commands,
            fixture->seed.range.gpu + ROOT_OFFSET,
            GPUGpuSubrange(GPUGpuRangeOfHeap(&fixture->data), sizeof(TaskTestData), sizeof(GPUExtent3)),
            1,
            0
        );
    else
        GPUCmdDrawMeshlets(commands, fixture->seed.range.gpu + ROOT_OFFSET, groupCount);
    GPUCmdEndRenderPass(commands);
    GPUCmdBarrier(
        commands,
        GPUStage_Task | GPUStage_Mesh,
        GPUAccess_ShaderWrite,
        GPUStage_Transfer,
        GPUAccess_TransferRead
    );
    GPUCmdBarrier(commands, GPUStage_ColorOutput, GPUAccess_ColorWrite, GPUStage_Transfer, GPUAccess_TransferRead);
    GPUCmdCopyMemory(
        commands,
        GPUGpuSubrange(GPUGpuRangeOfHeap(&fixture->data), 0, sizeof(TaskTestData)),
        GPUGpuSubrange(GPUGpuRangeOfHeap(&fixture->readback), 0, sizeof(TaskTestData))
    );
    const GPUTextureCopyDesc copy = {0};
    GPUCmdCopyTextureToMemory(
        commands,
        fixture->target,
        GPUGpuSubrange(GPUGpuRangeOfHeap(&fixture->readback), PIXEL_OFFSET, PIXEL_BYTES),
        &copy
    );
    GPUCmdBarrier(commands, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Host, GPUAccess_HostRead);
    GPUCmdEnd(commands);
    ++fixture->completion.value;
    GPUDeviceSubmit(
        fixture->device,
        &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = fixture->completion},
        0
    );
    GPUTimelineSemaphoreWait(fixture->completion);

    const TaskTestData* actual = (const TaskTestData*)fixture->readback.range.cpu;
    bool valid = memcmp(actual->visibility, seed->visibility, sizeof(seed->visibility)) == 0 &&
                 actual->visits[task_test_capacity] == 0;
    for (uint32_t batch = 0; batch < task_test_batches; ++batch) {
        uint32_t expected = 0;
        for (uint32_t lane = 0; lane < task_test_batch_size; ++lane) {
            const uint32_t id = batch * task_test_batch_size + lane;
            const uint32_t visible = (uint32_t)(id < count && (seed->visibility[id] & visibleMask) != 0);
            expected += visible;
            valid &= actual->visits[id] == visible;
        }
        valid &= actual->batch_counts[batch] == (batch < batches ? expected : ~0u);
    }
    for (uint64_t offset = sizeof(TaskTestData); offset < PIXEL_OFFSET; ++offset)
        valid &= fixture->readback.range.cpu[offset] == 0xa5;
    const uint8_t* pixels = fixture->readback.range.cpu + PIXEL_OFFSET;
    for (uint32_t y = 0; y < task_test_height; ++y)
        for (uint32_t x = 0; x < task_test_width; ++x) {
            const bool covered = x / 4 < count && (seed->visibility[x / 4] & visibleMask) != 0 &&
                                 (x % 4 == 1 || x % 4 == 2) && y >= 2 && y < 6;
            const uint8_t* pixel = pixels + (y * task_test_width + x) * 4;
            valid &= pixel[0] == (covered ? 255 : 0) && pixel[1] == (covered && (scenario & 1u) ? 255 : 0) &&
                     pixel[2] == (covered && indirect ? 255 : 0) && pixel[3] == 255;
        }
    if (!valid)
        fprintf(
            stderr,
            "Task shader %s case %u failed: count=%u mask=%u.\n",
            indirect ? "indirect" : "direct",
            scenario,
            count,
            visibleMask
        );
    return valid;
}

int main(int argc, char** argv) {
    const bool directOnly = argc == 2 && strcmp(argv[1], "--direct-only") == 0;
    if (argc != 1 && !directOnly) {
        fprintf(stderr, "Usage: %s [--direct-only]\n", argv[0]);
        return 1;
    }
    GPUDeviceDesc desc = {0};
    Fixture fixture = {.device = TestRequireDevice(&desc)};
    const bool testIndirect = !directOnly && GPUDeviceGetCaps(fixture.device)->indirectMeshDraw;
    if (!directOnly && !testIndirect)
        printf("Native indirect mesh draws unavailable; testing direct task/mesh draws.\n");
    bool valid = initialize(&fixture);
    if (valid) {
        const uint32_t counts[] = {0, 1, 32, 33, 70, 70, 70};
        const uint32_t masks[] = {7, 7, 7, 5, 7, 2, 0};
        for (uint32_t indirect = 0; indirect < (testIndirect ? 2u : 1u); ++indirect)
            for (uint32_t scenario = 0; scenario < GPUArrayCount(counts); ++scenario)
                valid &= renderCase(&fixture, counts[scenario], masks[scenario], scenario, indirect != 0);
    }
    GPUDeviceWaitIdle(fixture.device);
    GPUCommandPoolDestroy(fixture.pool);
    GPUGpuHeapDestroy(&fixture.readback);
    GPUGpuHeapDestroy(&fixture.data);
    GPUGpuHeapDestroy(&fixture.seed);
    GPUTimelineSemaphoreDestroy(fixture.completion.semaphore);
    GPURenderViewDestroy(fixture.view);
    GPUTextureDestroy(fixture.target);
    GPUTextureHeapDestroy(&fixture.textureHeap);
    GPUPSODestroy(fixture.pso);
    TestDestroyDevice(fixture.device);
    printf("Task shader compaction, culling and root ABI: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
