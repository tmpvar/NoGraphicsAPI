#include "shaders/root_data_generate_shared.h"
#include "test_support.h"

#include <stddef.h>

GPUStaticAssert(sizeof(RootData) == 256 && offsetof(RootData, values) == 16, root_data_layout);
GPUStaticAssert(sizeof(RootDataGenerate) == 32, root_data_generate_layout);

static GPUPSO* loadCompute(GPUDevice* device, const char* path, uint32_t threads) {
    const GPUByteSpan code = TestLoadShader(path);
    if (!code.data) return NULL;
    const GPUShaderStage stage = {.code = code, .entryPoint = "computeMain", .threadgroupSize = {threads, 1, 1}};
    GPUPSO* pso = GPUPSOCreateCompute(device, &stage);
    free((void*)code.data);
    return pso;
}

int main(int argc, char** argv) {
    const GPUDeviceDesc desc = {0};
    GPUDevice* device = TestRequireDevice(&desc);
    GPUPSO* consume = loadCompute(device, argc > 1 ? argv[1] : GPU_ROOT_DATA_PATH, 64);
    GPUPSO* generate = loadCompute(device, GPU_ROOT_GENERATE_PATH, 1);
    if (!consume || !generate) return 1;
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    GPUCommandPool* externalPool = GPUCommandPoolCreate(device, 0);
    GPUTimelineSemaphore* timeline = GPUTimelineSemaphoreCreate(device, 0);
    const GPUGpuHeap cpuRoots =
        GPUGpuHeapCreate(device, 3 * sizeof(RootData) + sizeof(RootDataGenerate), GPUMemoryType_CpuVisible);
    GPUBumpAllocator arena = GPUBumpAllocatorCreate(cpuRoots.range);
    const GPUGpuHeap roots = GPUGpuHeapCreate(device, 2 * sizeof(RootData) + 16, GPUMemoryType_GpuOnly);
    const GPUGpuHeap output = GPUGpuHeapCreate(device, 5 * 64 * sizeof(uint32_t), GPUMemoryType_GpuOnly);
    const GPUGpuHeap readback = GPUGpuHeapCreate(device, output.range.size, GPUMemoryType_Readback);
    bool valid = true;
    for (uint32_t round = 0; round < 4; ++round) {
        GPUCommandBuffer* first = GPUCmdBegin(pool);
        GPUCommandBuffer* second = GPUCmdBegin(pool);
        GPUCmdBindPSO(first, consume);
        GPUCmdBindPSO(second, consume);
        RootData root = {.output = (uint32_t*)output.range.gpu};
        for (uint32_t i = 0; i < 3; ++i) {
            root.output_index = i * 64;
            root.seed = round * 100 + i;
            for (uint32_t j = 0; j < 60; ++j) root.values[j] = root.seed * 7 + j;
            const GPUGpuCpuRange copied = GPUBumpAllocatorAllocate(&arena, sizeof(RootData));
            memcpy(copied.cpu, &root, sizeof(root));
            GPUCmdDispatch(i == 1 ? second : first, copied.gpu, (GPUExtent3){1, 1, 1});
        }
        memset(&root, 0, sizeof(root));
        GPUCmdBindPSO(second, generate);
        const GPUGpuCpuRange generated = GPUBumpAllocatorAllocateAtomic(&arena, sizeof(RootDataGenerate));
        const RootDataGenerate generateRoot = {
            .destination = (RootData*)roots.range.gpu,
            .output = (uint32_t*)output.range.gpu,
            .dispatch_arguments = (uint32_t*)(roots.range.gpu + 2 * sizeof(RootData)),
            .first_index = 3,
            .seed = round * 100 + 3,
        };
        memcpy(generated.cpu, &generateRoot, sizeof(generateRoot));
        GPUCmdDispatch(second, generated.gpu, (GPUExtent3){2, 1, 1});
        GPUCmdEnd(first);
        GPUCmdEnd(second);
        GPUCommandBuffer* third = GPUCmdBegin(externalPool);
        GPUCmdBarrier(
            third,
            GPUStage_Compute,
            GPUAccess_ShaderWrite,
            GPUStage_Compute | GPUStage_Indirect,
            GPUAccess_ShaderRead | GPUAccess_IndirectRead
        );
        GPUCmdBindPSO(third, consume);
        GPUCmdDispatch(third, roots.range.gpu, (GPUExtent3){1, 1, 1});
        GPUCmdDispatchIndirect(
            third,
            roots.range.gpu + sizeof(RootData),
            GPUGpuSubrange(GPUGpuRangeOfHeap(&roots), 2 * sizeof(RootData), 12)
        );
        GPUCmdBarrier(third, GPUStage_Compute, GPUAccess_ShaderWrite, GPUStage_Transfer, GPUAccess_TransferRead);
        GPUCmdCopyMemory(third, GPUGpuRangeOfHeap(&output), GPUGpuRangeOfHeap(&readback));
        GPUCmdBarrier(third, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Host, GPUAccess_HostRead);
        GPUCmdEnd(third);
        GPUDeviceSubmit(
            device,
            &(GPUSubmitDesc){.commands = GPUCommandBuffers(first, second, third), .completion = {timeline, round + 1}},
            0
        );
        GPUTimelineSemaphoreWait((GPUTimelinePoint){timeline, round + 1});
        const uint32_t* actual = (const uint32_t*)readback.range.cpu;
        for (uint32_t i = 0; i < 5; ++i)
            for (uint32_t lane = 0; lane < 64; ++lane) {
                const uint32_t seed = round * 100 + i;
                uint32_t expected = seed + lane;
                for (uint32_t j = 0; j < 60; ++j) expected = (expected * 33u) ^ (seed * 7 + j);
                if (actual[i * 64 + lane] != expected) valid = false;
            }
        GPUCommandPoolReset(pool);
        GPUCommandPoolReset(externalPool);
        GPUBumpAllocatorReset(&arena);
    }
    GPUDeviceWaitIdle(device);
    GPUCommandPoolDestroy(externalPool);
    GPUCommandPoolDestroy(pool);
    GPUGpuHeapDestroy(&readback);
    GPUGpuHeapDestroy(&output);
    GPUGpuHeapDestroy(&roots);
    GPUGpuHeapDestroy(&cpuRoots);
    GPUTimelineSemaphoreDestroy(timeline);
    GPUPSODestroy(generate);
    GPUPSODestroy(consume);
    TestDestroyDevice(device);
    printf(
        "Application root allocation, interleaved recording, arena reset, GPU-written roots and indirect arguments: "
        "%s\n",
        valid ? "PASS" : "FAIL"
    );
    return valid ? 0 : 1;
}
