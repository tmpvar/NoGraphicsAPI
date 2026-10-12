#include "test_support.h"

static bool check(
    GPUDevice* device,
    GPUTimelineSemaphore* timeline,
    uint64_t* value,
    GPUTextureType type,
    GPUFormat format
) {
    const bool compressed = format == GPUFormat_Bc7Unorm;
    const bool volume = type == GPUTextureType_3D;
    const GPUTextureDesc desc = {
        .type = type,
        .extent = {8, 8, volume ? 4u : 1u},
        .layerCount = volume                        ? 1u
                      : type == GPUTextureType_Cube ? 6u
                                                    : 3u,
        .format = format,
        .usage = GPUTextureUsage_TransferSource | GPUTextureUsage_TransferDestination,
    };
    const GPUSizeAlign required = GPUTextureGetSizeAlign(device, &desc);
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    GPUCommandBuffer* commands = GPUCmdBegin(pool);
    const GPUTextureHeap heap = GPUTextureHeapCreate(device, required.size);
    GPUTexture* texture = GPUTextureCreate(commands, &desc, &heap, 0);
    const GPUGpuHeap upload = GPUGpuHeapCreate(device, 4096, GPUMemoryType_CpuVisible);
    const GPUGpuHeap readback = GPUGpuHeapCreate(device, 4096, GPUMemoryType_Readback);
    for (uint32_t i = 0; i < 4096; ++i) upload.range.cpu[i] = (uint8_t)(i * 17 + i / 16);
    memset(readback.range.cpu, 0xa5, 4096);
    const uint32_t rowPitch = compressed ? 48 : 16;
    const uint32_t slicePitch = compressed ? 144 : 64;
    const uint32_t rowBytes = compressed ? 32 : 3;
    const GPUTextureCopyDesc copy = {
        .baseSlice = volume ? 0u : 1u,
        .sliceCount = volume ? 1u : 2u,
        .offset = {compressed ? 0u : 1u, compressed ? 0u : 1u, volume ? 1u : 0u},
        .extent = {compressed ? 8u : 3u, compressed ? 8u : 2u, volume ? 2u : 1u},
        .rowPitchBytes = rowPitch,
        .slicePitchBytes = slicePitch,
    };
    GPUCmdCopyMemoryToTexture(commands, GPUGpuRangeOfHeap(&upload), texture, &copy);
    // NVIDIA 596.99 needs a timeline wait before address-based image readback; see docs/known-driver-issues.md.
    GPUCmdEnd(commands);
    GPUDeviceSubmit(
        device,
        &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = {timeline, ++*value}},
        0
    );
    GPUTimelineSemaphoreWait((GPUTimelinePoint){timeline, *value});
    commands = GPUCmdBegin(pool);
    GPUCmdBarrier(commands, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Transfer, GPUAccess_TransferRead);
    GPUCmdCopyTextureToMemory(commands, texture, GPUGpuRangeOfHeap(&readback), &copy);
    GPUCmdEnd(commands);
    GPUDeviceSubmit(
        device,
        &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = {timeline, ++*value}},
        0
    );
    GPUTimelineSemaphoreWait((GPUTimelinePoint){timeline, *value});
    bool valid = true;
    for (uint32_t i = 0; i < 4096; ++i) {
        const uint32_t slice = i / slicePitch;
        const uint32_t inSlice = i % slicePitch;
        const bool copied = slice < 2 && inSlice / rowPitch < 2 && inSlice % rowPitch < rowBytes;
        const uint8_t expected = copied ? upload.range.cpu[i] : (uint8_t)0xa5;
        if (readback.range.cpu[i] != expected) {
            fprintf(
                stderr,
                "Texture copy type=%u format=%u at byte %u: got %u, expected %u.\n",
                (uint32_t)type,
                (uint32_t)format,
                i,
                readback.range.cpu[i],
                expected
            );
            valid = false;
            break;
        }
    }
    GPUCommandPoolDestroy(pool);
    GPUGpuHeapDestroy(&upload);
    GPUGpuHeapDestroy(&readback);
    GPUTextureDestroy(texture);
    GPUTextureHeapDestroy(&heap);
    return valid;
}

int main(void) {
    GPUDeviceDesc desc = {0};
    GPUDevice* device = TestRequireDevice(&desc);
    GPUTimelineSemaphore* timeline = GPUTimelineSemaphoreCreate(device, 0);
    uint64_t value = 0;
    const bool valid = check(device, timeline, &value, GPUTextureType_3D, GPUFormat_R8Unorm) &&
                       check(device, timeline, &value, GPUTextureType_2DArray, GPUFormat_R8Unorm) &&
                       check(device, timeline, &value, GPUTextureType_Cube, GPUFormat_R8Unorm) &&
                       (!GPUDeviceSupportsTextureFormat(
                            device,
                            GPUFormat_Bc7Unorm,
                            GPUTextureUsage_TransferSource | GPUTextureUsage_TransferDestination
                        ) ||
                        check(device, timeline, &value, GPUTextureType_2DArray, GPUFormat_Bc7Unorm));
    GPUDeviceWaitIdle(device);
    GPUTimelineSemaphoreDestroy(timeline);
    TestDestroyDevice(device);
    printf("3D/array/cube/BC7 pitch roundtrips: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
