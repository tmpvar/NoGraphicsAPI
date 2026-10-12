// Compile-time layout checks, default values, format table consistency, and a link-time sweep of every public gpu.h
// entry point.
#include "test_support.h"

#include <stddef.h>

typedef struct ApiRoot {
    uint64_t vertices;
    uint32_t textureIndex;
    float scale;
} ApiRoot;

typedef struct WordSpan {
    const uint32_t* data;
    uint64_t count;
} WordSpan;
#define WORDS(...) GPUSpanOf(WordSpan, uint32_t, __VA_ARGS__)

static const uint32_t shaderWords[] = {0x07230203u, 0x00010600u};

static bool hasSpirvHeader(WordSpan words) { return words.count == 2 && words.data[0] == 0x07230203u; }

GPUStaticAssert(sizeof(void*) == 8, pointer_size);
GPUStaticAssert(sizeof(ApiRoot) % 4 == 0, api_root_size);
GPUStaticAssert(
    sizeof(GPUTextureUsage) == 4 && sizeof(GPURenderingFlags) == 4 && sizeof(GPUStage) == 8 && sizeof(GPUAccess) == 8,
    flag_sizes
);
GPUStaticAssert(
    GPUError_None == 0 && GPUError_Unsupported == 1 && GPUError_DeviceLost == 2 && GPUError_DriverError == 3 &&
        GPUError_OutOfMemory == 4,
    error_values
);
GPUStaticAssert((GPUTextureUsage_Sampled | GPUTextureUsage_Storage) == 3, usage_bits);
GPUStaticAssert((GPUStage_Vertex | GPUStage_Fragment) == 68, stage_bits);
GPUStaticAssert(GPUStage_Task == (1ull << 3), task_stage);
GPUStaticAssert(GPUStage_AllCommands == (1ull << 11), all_commands_stage);
GPUStaticAssert((GPUStage_AllCommands | GPUStage_Host) == ((1ull << 11) | (1ull << 10)), host_stage);
GPUStaticAssert((GPUAccess_ShaderRead | GPUAccess_ShaderWrite) == 12, access_bits);

GPUStaticAssert(sizeof(WordSpan) == sizeof(void*) + sizeof(uint64_t), word_span_size);
GPUStaticAssert(sizeof(GPUByteSpan) == sizeof(void*) + sizeof(uint64_t), byte_span_size);
GPUStaticAssert(sizeof(GPUCommandBufferSpan) == 16 && sizeof(GPUTimelinePointSpan) == 16, handle_span_sizes);

static const GPUFormat textureFormats[] = {
    GPUFormat_R8Srgb,         GPUFormat_Rg8Srgb,        GPUFormat_Rgba8Srgb,    GPUFormat_Bgra8Srgb,
    GPUFormat_Rgba4Unorm,     GPUFormat_R5g5b5a1Unorm,  GPUFormat_R5g6b5Unorm,  GPUFormat_R8Unorm,
    GPUFormat_Rg8Unorm,       GPUFormat_Rgba8Unorm,     GPUFormat_Bgra8Unorm,   GPUFormat_R16Unorm,
    GPUFormat_Rg16Unorm,      GPUFormat_Rgba16Unorm,    GPUFormat_R8Uint,       GPUFormat_Rg8Uint,
    GPUFormat_Rgba8Uint,      GPUFormat_Bgra8Uint,      GPUFormat_R16Uint,      GPUFormat_Rg16Uint,
    GPUFormat_Rgba16Uint,     GPUFormat_R32Uint,        GPUFormat_Rg32Uint,     GPUFormat_Rgba32Uint,
    GPUFormat_R16Float,       GPUFormat_Rg16Float,      GPUFormat_Rgba16Float,  GPUFormat_R32Float,
    GPUFormat_Rg32Float,      GPUFormat_Rgba32Float,    GPUFormat_Rgb10a2Unorm, GPUFormat_Rg11b10Float,
    GPUFormat_D16Unorm,       GPUFormat_D24UnormS8Uint, GPUFormat_D32Float,     GPUFormat_S8Uint,
    GPUFormat_D32FloatS8Uint, GPUFormat_EacRg,          GPUFormat_Astc4x4Srgb,  GPUFormat_Astc4x4Unorm,
    GPUFormat_Bc3Srgb,        GPUFormat_Bc3Unorm,       GPUFormat_Bc5Rg,        GPUFormat_Bc6hUfloat,
    GPUFormat_Bc6hSfloat,     GPUFormat_Bc7Srgb,        GPUFormat_Bc7Unorm,
};
GPUStaticAssert(GPUArrayCount(textureFormats) + 1 == GPUFormat_Count, texture_format_count);

static bool validTextureFormats(void) {
    for (size_t index = 0; index < GPUArrayCount(textureFormats); ++index) {
        const GPUTextureFormatInfo info = GPUFormatGetTextureInfo(textureFormats[index]);
        if ((size_t)textureFormats[index] != index + 1 || info.blockExtent.x == 0 || info.blockExtent.y == 0 ||
            info.bytesPerBlock == 0)
            return false;
    }
    return GPUFormatGetTextureInfo(GPUFormat_Rgba8Unorm).bytesPerBlock == 4 &&
           GPUFormatGetTextureInfo(GPUFormat_Astc4x4Unorm).blockExtent.x == 4 &&
           GPUFormatGetTextureInfo(GPUFormat_Bc6hUfloat).blockExtent.x == 4 &&
           GPUFormatGetTextureInfo(GPUFormat_Bc6hUfloat).blockExtent.y == 4 &&
           GPUFormatGetTextureInfo(GPUFormat_Bc6hUfloat).bytesPerBlock == 16 &&
           GPUFormatGetTextureInfo(GPUFormat_Bc6hSfloat).bytesPerBlock == 16 &&
           GPUFormatGetTextureInfo(GPUFormat_D24UnormS8Uint).depth &&
           GPUFormatGetTextureInfo(GPUFormat_D24UnormS8Uint).stencil &&
           GPUFormatGetTextureInfo(GPUFormat_Undefined).bytesPerBlock == 0;
}

// Layouts of structs that cross into shared headers or are read as raw memory. C enums are int-sized, so GPUTextureDesc
// differs from the C++ layout.
typedef struct AlignedGPUExtent2 {
    char c;
    GPUExtent2 value;
} AlignedGPUExtent2;
typedef struct AlignedGPUExtent3 {
    char c;
    GPUExtent3 value;
} AlignedGPUExtent3;
typedef struct AlignedViewport {
    char c;
    GPUViewport value;
} AlignedViewport;
typedef struct AlignedScissor {
    char c;
    GPUScissor value;
} AlignedScissor;
#define ALIGNMENT_OF(type) offsetof(Aligned##type, value)
GPUStaticAssert(
    sizeof(GPUExtent2) == 8 && ALIGNMENT_OF(GPUExtent2) == 4 && offsetof(GPUExtent2, x) == 0 &&
        offsetof(GPUExtent2, y) == 4,
    u32x2_layout
);
GPUStaticAssert(
    sizeof(GPUExtent3) == 12 && ALIGNMENT_OF(GPUExtent3) == 4 && offsetof(GPUExtent3, x) == 0 &&
        offsetof(GPUExtent3, y) == 4 && offsetof(GPUExtent3, z) == 8,
    u32x3_layout
);
GPUStaticAssert(
    sizeof(GPUTextureFormatInfo) == 16 && offsetof(GPUTextureFormatInfo, blockExtent) == 0 &&
        offsetof(GPUTextureFormatInfo, bytesPerBlock) == 8 && offsetof(GPUTextureFormatInfo, stencil) == 13,
    format_info_layout
);
GPUStaticAssert(sizeof(GPUSwapchainFrame) == 16 && offsetof(GPUSwapchainFrame, extent) == 8, swapchain_frame_layout);
GPUStaticAssert(
    sizeof(GPUTextureDesc) == 40 && offsetof(GPUTextureDesc, extent) == 4 && offsetof(GPUTextureDesc, aliasable) == 36,
    texture_desc_layout
);
GPUStaticAssert(
    sizeof(GPUTextureCopyDesc) == 56 && offsetof(GPUTextureCopyDesc, offset) == 12 &&
        offsetof(GPUTextureCopyDesc, extent) == 24,
    texture_copy_layout
);
GPUStaticAssert(
    sizeof(GPUViewport) == 24 && ALIGNMENT_OF(Viewport) == 4 && offsetof(GPUViewport, x) == 0 &&
        offsetof(GPUViewport, minDepth) == 16,
    viewport_layout
);
GPUStaticAssert(
    sizeof(GPUScissor) == 16 && ALIGNMENT_OF(Scissor) == 4 && offsetof(GPUScissor, x) == 0 &&
        offsetof(GPUScissor, width) == 8,
    scissor_layout
);
GPUStaticAssert(
    sizeof(GPUGpuRange) == 16 && offsetof(GPUGpuRange, gpu) == 0 && offsetof(GPUGpuRange, size) == 8,
    gpu_range_layout
);
GPUStaticAssert(
    sizeof(GPUGpuCpuRange) == 24 && offsetof(GPUGpuCpuRange, cpu) == 0 && offsetof(GPUGpuCpuRange, gpu) == 8 &&
        offsetof(GPUGpuCpuRange, size) == 16,
    gpu_cpu_range_layout
);
GPUStaticAssert(
    sizeof(GPUSizeAlign) == 16 && offsetof(GPUSizeAlign, size) == 0 && offsetof(GPUSizeAlign, align) == 8,
    size_align_layout
);
GPUStaticAssert(
    sizeof(GPUClearColor) == 16 && offsetof(GPUClearColor, x) == 0 && offsetof(GPUClearColor, y) == 4 &&
        offsetof(GPUClearColor, z) == 8 && offsetof(GPUClearColor, w) == 12,
    clear_color_layout
);
GPUStaticAssert(
    sizeof(GPUGpuHeap) == 32 && offsetof(GPUGpuHeap, range) == 0 && offsetof(GPUGpuHeap, owner) == 24,
    gpu_heap_layout
);
GPUStaticAssert(
    sizeof(GPUTextureHeap) == 16 && offsetof(GPUTextureHeap, size) == 0 && offsetof(GPUTextureHeap, owner) == 8,
    texture_heap_layout
);
GPUStaticAssert(
    sizeof(GPUTimelinePoint) == 16 && offsetof(GPUTimelinePoint, semaphore) == 0 &&
        offsetof(GPUTimelinePoint, value) == 8,
    timeline_point_layout
);

static bool validDefaults(void) {
    const GPUDeviceDesc deviceDesc = {0};
    const GPUExtent2 zero2 = {0};
    const GPUExtent3 zero3 = {0};
    const GPUSwapchainFrame frame = {0};
    bool valid = deviceDesc.win32.hwnd == NULL && deviceDesc.win32.hinstance == NULL &&
                 deviceDesc.swapchainFormat == GPUFormat_Undefined &&
                 deviceDesc.swapchainColorSpace == GPUColorSpace_Srgb && deviceDesc.timestampQueryCount == 0 &&
                 deviceDesc.errorCallback == NULL && deviceDesc.allocUser == NULL;
    valid = valid && zero2.x == 0 && zero2.y == 0 && zero3.x == 0 && zero3.y == 0 && zero3.z == 0 &&
            frame.renderView == NULL && frame.extent.x == 0 && frame.extent.y == 0;

    const GPUTextureDesc texture = {0};
    valid = valid && texture.type == GPUTextureType_Undefined && texture.format == GPUFormat_Undefined &&
            texture.usage == GPUTextureUsage_None && !texture.mutableFormat && !texture.aliasable;
    const GPUTextureDescriptorDesc textureDescriptor = {0};
    valid = valid && textureDescriptor.format == GPUFormat_Undefined &&
            textureDescriptor.aspect == GPUTextureAspect_Automatic;
    const GPUSamplerDesc sampler = {0};
    valid = valid && sampler.minFilter == GPUFilter_Undefined && sampler.addressU == GPUAddressMode_Repeat &&
            !sampler.anisotropic && !sampler.compareEnabled && sampler.compare == GPUCompareOp_Undefined;

    const GPUColorTargetDesc colorTarget = {0};
    valid = valid && colorTarget.format == GPUFormat_Undefined && !colorTarget.blend.enabled &&
            colorTarget.blend.color.operation == GPUBlendOp_Add && colorTarget.writeMask == 0;
    const GPURasterizationState rasterization = {0};
    valid = valid && rasterization.cull == GPUCullMode_None;
    const GPUDepthStencilState depthStencil = {0};
    valid = valid && !depthStencil.depthTest && depthStencil.depthCompare == GPUCompareOp_Undefined &&
            !depthStencil.stencilTest && depthStencil.front.compare == GPUCompareOp_Undefined &&
            depthStencil.front.fail == GPUStencilOp_Keep && depthStencil.back.pass == GPUStencilOp_Keep;
    const GPUShaderStage shaderStage = {0};
    valid = valid && shaderStage.code.data == NULL && shaderStage.code.size == 0 && shaderStage.entryPoint == NULL;
    const GPUColorAttachment colorAttachment = {0};
    valid = valid && colorAttachment.renderView == NULL && colorAttachment.load == GPULoadOp_Load &&
            colorAttachment.store == GPUStoreOp_Store;

    const GPUGpuRange range = {0};
    const GPUGpuCpuRange cpuRange = {0};
    const GPUSizeAlign sizeAlign = {0};
    const GPUGpuHeap heap = {0};
    const GPUTextureHeap textureHeap = {0};
    const GPUTimelinePoint timelinePoint = {0};
    const GPUSubmitDesc submit = {0};
    valid = valid && submit.commands.count == 0 && submit.waits.count == 0 && submit.completion.semaphore == NULL &&
            submit.completion.value == 0;
    valid = valid && range.gpu == NULL && range.size == 0 && cpuRange.cpu == NULL && cpuRange.gpu == NULL &&
            cpuRange.size == 0;
    valid = valid && GPUGpuRangeOf(cpuRange).gpu == NULL && GPUGpuRangeOf(cpuRange).size == 0;
    valid = valid && sizeAlign.size == 0 && sizeAlign.align == 0;
    valid = valid && heap.range.cpu == NULL && heap.range.gpu == NULL && heap.range.size == 0 && heap.owner == NULL;
    valid = valid && GPUGpuRangeOfHeap(&heap).gpu == NULL && GPUGpuRangeOfHeap(&heap).size == 0;
    valid = valid && textureHeap.size == 0 && textureHeap.owner == NULL && timelinePoint.semaphore == NULL &&
            timelinePoint.value == 0;
    return valid;
}

// Function signatures. Assigning a mismatched function to a typed pointer is a compile error.
typedef struct ApiFunctions {
    bool (*deviceInit)(GPUDevice*, const GPUDeviceDesc*, GPUError*);
    void (*deviceDestroy)(GPUDevice*);
    const GPUDeviceCaps* (*getDeviceCaps)(const GPUDevice*);
    bool (*supportsTextureFormat)(const GPUDevice*, GPUFormat, GPUTextureUsage);
    GPUExtent2 (*getDrawableExtent)(GPUDevice*);
    GPUError (*setSwapchainFormat)(GPUDevice*, GPUFormat, GPUColorSpace);
    GPUTimelineSemaphore* (*createTimelineSemaphore)(GPUDevice*, uint64_t);
    void (*destroyTimelineSemaphore)(GPUTimelineSemaphore*);
    uint64_t (*timelineCompletedValue)(const GPUTimelineSemaphore*);
    void (*waitTimeline)(GPUTimelinePoint);
    void (*waitIdle)(GPUDevice*);
    GPUSwapchainFrame (*acquire)(GPUCommandBuffer*);
    void (*submitAndPresent)(GPUDevice*, const GPUSubmitDesc*);
    GPUGpuHeap (*createGpuHeap)(GPUDevice*, uint64_t, GPUMemoryType);
    void (*destroyGpuHeap)(const GPUGpuHeap*);
    GPUTextureHeap (*createTextureHeap)(GPUDevice*, uint64_t);
    void (*destroyTextureHeap)(const GPUTextureHeap*);
    GPUSizeAlign (*getTextureSizeAlign)(GPUDevice*, const GPUTextureDesc*);
    GPUTexture* (*createTexture)(GPUCommandBuffer*, const GPUTextureDesc*, const GPUTextureHeap*, uint64_t);
    void (*destroyTexture)(GPUTexture*);
    void (*activateTextureAlias)(GPUCommandBuffer*, GPUTexture*);
    GPURenderView* (*createRenderView)(GPUTexture*, const GPURenderViewDesc*);
    void (*destroyRenderView)(GPURenderView*);
    GPUTextureDescriptorHeap* (*createTextureDescriptorHeap)(GPUDevice*, uint32_t);
    void (*destroyTextureDescriptorHeap)(GPUTextureDescriptorHeap*);
    void (*writeTextureDescriptor)(
        GPUTextureDescriptorHeap*,
        uint32_t,
        const GPUTexture*,
        GPUTextureDescriptorType,
        const GPUTextureDescriptorDesc*
    );
    void (*copyTextureDescriptors)(
        const GPUTextureDescriptorHeap*,
        uint32_t,
        GPUTextureDescriptorHeap*,
        uint32_t,
        uint32_t
    );
    GPUSamplerDescriptorHeap* (*createSamplerDescriptorHeap)(GPUDevice*, uint32_t);
    void (*destroySamplerDescriptorHeap)(GPUSamplerDescriptorHeap*);
    void (*writeSamplerDescriptor)(GPUSamplerDescriptorHeap*, uint32_t, const GPUSamplerDesc*);
    void (*copySamplerDescriptors)(
        const GPUSamplerDescriptorHeap*,
        uint32_t,
        GPUSamplerDescriptorHeap*,
        uint32_t,
        uint32_t
    );
    GPUPSO* (*createGraphicsPso)(GPUDevice*, const GPUGraphicsPSODesc*);
    GPUPSO* (*createMeshPso)(GPUDevice*, const GPUMeshPSODesc*);
    GPUPSO* (*createComputePso)(GPUDevice*, const GPUShaderStage*);
    void (*destroyPso)(GPUPSO*);
    GPUCommandPool* (*createCommandPool)(GPUDevice*, uint32_t);
    void (*destroyCommandPool)(GPUCommandPool*);
    void (*resetCommandPool)(GPUCommandPool*);
    GPUCommandBuffer* (*beginCommands)(GPUCommandPool*);
    void (*endCommands)(GPUCommandBuffer*);
    void (*submit)(GPUDevice*, const GPUSubmitDesc*, uint32_t);
    void (*setTextureDescriptorHeap)(GPUCommandBuffer*, GPUTextureDescriptorHeap*);
    void (*setSamplerDescriptorHeap)(GPUCommandBuffer*, GPUSamplerDescriptorHeap*);
    void (*copyMemory)(GPUCommandBuffer*, GPUGpuRange, GPUGpuRange);
    void (*copyMemoryToTexture)(GPUCommandBuffer*, GPUGpuRange, GPUTexture*, const GPUTextureCopyDesc*);
    void (*copyTextureToMemory)(GPUCommandBuffer*, GPUTexture*, GPUGpuRange, const GPUTextureCopyDesc*);
    void (*barrier)(GPUCommandBuffer*, GPUStage, GPUAccess, GPUStage, GPUAccess);
    void (*writeTimestamp)(GPUCommandBuffer*, uint64_t*, GPUStage);
    void (*readTimestamps)(GPUCommandPool*);
    void (*beginRenderPass)(GPUCommandBuffer*, const GPURenderingDesc*, GPURenderingFlags);
    void (*endRenderPass)(GPUCommandBuffer*);
    void (*setViewport)(GPUCommandBuffer*, const GPUViewport*);
    void (*setScissor)(GPUCommandBuffer*, const GPUScissor*);
    void (*setDepthStencil)(GPUCommandBuffer*, const GPUDepthStencilState*);
    void (*bindPso)(GPUCommandBuffer*, const GPUPSO*);
    void (*draw)(GPUCommandBuffer*, const void*, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*drawIndexed)(
        GPUCommandBuffer*,
        const void*,
        GPUGpuRange,
        GPUIndexType,
        uint32_t,
        uint32_t,
        uint32_t,
        int32_t,
        uint32_t
    );
    void (*drawIndirect)(GPUCommandBuffer*, const void*, GPUGpuRange, uint32_t, uint32_t);
    void (*drawIndexedIndirect)(
        GPUCommandBuffer*,
        const void*,
        GPUGpuRange,
        GPUIndexType,
        GPUGpuRange,
        uint32_t,
        uint32_t
    );
    void (*dispatch)(GPUCommandBuffer*, const void*, GPUExtent3);
    void (*dispatchIndirect)(GPUCommandBuffer*, const void*, GPUGpuRange);
    void (*drawMeshlets)(GPUCommandBuffer*, const void*, GPUExtent3);
    void (*drawMeshletsIndirect)(GPUCommandBuffer*, const void*, GPUGpuRange, uint32_t, uint32_t);
    void (*setErrorCallback)(GPUDevice*, GPUErrorCallback, void*);
} ApiFunctions;

const ApiFunctions apiFunctions = {
    .deviceInit = GPUDeviceInit,
    .deviceDestroy = GPUDeviceDestroy,
    .getDeviceCaps = GPUDeviceGetCaps,
    .supportsTextureFormat = GPUDeviceSupportsTextureFormat,
    .getDrawableExtent = GPUDeviceGetDrawableExtent,
    .setSwapchainFormat = GPUDeviceSetSwapchainFormat,
    .createTimelineSemaphore = GPUTimelineSemaphoreCreate,
    .destroyTimelineSemaphore = GPUTimelineSemaphoreDestroy,
    .timelineCompletedValue = GPUTimelineSemaphoreGetCompletedValue,
    .waitTimeline = GPUTimelineSemaphoreWait,
    .waitIdle = GPUDeviceWaitIdle,
    .acquire = GPUCmdAcquire,
    .submitAndPresent = GPUDeviceSubmitAndPresent,
    .createGpuHeap = GPUGpuHeapCreate,
    .destroyGpuHeap = GPUGpuHeapDestroy,
    .createTextureHeap = GPUTextureHeapCreate,
    .destroyTextureHeap = GPUTextureHeapDestroy,
    .getTextureSizeAlign = GPUTextureGetSizeAlign,
    .createTexture = GPUTextureCreate,
    .destroyTexture = GPUTextureDestroy,
    .activateTextureAlias = GPUCmdActivateTextureAlias,
    .createRenderView = GPURenderViewCreate,
    .destroyRenderView = GPURenderViewDestroy,
    .createTextureDescriptorHeap = GPUTextureDescriptorHeapCreate,
    .destroyTextureDescriptorHeap = GPUTextureDescriptorHeapDestroy,
    .writeTextureDescriptor = GPUTextureDescriptorHeapWrite,
    .copyTextureDescriptors = GPUTextureDescriptorHeapCopy,
    .createSamplerDescriptorHeap = GPUSamplerDescriptorHeapCreate,
    .destroySamplerDescriptorHeap = GPUSamplerDescriptorHeapDestroy,
    .writeSamplerDescriptor = GPUSamplerDescriptorHeapWrite,
    .copySamplerDescriptors = GPUSamplerDescriptorHeapCopy,
    .createGraphicsPso = GPUPSOCreateGraphics,
    .createMeshPso = GPUPSOCreateMesh,
    .createComputePso = GPUPSOCreateCompute,
    .destroyPso = GPUPSODestroy,
    .createCommandPool = GPUCommandPoolCreate,
    .destroyCommandPool = GPUCommandPoolDestroy,
    .resetCommandPool = GPUCommandPoolReset,
    .beginCommands = GPUCmdBegin,
    .endCommands = GPUCmdEnd,
    .submit = GPUDeviceSubmit,
    .setTextureDescriptorHeap = GPUCmdSetTextureDescriptorHeap,
    .setSamplerDescriptorHeap = GPUCmdSetSamplerDescriptorHeap,
    .copyMemory = GPUCmdCopyMemory,
    .copyMemoryToTexture = GPUCmdCopyMemoryToTexture,
    .copyTextureToMemory = GPUCmdCopyTextureToMemory,
    .barrier = GPUCmdBarrier,
    .writeTimestamp = GPUCmdWriteTimestamp,
    .readTimestamps = GPUCommandPoolReadTimestamps,
    .beginRenderPass = GPUCmdBeginRenderPass,
    .endRenderPass = GPUCmdEndRenderPass,
    .setViewport = GPUCmdSetViewport,
    .setScissor = GPUCmdSetScissor,
    .setDepthStencil = GPUCmdSetDepthStencil,
    .bindPso = GPUCmdBindPSO,
    .draw = GPUCmdDraw,
    .drawIndexed = GPUCmdDrawIndexed,
    .drawIndirect = GPUCmdDrawIndirect,
    .drawIndexedIndirect = GPUCmdDrawIndexedIndirect,
    .dispatch = GPUCmdDispatch,
    .dispatchIndirect = GPUCmdDispatchIndirect,
    .drawMeshlets = GPUCmdDrawMeshlets,
    .drawMeshletsIndirect = GPUCmdDrawMeshletsIndirect,
    .setErrorCallback = GPUDeviceSetErrorCallback,
};

static void deleteNothing(void* user) { (void)user; }

// Never called; it only has to compile and link against every entry point.
void compileApiSurface(
    GPUDevice* device,
    GPUTexture* texture,
    GPURenderView* renderView,
    GPUPSO* pso,
    GPUCommandBuffer* commands,
    GPUTimelineSemaphore* semaphore,
    const ApiRoot* root,
    HWND nativeWindow
) {
    const GPUDeviceDesc deviceDesc = {.timestampQueryCount = 256};
    GPUDevice deviceStorage = {0};
    GPUError deviceError;
    const bool deviceCreated = GPUDeviceInit(&deviceStorage, &deviceDesc, &deviceError);
    const GPUDeviceDesc windowDesc = {
        .win32 = {.hwnd = nativeWindow},
        .swapchainFormat = GPUFormat_Bgra8Srgb,
        .desiredSwapchainImageCount = 3,
        .timestampQueryCount = 513,
    };
    GPUDevice windowDevice = {0};
    const bool windowCreated = GPUDeviceInit(&windowDevice, &windowDesc, NULL);
    const GPUDeviceCaps* caps = GPUDeviceGetCaps(device);
    const bool supported = GPUDeviceSupportsTextureFormat(device, GPUFormat_Rgba8Unorm, GPUTextureUsage_Sampled);
    const GPUError formatError = GPUDeviceSetSwapchainFormat(device, GPUFormat_Bgra8Srgb, GPUColorSpace_Srgb);
    GPUDeviceDestroy(device);

    GPUTimelineSemaphore* timeline = GPUTimelineSemaphoreCreate(device, 1);
    const uint64_t completed = GPUTimelineSemaphoreGetCompletedValue(semaphore);
    GPUTimelineSemaphoreWait((GPUTimelinePoint){.semaphore = semaphore, .value = 1});
    GPUTimelineSemaphoreDestroy(timeline);

    const GPUExtent2 drawableExtent = GPUDeviceGetDrawableExtent(device);
    const GPUSwapchainFrame frame = GPUCmdAcquire(commands);
    GPUCmdEnd(commands);
    GPUDeviceSubmitAndPresent(
        device,
        &(GPUSubmitDesc){.commands = GPUCommandBuffers(commands), .completion = {.semaphore = semaphore, .value = 2}}
    );

    const GPUGpuHeap bytes = GPUGpuHeapCreate(device, 64, GPUMemoryType_CpuVisible);
    const GPUGpuHeap gpuOnly = GPUGpuHeapCreate(device, 64, GPUMemoryType_GpuOnly);
    GPUTextureDescriptorHeap* textureDescriptors = GPUTextureDescriptorHeapCreate(device, 64);
    GPUSamplerDescriptorHeap* samplerDescriptors = GPUSamplerDescriptorHeapCreate(device, 64);
    const GPUGpuRange range = GPUGpuRangeOfHeap(&bytes);
    GPUTextureDescriptorHeapCopy(textureDescriptors, 0, textureDescriptors, 1, 1);
    GPUSamplerDescriptorHeapCopy(samplerDescriptors, 0, samplerDescriptors, 1, 1);
    GPUSamplerDescriptorHeapDestroy(samplerDescriptors);
    GPUTextureDescriptorHeapDestroy(textureDescriptors);
    GPUGpuHeapDestroy(&gpuOnly);
    GPUGpuHeapDestroy(&bytes);

    const GPUTextureDesc textureDesc = {0};
    const GPUSizeAlign textureSizeAlign = GPUTextureGetSizeAlign(device, &textureDesc);
    const GPUTextureHeap textureHeap = GPUTextureHeapCreate(device, textureSizeAlign.size);
    GPUTexture* createdTexture = GPUTextureCreate(commands, &textureDesc, &textureHeap, 0);
    GPUCmdActivateTextureAlias(commands, createdTexture);
    const GPURenderViewDesc renderViewDesc = {0};
    GPURenderView* createdRenderView = GPURenderViewCreate(texture, &renderViewDesc);
    const GPUTextureDescriptorDesc descriptorDesc = {0};
    GPUTextureDescriptorHeapWrite(textureDescriptors, 0, texture, GPUTextureDescriptorType_Sampled, &descriptorDesc);
    const GPUSamplerDesc samplerDesc = {0};
    GPUSamplerDescriptorHeapWrite(samplerDescriptors, 0, &samplerDesc);
    GPURenderViewDestroy(createdRenderView);
    GPUTextureDestroy(createdTexture);
    GPUTextureHeapDestroy(&textureHeap);

    const GPUGraphicsPSODesc graphicsDesc = {0};
    GPUPSO* graphics = GPUPSOCreateGraphics(device, &graphicsDesc);
    const GPUMeshPSODesc meshDesc = {0};
    GPUPSO* mesh = GPUPSOCreateMesh(device, &meshDesc);
    const GPUShaderStage computeStage = {.code = GPUBytes(shaderWords), .entryPoint = "computeMain"};
    GPUPSO* compute = GPUPSOCreateCompute(device, &computeStage);
    GPUPSODestroy(graphics);
    GPUPSODestroy(mesh);
    GPUPSODestroy(compute);
    GPUPSODestroy(pso);

    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    GPUCommandBuffer* a = GPUCmdBegin(pool);
    GPUCommandBuffer* b = GPUCmdBegin(pool);
    uint64_t timestamps[2] = {0, 0};
    GPUCmdWriteTimestamp(a, timestamps, GPUStage_AllCommands);
    GPUCmdWriteTimestamp(b, timestamps + 1, GPUStage_Transfer);
    GPUCmdBindPSO(a, pso);
    GPUCmdSetTextureDescriptorHeap(a, textureDescriptors);
    GPUCmdSetSamplerDescriptorHeap(a, samplerDescriptors);
    const GPUColorAttachment color = {.renderView = renderView};
    const GPURenderingDesc rendering = {.colors = {&color, 1}};
    GPUCmdBeginRenderPass(a, &rendering, GPURenderingFlags_None);
    const GPUViewport viewport = {0};
    GPUCmdSetViewport(a, &viewport);
    const GPUScissor scissor = {0};
    GPUCmdSetScissor(a, &scissor);
    const GPUDepthStencilState depthStencil = {0};
    GPUCmdSetDepthStencil(a, &depthStencil);
    GPUCmdDraw(a, root, 3, 1, 0, 0);
    GPUCmdDrawIndexed(a, root, range, GPUIndexType_U32, 3, 1, 0, 0, 0);
    GPUCmdDrawIndirect(a, root, range, 1, 0);
    GPUCmdDrawIndexedIndirect(a, root, range, GPUIndexType_U32, range, 1, 0);
    GPUCmdDrawMeshlets(a, root, (GPUExtent3){1, 1, 1});
    if (caps->indirectMeshDraw) GPUCmdDrawMeshletsIndirect(a, root, range, 1, 0);
    GPUCmdEndRenderPass(a);
    GPUCmdDispatch(a, root, (GPUExtent3){1, 1, 1});
    GPUCmdDispatchIndirect(a, root, range);
    GPUCmdCopyMemory(a, range, range);
    const GPUTextureCopyDesc copy = {0};
    GPUCmdCopyMemoryToTexture(a, range, texture, &copy);
    GPUCmdCopyTextureToMemory(a, texture, range, &copy);
    GPUCmdBarrier(a, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Fragment, GPUAccess_ShaderRead);
    GPUCmdEnd(a);
    GPUCmdEnd(b);
    GPUDeviceSubmit(
        device,
        &(GPUSubmitDesc){
            .commands = GPUCommandBuffers(a, b),
            .waits = GPUTimelinePoints({.semaphore = semaphore, .value = 2}),
            .completion = {.semaphore = semaphore, .value = 3}
        },
        0
    );
    GPUDeviceSubmit(device, &(GPUSubmitDesc){.completion = {.semaphore = semaphore, .value = 4}}, 0);
    GPUDeviceSubmit(
        device,
        &(GPUSubmitDesc){
            .waits = GPUTimelinePoints({.semaphore = semaphore, .value = 4}),
            .completion = {.semaphore = semaphore, .value = 5}
        },
        1
    );
    GPUDeviceWaitIdle(device);
    GPUCommandPoolReadTimestamps(pool);
    GPUCommandPoolReset(pool);
    GPUCommandPoolDestroy(pool);

    GPUBumpAllocator bump = GPUBumpAllocatorCreate(bytes.range);
    const GPUGpuCpuRange bumped = GPUBumpAllocatorAllocate(&bump, 16);
    const GPUGpuCpuRange bumpedAtomic = GPUBumpAllocatorAllocateAtomic(&bump, 16);
    GPUBumpAllocatorReset(&bump);
    GPUDeleteEntry entries[4];
    GPUDeleteQueue deleteQueue = GPUDeleteQueueCreate(semaphore, entries, 4);
    GPUDeleteQueueDefer(&deleteQueue, 1, deleteNothing, NULL);
    GPUDeleteQueueTick(&deleteQueue);
    GPUDeleteQueueDrain(&deleteQueue);

    (void)deviceCreated;
    (void)deviceError;
    (void)windowCreated;
    (void)supported;
    (void)formatError;
    (void)completed;
    (void)drawableExtent;
    (void)frame;
    (void)textureSizeAlign;
    (void)bumped;
    (void)bumpedAtomic;
}

int main(void) {
    const ApiRoot root = {0};
    const GPUByteSpan bytes = GPUBytes(root);
    const bool validBytes = bytes.data == (const void*)&root && bytes.size == sizeof(root);
    const GPUByteSpan pointerBytes = GPUByteSpanOf(&root.textureIndex, sizeof(root.textureIndex));
    const bool validPointerBytes = pointerBytes.data == (const void*)&root.textureIndex && pointerBytes.size == 4;
    uint32_t words[2] = {0x07230203u, 0x00010600u};
    const WordSpan wordSpan = {words, GPUArrayCount(words)};
    words[1] = 42;
    const WordSpan arraySpan = {shaderWords, GPUArrayCount(shaderWords)};
    const WordSpan literalSpan = WORDS(0x07230203u, 0x00010600u);
    const bool validRanges = hasSpirvHeader(wordSpan) && wordSpan.data[1] == 42 && arraySpan.data == shaderWords &&
                             arraySpan.count == 2 && hasSpirvHeader(arraySpan) && literalSpan.count == 2 &&
                             hasSpirvHeader(literalSpan) && hasSpirvHeader(WORDS(0x07230203u, 0x00010600u));
    (void)apiFunctions;
    return validRanges && validBytes && validPointerBytes && validDefaults() && validTextureFormats() ? 0 : 1;
}
