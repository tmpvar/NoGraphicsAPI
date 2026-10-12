#include "shaders/shader_abi_shared.h"
#include "test_support.h"

#include <math.h>
#include <stddef.h>

GPUStaticAssert(sizeof(ShaderAbiData) == 80 && sizeof(ShaderAbiRoot) == 96, shader_abi_sizes);
GPUStaticAssert(
    offsetof(ShaderAbiRoot, vector) == 4 && offsetof(ShaderAbiRoot, color) == 16,
    shader_abi_vector_offsets
);
GPUStaticAssert(
    offsetof(ShaderAbiRoot, output) == 32 && offsetof(ShaderAbiRoot, matrix) == 40 &&
        offsetof(ShaderAbiRoot, sampled) == 80,
    shader_abi_offsets
);

// Vulkan guarantees 4,000 sampler descriptors plus its reserved heap range.
#define SAMPLER_COUNT 4000u

static bool testShaderAbi(GPUDevice* device) {
    const GPUByteSpan code = TestLoadShader(GPU_SHADER_ABI_PATH);
    if (!code.data) return false;
    const GPUShaderStage stage =
        {.code = code, .entryPoint = "computeMain", .threadgroupSize = {shader_abi_thread_count, 1, 1}};
    GPUPSO* pso = GPUPSOCreateCompute(device, &stage);
    free((void*)code.data);
    if (!pso) return false;
    const GPUTextureDesc desc = {
        .extent = {2, 1, 1},
        .usage = GPUTextureUsage_Sampled | GPUTextureUsage_TransferDestination
    };
    const GPUSizeAlign placement = GPUTextureGetSizeAlign(device, &desc);
    const uint64_t stride = (placement.size + placement.align - 1) / placement.align * placement.align;
    const GPUTextureHeap textureHeap = GPUTextureHeapCreate(device, stride * shader_abi_thread_count);
    GPUTexture* textures[shader_abi_thread_count] = {NULL};
    GPUTextureDescriptorHeap* views = GPUTextureDescriptorHeapCreate(device, shader_abi_thread_count);
    GPUSamplerDescriptorHeap* samplers = GPUSamplerDescriptorHeapCreate(device, SAMPLER_COUNT);
    const GPUGpuHeap upload = GPUGpuHeapCreate(device, 128, GPUMemoryType_CpuVisible);
    const GPUGpuHeap output = GPUGpuHeapCreate(device, 1024, GPUMemoryType_GpuOnly);
    const GPUGpuHeap readback = GPUGpuHeapCreate(device, 1024, GPUMemoryType_Readback);
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    GPUCommandBuffer* commands = GPUCmdBegin(pool);
    const GPUTextureDescriptorDesc descriptorDesc = {0};
    for (uint32_t i = 0; i != shader_abi_thread_count; ++i) {
        textures[i] = GPUTextureCreate(commands, &desc, &textureHeap, stride * i);
        if (!textures[i]) return false;
        GPUTextureDescriptorHeapWrite(views, i, textures[i], GPUTextureDescriptorType_Sampled, &descriptorDesc);
        const GPUSamplerDesc sampler = {
            .minFilter = GPUFilter_Nearest,
            .magFilter = GPUFilter_Nearest,
            .addressU = (i & 1) ? GPUAddressMode_Repeat : GPUAddressMode_ClampToEdge,
        };
        GPUSamplerDescriptorHeapWrite(samplers, SAMPLER_COUNT - shader_abi_thread_count + i, &sampler);
        for (uint32_t component = 0; component != 8; ++component)
            upload.range.cpu[i * 8 + component] = (uint8_t)(17 * i + component + 1);
    }
    GPUTimelineSemaphore* timeline = GPUTimelineSemaphoreCreate(device, 0);
    if (!timeline) return false;
    const ShaderAbiRoot root = {
        .scalar = 3.0f,
        .vector = {.x = 5.0f, .y = 6.0f, .z = 7.0f},
        .color = {.x = 8.0f, .y = 9.0f, .z = 10.0f, .w = 11.0f},
        .output = (ShaderAbiData*)output.range.gpu,
        .matrix =
            {.rows =
                 {{.x = 12.0f, .y = 13.0f, .z = 14.0f},
                  {.x = 15.0f, .y = 16.0f, .z = 17.0f},
                  {.x = 18.0f, .y = 19.0f, .z = 20.0f}}},
        .sampled = (F32x4*)(output.range.gpu + 512),
        .texture_base = 0,
        .sampler_base = SAMPLER_COUNT - shader_abi_thread_count,
    };
    const GPUTextureCopyDesc copy = {0};
    for (uint32_t i = 0; i != shader_abi_thread_count; ++i)
        GPUCmdCopyMemoryToTexture(commands, GPUGpuSubrange(GPUGpuRangeOfHeap(&upload), i * 8, 8), textures[i], &copy);
    GPUCmdBarrier(commands, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Compute, GPUAccess_ShaderRead);
    GPUCmdSetTextureDescriptorHeap(commands, views);
    GPUCmdSetSamplerDescriptorHeap(commands, samplers);
    GPUCmdBindPSO(commands, pso);
    memcpy(upload.range.cpu + 32, &root, sizeof(root));
    GPUCmdDispatch(commands, upload.range.gpu + 32, (GPUExtent3){1, 1, 1});
    GPUCmdBarrier(commands, GPUStage_Compute, GPUAccess_ShaderWrite, GPUStage_Transfer, GPUAccess_TransferRead);
    GPUCmdCopyMemory(commands, GPUGpuRangeOfHeap(&output), GPUGpuRangeOfHeap(&readback));
    GPUCmdEnd(commands);
    GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = {timeline, 1}}, 0);
    GPUTimelineSemaphoreWait((GPUTimelinePoint){timeline, 1});
    bool valid = true;
    const ShaderAbiData* values = (const ShaderAbiData*)readback.range.cpu;
    const F32x4* sampled = (const F32x4*)(readback.range.cpu + 512);
    for (uint32_t i = 0; i != shader_abi_thread_count; ++i) {
        valid &= values[i].scalar == root.scalar && memcmp(&values[i].vector, &root.vector, sizeof(root.vector)) == 0;
        valid &= memcmp(&values[i].color, &root.color, sizeof(root.color)) == 0 &&
                 memcmp(&values[i].matrix, &root.matrix, sizeof(root.matrix)) == 0;
        const float* transformed = &values[i].transformed.x;
        for (uint32_t row = 0; row != 3; ++row)
            valid &= transformed[row] ==
                     root.matrix.rows[row].x * 2 + root.matrix.rows[row].y * 3 + root.matrix.rows[row].z * 5;
        const float* components = &sampled[i].x;
        for (uint32_t component = 0; component != 4; ++component)
            valid &=
                fabsf(components[component] - (float)upload.range.cpu[i * 8 + ((i & 1) ? 0 : 4) + component] / 255.0f) <
                0.00001f;
    }
    GPUCommandPoolDestroy(pool);
    GPUTimelineSemaphoreDestroy(timeline);
    GPUGpuHeapDestroy(&readback);
    GPUGpuHeapDestroy(&output);
    GPUGpuHeapDestroy(&upload);
    GPUSamplerDescriptorHeapDestroy(samplers);
    GPUTextureDescriptorHeapDestroy(views);
    for (uint32_t i = 0; i != shader_abi_thread_count; ++i) GPUTextureDestroy(textures[i]);
    GPUTextureHeapDestroy(&textureHeap);
    GPUPSODestroy(pso);
    return valid;
}

int main(void) {
    const GPUDeviceDesc desc = {0};
    GPUDevice* device = TestRequireDevice(&desc);
    const bool valid = testShaderAbi(device);
    GPUDeviceWaitIdle(device);
    TestDestroyDevice(device);
    printf(
        "%s: shared shader root layout, matrices, GPU pointers, divergent texture slots and sampler indices %u..%u\n",
        valid ? "PASS" : "FAIL",
        SAMPLER_COUNT - shader_abi_thread_count,
        SAMPLER_COUNT - 1
    );
    return valid ? 0 : 1;
}
