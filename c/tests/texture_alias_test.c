#include "test_support.h"

#define WIDTH 256u
#define HEIGHT 192u
#define PASSES 8u
#define READBACK_STRIDE ((uint64_t)WIDTH * HEIGHT * 8)

static void recordAlias(
    GPUCommandBuffer* commands,
    GPUTexture* const* textures,
    GPURenderView* const* views,
    const GPUGpuHeap* readback,
    uint32_t pass
) {
    const uint32_t mode = pass & 1u;
    const bool alternate = (pass & 2u) != 0;
    GPUCmdActivateTextureAlias(commands, textures[mode]);
    GPUColorAttachment color = {.renderView = views[mode], .load = GPULoadOp_Clear};
    color.clear =
        mode ? (GPUClearColor){.x = alternate ? 0.5f : 2.5f, .y = alternate ? 1.0f : 12.5f, .z = 0.5f, .w = 1.0f}
             : (GPUClearColor){.x = alternate ? 0.0f : 1.0f, .y = alternate ? 1.0f : 0.0f, .z = 1.0f, .w = 1.0f};
    const GPURenderingDesc rendering = {.colors = {&color, 1}};
    GPUCmdBeginRenderPass(commands, &rendering, GPURenderingFlags_None);
    GPUCmdEndRenderPass(commands);
    GPUCmdBarrier(commands, GPUStage_ColorOutput, GPUAccess_ColorWrite, GPUStage_Transfer, GPUAccess_TransferRead);
    const GPUTextureCopyDesc copy = {.extent = {WIDTH, HEIGHT, 1}};
    GPUCmdCopyTextureToMemory(
        commands,
        textures[mode],
        GPUGpuSubrange(GPUGpuRangeOfHeap(readback), pass * READBACK_STRIDE, READBACK_STRIDE),
        &copy
    );
}

static bool checkAlias(const GPUGpuHeap* readback, uint32_t pass) {
    const bool alternate = (pass & 2u) != 0;
    const uint8_t* data = readback->range.cpu + pass * READBACK_STRIDE;
    for (uint32_t pixel = 0; pixel < WIDTH * HEIGHT; ++pixel) {
        bool valid;
        if (pass & 1u) {
            uint16_t color[4];
            memcpy(color, data + pixel * 8, sizeof(color));
            valid = color[0] == (alternate ? 0x3800 : 0x4100) && color[1] == (alternate ? 0x3c00 : 0x4a40) &&
                    color[2] == 0x3800 && color[3] == 0x3c00;
        } else {
            const uint8_t* color = data + pixel * 4;
            valid = color[0] == 255 && color[1] == (alternate ? 255 : 0) && color[2] == (alternate ? 0 : 255) &&
                    color[3] == 255;
        }
        if (!valid) {
            fprintf(stderr, "Aliased texture pass %u pixel %u has unexpected color.\n", pass, pixel);
            return false;
        }
    }
    return true;
}

int main(void) {
    GPUDeviceDesc deviceDesc = {0};
    GPUDevice* device = TestRequireDevice(&deviceDesc);
    GPUTextureDesc descriptions[2] = {0};
    for (uint32_t mode = 0; mode < 2; ++mode) {
        descriptions[mode].extent = (GPUExtent3){WIDTH, HEIGHT, 1};
        descriptions[mode].format = mode ? GPUFormat_Rgba16Float : GPUFormat_Bgra8Srgb;
        descriptions[mode].usage =
            GPUTextureUsage_Sampled | GPUTextureUsage_ColorAttachment | GPUTextureUsage_TransferSource;
        descriptions[mode].aliasable = true;
    }
    const GPUSizeAlign sdr = GPUTextureGetSizeAlign(device, &descriptions[0]);
    const GPUSizeAlign hdr = GPUTextureGetSizeAlign(device, &descriptions[1]);
    const uint64_t offset = sdr.align > hdr.align ? sdr.align : hdr.align;
    const GPUTextureHeap heap = GPUTextureHeapCreate(device, offset + (sdr.size > hdr.size ? sdr.size : hdr.size));
    const GPUGpuHeap readback = GPUGpuHeapCreate(device, PASSES * READBACK_STRIDE, GPUMemoryType_Readback);
    GPUTextureDescriptorHeap* descriptors = GPUTextureDescriptorHeapCreate(device, 2);
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    GPUTimelinePoint completion = {.semaphore = GPUTimelineSemaphoreCreate(device, 0), .value = 0};
    GPUCommandBuffer* commands = GPUCmdBegin(pool);
    GPUTexture* textures[2] = {NULL, NULL};
    GPURenderView* views[2] = {NULL, NULL};
    const GPURenderViewDesc viewDesc = {0};
    const GPUTextureDescriptorDesc descriptorDesc = {0};
    for (uint32_t mode = 0; mode < 2; ++mode) {
        textures[mode] = GPUTextureCreate(commands, &descriptions[mode], &heap, offset);
        views[mode] = GPURenderViewCreate(textures[mode], &viewDesc);
        GPUTextureDescriptorHeapWrite(
            descriptors,
            mode,
            textures[mode],
            GPUTextureDescriptorType_Sampled,
            &descriptorDesc
        );
    }
    // Alternate without CPU waits first, then repeat handoffs between idle submissions using the same handles and
    // descriptors.
    for (uint32_t pass = 0; pass < PASSES; ++pass) recordAlias(commands, textures, views, &readback, pass);
    GPUCmdBarrier(commands, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Host, GPUAccess_HostRead);
    GPUCmdEnd(commands);
    ++completion.value;
    GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = completion}, 0);
    GPUTimelineSemaphoreWait(completion);
    bool valid = true;
    for (uint32_t pass = 0; pass < PASSES; ++pass) valid = checkAlias(&readback, pass) && valid;
    for (uint32_t pass = 0; pass < PASSES; ++pass) {
        GPUDeviceWaitIdle(device);
        GPUCommandPoolReset(pool);
        commands = GPUCmdBegin(pool);
        recordAlias(commands, textures, views, &readback, pass);
        GPUCmdBarrier(commands, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Host, GPUAccess_HostRead);
        GPUCmdEnd(commands);
        ++completion.value;
        GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = completion}, 0);
        GPUTimelineSemaphoreWait(completion);
        valid = checkAlias(&readback, pass) && valid;
    }
    GPUDeviceWaitIdle(device);
    GPUCommandPoolDestroy(pool);
    GPUTextureDescriptorHeapDestroy(descriptors);
    for (uint32_t mode = 0; mode < 2; ++mode) {
        GPURenderViewDestroy(views[mode]);
        GPUTextureDestroy(textures[mode]);
    }
    GPUTextureHeapDestroy(&heap);
    GPUGpuHeapDestroy(&readback);
    GPUTimelineSemaphoreDestroy(completion.semaphore);
    TestDestroyDevice(device);
    printf("Resident SDR/HDR texture aliases, batched and idle handoffs: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
