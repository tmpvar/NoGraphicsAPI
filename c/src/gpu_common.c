#include "gpu_api.h"
#include "gpu_internal.h"

static void GPU_Report(const GPUDeviceState* device, const GPUMessage* message) {
    if (device && device->errorCallback) device->errorCallback(device->errorUser, message);
}

static void GPU_ReportKind(const GPUDeviceState* device, GPUMessageKind kind) {
    const GPUMessage message = {.kind = kind, .fatal = false};
    GPU_Report(device, &message);
}

static void GPU_ReportLimit(const GPUDeviceState* device, GPULimit limit) {
    const GPUMessage message = {.kind = GPUMessageKind_LimitReached, .fatal = false, .data.limit = {.limit = limit}};
    GPU_Report(device, &message);
}

static void GPU_ReportVkFailure(
    const GPUDeviceState* device,
    VkResult result,
    const char* operation,
    bool fatal,
    uint64_t byteCount
) {
    const GPUMessage message = {
        .kind = GPUMessageKind_VulkanFailure,
        .fatal = fatal,
        .data.vulkan = {.operation = operation, .result = (int32_t)result, .byteCount = byteCount},
    };
    GPU_Report(device, &message);
}

static GPU_NORETURN void GPU_AbortVkFailure(
    const GPUDeviceState* device,
    VkResult result,
    const char* operation,
    uint64_t byteCount
) {
    GPU_ReportVkFailure(device, result, operation, true, byteCount);
    GPUAssert(result == VK_SUCCESS, "unexpected Vulkan failure");
    abort();
}

static GPUError GPU_ErrorFromVk(
    const GPUDeviceState* device,
    VkResult result,
    const char* operation,
    uint64_t byteCount
) {
    if (result != VK_SUCCESS) GPU_ReportVkFailure(device, result, operation, false, byteCount);
    switch (result) {
    case VK_SUCCESS: return GPUError_None;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_TOO_MANY_OBJECTS: return GPUError_OutOfMemory;
    case VK_ERROR_DEVICE_LOST: return GPUError_DeviceLost;
    case VK_ERROR_LAYER_NOT_PRESENT:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return GPUError_Unsupported;
    default: return GPUError_DriverError;
    }
}

static void GPU_RequireVk(const GPUDeviceState* device, VkResult result, const char* operation, uint64_t byteCount) {
    if (result != VK_SUCCESS) GPU_AbortVkFailure(device, result, operation, byteCount);
}

static void GPU_AssertVk(VkResult result) {
    GPUAssert(result == VK_SUCCESS, "unexpected Vulkan failure");
    (void)result;
}

static void GPU_RequireError(const GPUDeviceState* device, GPUError error, const char* operation) {
    if (error != GPUError_None) {
        const GPUMessage message = {
            .kind = GPUMessageKind_GraphicsFailure,
            .fatal = true,
            .data.graphics = {.operation = operation, .error = error}
        };
        GPU_Report(device, &message);
        GPUAssert(false, "unexpected graphics API failure");
        abort();
    }
}

static const char* GPU_CommonVkResultDescription(int32_t result) {
    switch (result) {
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED (graphics initialization failed)";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY (not enough system memory)";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY (not enough GPU memory or CPU-visible GPU memory)";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS (graphics resource limit reached)";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST (graphics device lost)";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED (GPU memory could not be mapped for CPU access)";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT (required Vulkan layer unavailable)";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT (required Vulkan extension unavailable)";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT (required graphics feature unavailable)";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER (incompatible graphics driver)";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED (required texture or display format unsupported)";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV (the graphics driver rejected the shader)";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN (unspecified graphics driver error)";
    default: return "graphics driver error";
    }
}

static const char* GPU_CommonLimitName(GPULimit limit) {
    switch (limit) {
    case GPULimit_TimelineSemaphores: return "maxTimelineSemaphores";
    case GPULimit_GpuHeaps: return "maxGpuHeaps";
    case GPULimit_TextureHeaps: return "maxTextureHeaps";
    case GPULimit_Textures: return "maxTextures";
    case GPULimit_RenderViews: return "maxRenderViews";
    case GPULimit_TextureDescriptorHeaps: return "maxTextureDescriptorHeaps";
    case GPULimit_SamplerDescriptorHeaps: return "maxSamplerDescriptorHeaps";
    case GPULimit_Psos: return "maxPsos";
    case GPULimit_CommandPools: return "maxCommandPools";
    }
    return "unknown";
}

int GPUMessageFormat(const GPUMessage* message, char* buffer, uint64_t size) {
    GPUAssert(message && buffer && size != 0, "GPUMessageFormat requires a message and a nonempty buffer");
    switch (message->kind) {
    case GPUMessageKind_VulkanFailure:
        if (message->data.vulkan.byteCount)
            return GPU_SNPRINTF(
                buffer,
                size,
                "%s failed: %s (Vulkan result %d, %.2f MiB requested).",
                message->data.vulkan.operation,
                GPU_CommonVkResultDescription(message->data.vulkan.result),
                (int)message->data.vulkan.result,
                (double)message->data.vulkan.byteCount / (1024.0 * 1024.0)
            );
        return GPU_SNPRINTF(
            buffer,
            size,
            "%s failed: %s (Vulkan result %d).",
            message->data.vulkan.operation,
            GPU_CommonVkResultDescription(message->data.vulkan.result),
            (int)message->data.vulkan.result
        );
    case GPUMessageKind_GraphicsFailure:
        return GPU_SNPRINTF(
            buffer,
            size,
            "%s failed (graphics error %u).",
            message->data.graphics.operation,
            (uint32_t)message->data.graphics.error
        );
    case GPUMessageKind_NoMemoryHeap:
        return GPU_SNPRINTF(
            buffer,
            size,
            "No compatible %s GPU memory heap can hold %.2f MiB.",
            message->data.memoryHeap.cpuVisible ? "CPU-visible" : "device-local",
            (double)message->data.memoryHeap.byteCount / (1024.0 * 1024.0)
        );
    case GPUMessageKind_LimitReached:
        return GPU_SNPRINTF(
            buffer,
            size,
            "GPUDeviceLimits::%s exceeded.",
            GPU_CommonLimitName(message->data.limit.limit)
        );
    case GPUMessageKind_LoaderTooOld:
        return GPU_SNPRINTF(buffer, size, "Vulkan 1.4 or newer is required. Update the graphics driver.");
    case GPUMessageKind_LoaderUnavailable:
        return GPU_SNPRINTF(
            buffer,
            size,
            "The Vulkan loader library could not be opened. Install a graphics driver with Vulkan support."
        );
    case GPUMessageKind_NoCompatibleGpu:
        return GPU_SNPRINTF(
            buffer,
            size,
            "No compatible GPU found. Vulkan 1.4, VK_EXT_descriptor_heap, VK_KHR_device_address_commands, "
            "VK_EXT_mesh_shader, "
            "VK_KHR_shader_untyped_pointers, and the required memory, shader, and presentation features are needed."
        );
    case GPUMessageKind_DeviceUnsupported:
        return GPU_SNPRINTF(buffer, size, "Required Vulkan device, memory, or display capabilities are unavailable.");
    case GPUMessageKind_MissingEntryPoints:
        return GPU_SNPRINTF(
            buffer,
            size,
            "The graphics driver did not provide the required Vulkan command entry points."
        );
    case GPUMessageKind_TextureUnsupported:
        return GPU_SNPRINTF(buffer, size, "The GPU cannot create the requested texture format, usage, or extent.");
    case GPUMessageKind_SurfaceCompositeAlphaUnsupported:
        return GPU_SNPRINTF(buffer, size, "The display surface exposes no supported composite alpha mode.");
    case GPUMessageKind_SurfaceExtentUnavailable:
        return GPU_SNPRINTF(buffer, size, "The display surface does not provide a fixed drawable extent.");
    }
    return GPU_SNPRINTF(buffer, size, "Unknown graphics message %d.", (int)message->kind);
}

static bool GPU_HasExtension(const VkExtensionProperties* values, uint32_t count, const char* name) {
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(values[index].extensionName, name) == 0) return true;
    }
    return false;
}

#if defined(GPU_DEBUG)
static bool GPU_HasLayer(const VkLayerProperties* values, uint32_t count, const char* name) {
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(values[index].layerName, name) == 0) return true;
    }
    return false;
}
#endif

#if defined(GPU_DEBUG)
static VKAPI_ATTR VkBool32 VKAPI_CALL GPU_CommonDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData
) {
    (void)types;
    (void)userData;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && callbackData && callbackData->pMessage) {
        // Keep the library callback dependency-free. Applications can still install
        // their own messenger; this one makes validation failures debugger-visible.
        fputs("NoGraphicsAPI validation: ", stderr);
        fputs(callbackData->pMessage, stderr);
        fputc('\n', stderr);
    }
    return VK_FALSE;
}
#endif

static void GPU_PoolInit(GPUPool* pool, uint8_t* storage, uint32_t slotSize, uint32_t capacity) {
    GPUAssert(
        pool && storage && slotSize >= 16 && (slotSize % 16) == 0,
        "pool && storage && slotSize >= 16 && (slotSize % 16) == 0"
    );
    pool->slots = storage;
    pool->slotSize = slotSize;
    pool->capacity = capacity;
    pool->freeHead = 0;
    pool->liveCount = 0;
    pool->lock = 0;
    for (uint32_t index = 0; index < capacity; ++index) {
        const uint32_t next = index + 1;
        memcpy(storage + (uint64_t)index * slotSize, &next, sizeof(next));
    }
}

static void GPU_CommonPoolLock(GPUPool* pool) {
    while (GPU_AtomicExchangeU32(&pool->lock, 1) == 1) {}
}

static void* GPU_PoolAlloc(GPUPool* pool) {
    GPU_CommonPoolLock(pool);
    if (pool->freeHead == pool->capacity) {
        GPU_AtomicStoreU32(&pool->lock, 0);
        return NULL;
    }
    uint8_t* slot = pool->slots + (uint64_t)pool->freeHead * pool->slotSize;
    memcpy(&pool->freeHead, slot, sizeof(uint32_t));
    ++pool->liveCount;
    GPU_AtomicStoreU32(&pool->lock, 0);
    memset(slot, 0, pool->slotSize);
    return slot;
}

static void GPU_PoolFree(GPUPool* pool, void* slot) {
    const uint64_t offset = (uint64_t)((uint8_t*)slot - pool->slots);
    GPUAssert(
        (uint8_t*)slot >= pool->slots && offset < (uint64_t)pool->capacity * pool->slotSize &&
            (offset % pool->slotSize) == 0,
        "slot does not belong to this pool"
    );
    GPU_CommonPoolLock(pool);
    memcpy(slot, &pool->freeHead, sizeof(uint32_t));
    pool->freeHead = (uint32_t)(offset / pool->slotSize);
    --pool->liveCount;
    GPU_AtomicStoreU32(&pool->lock, 0);
}

static VkFormat GPU_ToVkFormat(GPUFormat format) {
    switch (format) {
    case GPUFormat_R8Srgb: return VK_FORMAT_R8_SRGB;
    case GPUFormat_Rg8Srgb: return VK_FORMAT_R8G8_SRGB;
    case GPUFormat_Rgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case GPUFormat_Bgra8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case GPUFormat_Rgba4Unorm: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case GPUFormat_R5g5b5a1Unorm: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case GPUFormat_R5g6b5Unorm: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case GPUFormat_R8Unorm: return VK_FORMAT_R8_UNORM;
    case GPUFormat_Rg8Unorm: return VK_FORMAT_R8G8_UNORM;
    case GPUFormat_Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case GPUFormat_Bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case GPUFormat_R16Unorm: return VK_FORMAT_R16_UNORM;
    case GPUFormat_Rg16Unorm: return VK_FORMAT_R16G16_UNORM;
    case GPUFormat_Rgba16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case GPUFormat_R8Uint: return VK_FORMAT_R8_UINT;
    case GPUFormat_Rg8Uint: return VK_FORMAT_R8G8_UINT;
    case GPUFormat_Rgba8Uint: return VK_FORMAT_R8G8B8A8_UINT;
    case GPUFormat_Bgra8Uint: return VK_FORMAT_B8G8R8A8_UINT;
    case GPUFormat_R16Uint: return VK_FORMAT_R16_UINT;
    case GPUFormat_Rg16Uint: return VK_FORMAT_R16G16_UINT;
    case GPUFormat_Rgba16Uint: return VK_FORMAT_R16G16B16A16_UINT;
    case GPUFormat_R32Uint: return VK_FORMAT_R32_UINT;
    case GPUFormat_Rg32Uint: return VK_FORMAT_R32G32_UINT;
    case GPUFormat_Rgba32Uint: return VK_FORMAT_R32G32B32A32_UINT;
    case GPUFormat_R16Float: return VK_FORMAT_R16_SFLOAT;
    case GPUFormat_Rg16Float: return VK_FORMAT_R16G16_SFLOAT;
    case GPUFormat_Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case GPUFormat_R32Float: return VK_FORMAT_R32_SFLOAT;
    case GPUFormat_Rg32Float: return VK_FORMAT_R32G32_SFLOAT;
    case GPUFormat_Rgba32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case GPUFormat_Rgb10a2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case GPUFormat_Rg11b10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case GPUFormat_D16Unorm: return VK_FORMAT_D16_UNORM;
    case GPUFormat_D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
    case GPUFormat_D32Float: return VK_FORMAT_D32_SFLOAT;
    case GPUFormat_S8Uint: return VK_FORMAT_S8_UINT;
    case GPUFormat_D32FloatS8Uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case GPUFormat_EacRg: return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    case GPUFormat_Astc4x4Srgb: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case GPUFormat_Astc4x4Unorm: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case GPUFormat_Bc3Srgb: return VK_FORMAT_BC3_SRGB_BLOCK;
    case GPUFormat_Bc3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
    case GPUFormat_Bc5Rg: return VK_FORMAT_BC5_UNORM_BLOCK;
    case GPUFormat_Bc6hUfloat: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case GPUFormat_Bc6hSfloat: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case GPUFormat_Bc7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    case GPUFormat_Bc7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case GPUFormat_Count:
    case GPUFormat_Undefined: return VK_FORMAT_UNDEFINED;
    }
    return VK_FORMAT_UNDEFINED;
}

static bool GPU_CompatibleViewFormats(GPUFormat imageFormat, GPUFormat viewFormat) {
    if (imageFormat == viewFormat) return true;
    const GPUTextureFormatInfo image = GPUFormatGetTextureInfo(imageFormat);
    const GPUTextureFormatInfo view = GPUFormatGetTextureInfo(viewFormat);
    if (image.depth || image.stencil || view.depth || view.stencil) return false;
    if (image.blockExtent.x == 1 && view.blockExtent.x == 1)
        return image.bytesPerBlock != 0 && image.bytesPerBlock == view.bytesPerBlock;
    return (imageFormat == GPUFormat_Astc4x4Unorm && viewFormat == GPUFormat_Astc4x4Srgb) ||
           (imageFormat == GPUFormat_Astc4x4Srgb && viewFormat == GPUFormat_Astc4x4Unorm) ||
           (imageFormat == GPUFormat_Bc3Unorm && viewFormat == GPUFormat_Bc3Srgb) ||
           (imageFormat == GPUFormat_Bc3Srgb && viewFormat == GPUFormat_Bc3Unorm) ||
           (imageFormat == GPUFormat_Bc6hUfloat && viewFormat == GPUFormat_Bc6hSfloat) ||
           (imageFormat == GPUFormat_Bc6hSfloat && viewFormat == GPUFormat_Bc6hUfloat) ||
           (imageFormat == GPUFormat_Bc7Unorm && viewFormat == GPUFormat_Bc7Srgb) ||
           (imageFormat == GPUFormat_Bc7Srgb && viewFormat == GPUFormat_Bc7Unorm);
}

static bool GPU_HasDepthAspect(GPUFormat format) { return GPUFormatGetTextureInfo(format).depth; }

static bool GPU_HasStencilAspect(GPUFormat format) { return GPUFormatGetTextureInfo(format).stencil; }

static VkImageAspectFlags GPU_ImageAspects(GPUFormat format) {
    VkImageAspectFlags result = 0;
    if (GPU_HasDepthAspect(format)) result |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (GPU_HasStencilAspect(format)) result |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (result == 0) result = VK_IMAGE_ASPECT_COLOR_BIT;
    return result;
}

static VkImageType GPU_ToVkImageType(GPUTextureType type) {
    switch (type) {
    case GPUTextureType_1D: return VK_IMAGE_TYPE_1D;
    case GPUTextureType_3D: return VK_IMAGE_TYPE_3D;
    case GPUTextureType_Undefined:
    case GPUTextureType_2D:
    case GPUTextureType_Cube:
    case GPUTextureType_2DArray:
    case GPUTextureType_CubeArray: return VK_IMAGE_TYPE_2D;
    }
    return VK_IMAGE_TYPE_MAX_ENUM;
}

static VkImageViewType GPU_ToVkImageViewType(GPUTextureType type) {
    switch (type) {
    case GPUTextureType_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case GPUTextureType_Undefined:
    case GPUTextureType_2D: return VK_IMAGE_VIEW_TYPE_2D;
    case GPUTextureType_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case GPUTextureType_Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case GPUTextureType_2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case GPUTextureType_CubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }
    return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
}

static VkFormatFeatureFlags2 GPU_RequiredFormatFeatures(GPUTextureUsage usage) {
    VkFormatFeatureFlags2 result = 0;
    if (usage & GPUTextureUsage_Sampled) result |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    if (usage & GPUTextureUsage_Storage)
        result |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    if (usage & GPUTextureUsage_ColorAttachment) result |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
    if (usage & GPUTextureUsage_DepthStencilAttachment) result |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & GPUTextureUsage_TransferSource) result |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
    if (usage & GPUTextureUsage_TransferDestination) result |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    return result;
}

#define GPU_COMMON_KNOWN_TEXTURE_USAGE_BITS                                                \
    (GPUTextureUsage_Sampled | GPUTextureUsage_Storage | GPUTextureUsage_ColorAttachment | \
     GPUTextureUsage_DepthStencilAttachment | GPUTextureUsage_TransferSource | GPUTextureUsage_TransferDestination)

#if defined(GPU_DEBUG)
static bool GPU_HasValidTextureUsageBits(GPUTextureUsage usage) {
    return usage != 0 && (usage & ~GPU_COMMON_KNOWN_TEXTURE_USAGE_BITS) == 0;
}
#endif

static GPUTextureCompression GPU_TextureCompression(GPUFormat format) {
    switch (format) {
    case GPUFormat_Bc3Srgb:
    case GPUFormat_Bc3Unorm:
    case GPUFormat_Bc5Rg:
    case GPUFormat_Bc6hUfloat:
    case GPUFormat_Bc6hSfloat:
    case GPUFormat_Bc7Srgb:
    case GPUFormat_Bc7Unorm: return GPUTextureCompression_Bc;
    case GPUFormat_Astc4x4Srgb:
    case GPUFormat_Astc4x4Unorm: return GPUTextureCompression_Astc;
    case GPUFormat_EacRg: return GPUTextureCompression_Etc2;
    default: return GPUTextureCompression_None;
    }
}

static VkFormatFeatureFlags2 GPU_OptimalFormatFeatures(const GPUDeviceState* state, GPUFormat format) {
    VkFormatProperties3 properties3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &properties3};
    state->fn.vkGetPhysicalDeviceFormatProperties2(state->physicalDevice, GPU_ToVkFormat(format), &properties2);
    return properties3.optimalTilingFeatures;
}

static VkBlendFactor GPU_ToVkBlendFactor(GPUBlendFactor factor) {
    switch (factor) {
    case GPUBlendFactor_Zero: return VK_BLEND_FACTOR_ZERO;
    case GPUBlendFactor_One: return VK_BLEND_FACTOR_ONE;
    case GPUBlendFactor_SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case GPUBlendFactor_OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GPUBlendFactor_DestinationColor: return VK_BLEND_FACTOR_DST_COLOR;
    case GPUBlendFactor_OneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GPUBlendFactor_SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GPUBlendFactor_OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GPUBlendFactor_DestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case GPUBlendFactor_OneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GPUBlendFactor_SourceAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_MAX_ENUM;
}

static VkPipelineStageFlags2 GPU_ToVkStages(GPUStage stages) {
    VkPipelineStageFlags2 result = 0;
    if (stages & GPUStage_Indirect) result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (stages & GPUStage_IndexInput) result |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    if (stages & GPUStage_Vertex) result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (stages & GPUStage_Task) result |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
    if (stages & GPUStage_Mesh) result |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    if (stages & GPUStage_DepthStencilTests)
        result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (stages & GPUStage_Fragment) result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (stages & GPUStage_ColorOutput) result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (stages & GPUStage_Compute) result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (stages & GPUStage_Transfer) result |= VK_PIPELINE_STAGE_2_COPY_BIT;
    if (stages & GPUStage_Host) result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    if (stages & GPUStage_AllCommands) result |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return result;
}

static VkAccessFlags2 GPU_ToVkAccess(GPUAccess accesses) {
    VkAccessFlags2 result = 0;
    if (accesses & GPUAccess_TransferRead) result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (accesses & GPUAccess_TransferWrite) result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (accesses & GPUAccess_ShaderRead)
        result |=
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
    if (accesses & GPUAccess_ShaderWrite) result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (accesses & GPUAccess_ColorRead) result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (accesses & GPUAccess_ColorWrite) result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (accesses & GPUAccess_DepthStencilRead) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (accesses & GPUAccess_DepthStencilWrite) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (accesses & GPUAccess_IndirectRead) result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (accesses & GPUAccess_IndexRead) result |= VK_ACCESS_2_INDEX_READ_BIT;
    if (accesses & GPUAccess_HostRead) result |= VK_ACCESS_2_HOST_READ_BIT;
    if (accesses & GPUAccess_DescriptorRead)
        result |= VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT | VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT;
    return result;
}

// GPU_ToVkCompareOp subtracts the Undefined slot.
GPUStaticAssert(
    GPUCompareOp_Never - 1 == (int)VK_COMPARE_OP_NEVER && GPUCompareOp_Less - 1 == (int)VK_COMPARE_OP_LESS &&
        GPUCompareOp_Equal - 1 == (int)VK_COMPARE_OP_EQUAL &&
        GPUCompareOp_LessEqual - 1 == (int)VK_COMPARE_OP_LESS_OR_EQUAL &&
        GPUCompareOp_Greater - 1 == (int)VK_COMPARE_OP_GREATER &&
        GPUCompareOp_NotEqual - 1 == (int)VK_COMPARE_OP_NOT_EQUAL &&
        GPUCompareOp_GreaterEqual - 1 == (int)VK_COMPARE_OP_GREATER_OR_EQUAL &&
        GPUCompareOp_Always - 1 == (int)VK_COMPARE_OP_ALWAYS,
    compare_op_order
);

static VkCompareOp GPU_ToVkCompareOp(GPUCompareOp op) {
    GPUAssert(op != GPUCompareOp_Undefined, "compare op must be resolved before conversion");
    return (VkCompareOp)(op - 1);
}

static VkFilter GPU_ToVkFilter(GPUFilter filter) {
    return filter == GPUFilter_Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

static VkSamplerMipmapMode GPU_ToVkMipmapMode(GPUFilter filter) {
    return filter == GPUFilter_Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

static bool GPU_IsUsableMemoryType(const VkPhysicalDeviceMemoryProperties* properties, uint32_t index) {
    const VkMemoryType* type = &properties->memoryTypes[index];
    if ((type->propertyFlags & GPU_FORBIDDEN_MEMORY_PROPERTIES) != 0) return false;
    return (properties->memoryHeaps[type->heapIndex].flags & VK_MEMORY_HEAP_TILE_MEMORY_BIT_QCOM) == 0;
}

static void GPU_RecordImageBarriers(
    const GPUDeviceState* state,
    VkCommandBuffer commandBuffer,
    const VkImageMemoryBarrier2* barriers,
    uint32_t count
) {
    GPUAssert(commandBuffer && barriers && count != 0, "commandBuffer && barriers && count != 0");
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = count,
        .pImageMemoryBarriers = barriers
    };
    state->fn.vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

static void GPU_RecordBarrier(
    const GPUDeviceState* state,
    VkCommandBuffer commandBuffer,
    GPUStage before,
    GPUAccess beforeAccess,
    GPUStage after,
    GPUAccess afterAccess
) {
    const VkMemoryBarrier2 memoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = GPU_ToVkStages(before),
        .srcAccessMask = GPU_ToVkAccess(beforeAccess),
        .dstStageMask = GPU_ToVkStages(after),
        .dstAccessMask = GPU_ToVkAccess(afterAccess),
    };
    const VkDependencyInfo dependency =
        {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier};
    state->fn.vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

static void GPU_MakeHeapBindInfo(
    GPUGpuRange heap,
    VkDeviceSize reservedAlignment,
    VkDeviceSize reservedSize,
    VkBindHeapInfoEXT* output
) {
    const VkDeviceSize reservedOffset = GPU_AlignUp(heap.size, reservedAlignment);
    *output = (VkBindHeapInfoEXT){
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .heapRange = {.address = (VkDeviceAddress)(uintptr_t)heap.gpu, .size = reservedOffset + reservedSize},
        .reservedRangeOffset = reservedOffset,
        .reservedRangeSize = reservedSize,
    };
}

static VkDeviceMemoryImageCopyKHR GPU_MakeTextureCopyRegion(
    const GPUTexture* texture,
    const GPUTextureCopyDesc* copy,
    GPUGpuRange memory
) {
    uint32_t mipWidth = texture->width >> copy->mipLevel;
    uint32_t mipHeight = texture->height >> copy->mipLevel;
    uint32_t mipDepth = texture->depth >> copy->mipLevel;
    if (mipWidth == 0) mipWidth = 1;
    if (mipHeight == 0) mipHeight = 1;
    if (mipDepth == 0) mipDepth = 1;

    const uint32_t width = copy->extent.x == 0 ? mipWidth - copy->offset.x : copy->extent.x;
    const GPUTextureFormatInfo formatInfo = GPUFormatGetTextureInfo(texture->format);
    const uint64_t rowPitch = copy->rowPitchBytes == 0
                                  ? GPU_DivideUp(width, formatInfo.blockExtent.x) * formatInfo.bytesPerBlock
                                  : copy->rowPitchBytes;
    GPUAssert(copy->slicePitchBytes == 0 || rowPitch != 0, "slice pitch conversion requires a non-zero row pitch");
    return (VkDeviceMemoryImageCopyKHR){
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .addressRange = {.address = (VkDeviceAddress)(uintptr_t)memory.gpu, .size = memory.size},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .addressRowLength = (uint32_t)(copy->rowPitchBytes / formatInfo.bytesPerBlock * formatInfo.blockExtent.x),
        .addressImageHeight =
            copy->slicePitchBytes == 0 ? 0u : (uint32_t)(copy->slicePitchBytes / rowPitch * formatInfo.blockExtent.y),
        .imageSubresource =
            {
                .aspectMask = GPU_ImageAspects(texture->format),
                .mipLevel = copy->mipLevel,
                .baseArrayLayer = copy->baseSlice,
                .layerCount = copy->sliceCount == 0 ? texture->layerCount - copy->baseSlice : copy->sliceCount,
            },
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageOffset = {.x = (int32_t)copy->offset.x, .y = (int32_t)copy->offset.y, .z = (int32_t)copy->offset.z},
        .imageExtent = {
            .width = width,
            .height = copy->extent.y == 0 ? mipHeight - copy->offset.y : copy->extent.y,
            .depth = copy->extent.z == 0 ? mipDepth - copy->offset.z : copy->extent.z,
        },
    };
}
