#include "gpu_api.h"
#include "gpu_internal.h"

static uint32_t GPU_DevicePopcount(uint32_t value) {
    // Keep the core library usable on x86-64 CPUs without POPCNT.
    value -= (value >> 1) & 0x55555555u;
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    value = (value + (value >> 4)) & 0x0f0f0f0fu;
    return (value * 0x01010101u) >> 24;
}

static uint32_t GPU_DeviceCountLeadingZeros(uint32_t value) {
    if (value == 0) return 32;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanReverse(&index, value);
    return 31u - index;
#else
    return (uint32_t)__builtin_clz(value);
#endif
}

static bool GPU_FindMemoryType(
    const GPUDeviceState* device,
    uint32_t bits,
    VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred,
    VkDeviceSize minimumHeapSize,
    uint32_t* output,
    VkMemoryPropertyFlags avoided
) {
    const VkPhysicalDeviceMemoryProperties* properties = &device->memoryProperties;
    bool hasBest = false;
    bool bestIsAvoided = false;
    uint32_t best = 0;
    uint32_t bestScore = 0;
    VkDeviceSize bestHeapSize = 0;
    for (uint32_t i = 0; i < properties->memoryTypeCount; ++i) {
        if ((bits & (1u << i)) == 0) continue;
        const VkMemoryPropertyFlags flags = properties->memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if (!GPU_IsUsableMemoryType(properties, i)) continue;
        const VkMemoryHeap* heap = &properties->memoryHeaps[properties->memoryTypes[i].heapIndex];
        if (heap->size < minimumHeapSize) {
            continue;
        }
        const bool isAvoided = (flags & avoided) != 0;
        const uint32_t score = GPU_DevicePopcount(flags & preferred);
        if (!hasBest || (bestIsAvoided && !isAvoided) ||
            (bestIsAvoided == isAvoided && (score > bestScore || (score == bestScore && heap->size > bestHeapSize)))) {
            best = i;
            hasBest = true;
            bestIsAvoided = isAvoided;
            bestScore = score;
            bestHeapSize = heap->size;
        }
    }
    if (!hasBest) return false;
    *output = best;
    return true;
}

static bool GPU_CreateBackingBuffer(
    const GPUDeviceState* device,
    GPUBackingBuffer* output,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred,
    VkMemoryPropertyFlags avoided
) {
    memset(output, 0, sizeof(*output));
    GPUBackingBuffer result = {0};
    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
    };
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateBuffer(device->device, &bufferInfo, NULL, &result.buffer),
            "vkCreateBuffer",
            size
        ) != GPUError_None)
        return false;

    VkMemoryRequirements requirements = {0};
    device->fn.vkGetBufferMemoryRequirements(device->device, result.buffer, &requirements);
    uint32_t memoryType = 0;
    if (!GPU_FindMemoryType(
            device,
            requirements.memoryTypeBits,
            required,
            preferred,
            requirements.size,
            &memoryType,
            avoided
        )) {
        const GPUMessage message = {
            .kind = GPUMessageKind_NoMemoryHeap,
            .fatal = false,
            .data.memoryHeap =
                {.cpuVisible = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0, .byteCount = requirements.size},
        };
        GPU_Report(device, &message);
        device->fn.vkDestroyBuffer(device->device, result.buffer, NULL);
        return false;
    }

    const VkMemoryAllocateFlagsInfo flagsInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flagsInfo,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType,
    };
    const char* operation = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "vkAllocateMemory (CPU-visible GPU heap)"
                                                                             : "vkAllocateMemory (GPU-only heap)";
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkAllocateMemory(device->device, &allocateInfo, NULL, &result.memory),
            operation,
            requirements.size
        ) != GPUError_None) {
        device->fn.vkDestroyBuffer(device->device, result.buffer, NULL);
        return false;
    }
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkBindBufferMemory(device->device, result.buffer, result.memory, 0),
            "vkBindBufferMemory",
            requirements.size
        ) != GPUError_None) {
        device->fn.vkDestroyBuffer(device->device, result.buffer, NULL);
        device->fn.vkFreeMemory(device->device, result.memory, NULL);
        return false;
    }

    if ((required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        if (GPU_ErrorFromVk(
                device,
                device->fn.vkMapMemory(device->device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped),
                "vkMapMemory",
                requirements.size
            ) != GPUError_None) {
            device->fn.vkDestroyBuffer(device->device, result.buffer, NULL);
            device->fn.vkFreeMemory(device->device, result.memory, NULL);
            return false;
        }
    }

    const VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = result.buffer,
    };
    result.address = device->fn.vkGetBufferDeviceAddress(device->device, &addressInfo);
    *output = result;
    return true;
}

static void GPU_DestroyBackingBuffer(const GPUDeviceState* device, GPUBackingBuffer* backing) {
    if (backing->mapped) device->fn.vkUnmapMemory(device->device, backing->memory);
    device->fn.vkDestroyBuffer(device->device, backing->buffer, NULL);
    device->fn.vkFreeMemory(device->device, backing->memory, NULL);
}

static GPUGpuHeap GPU_AllocateDescriptorHeap(GPUDeviceState* device, VkDeviceSize size, GPUDescriptorHeapType type) {
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT* heapProperties = &device->heapProperties;
    const VkDeviceSize resourceAlignment =
        heapProperties->imageDescriptorAlignment > heapProperties->bufferDescriptorAlignment
            ? heapProperties->imageDescriptorAlignment
            : heapProperties->bufferDescriptorAlignment;
    const VkDeviceSize reservedAlignment =
        type == GPUDescriptorHeapType_Texture ? resourceAlignment : heapProperties->samplerDescriptorAlignment;
    const VkDeviceSize heapAlignment = type == GPUDescriptorHeapType_Texture ? heapProperties->resourceHeapAlignment
                                                                             : heapProperties->samplerHeapAlignment;
    const VkDeviceSize reservedSize = type == GPUDescriptorHeapType_Texture
                                          ? heapProperties->minResourceHeapReservedRange
                                          : heapProperties->minSamplerHeapReservedRange;

    const VkDeviceSize reservedOffset = GPU_AlignUp(size, reservedAlignment);
    const VkDeviceSize bindSize = reservedOffset + reservedSize;
    const VkDeviceSize allocationAlignment =
        heapAlignment > GPU_ALLOCATION_ALIGNMENT ? heapAlignment : GPU_ALLOCATION_ALIGNMENT;
    const VkDeviceSize alignmentPadding = allocationAlignment - 1;
    const VkDeviceSize backingSize = bindSize + alignmentPadding;

    GPUGpuHeapOwner* heap = (GPUGpuHeapOwner*)GPU_PoolAlloc(&device->gpuHeapPool);
    if (!heap) {
        GPU_ReportLimit(device, GPULimit_GpuHeaps);
        return (GPUGpuHeap){0};
    }
    heap->state = device;
    if (!GPU_CreateBackingBuffer(
            device,
            &heap->backing,
            backingSize,
            GPU_UNIVERSAL_BUFFER_USAGE | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT,
            GPU_CPU_VISIBLE_MEMORY_PROPERTIES,
            0,
            0
        )) {
        GPU_PoolFree(&device->gpuHeapPool, heap);
        return (GPUGpuHeap){0};
    }

    const VkDeviceAddress gpuAddress = GPU_AlignUp(heap->backing.address, allocationAlignment);
    const VkDeviceSize allocationOffset = gpuAddress - heap->backing.address;
    return (GPUGpuHeap){
        .range =
            {
                .cpu = (uint8_t*)heap->backing.mapped + allocationOffset,
                .gpu = (uint8_t*)(uintptr_t)gpuAddress,
                .size = size,
            },
        .owner = heap,
    };
}

static VkMemoryRequirements GPU_DeviceBufferMemoryRequirements(GPUDeviceState* device, VkBufferUsageFlags usage) {
    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 1,
        .usage = usage,
        .sharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
    };
    const VkDeviceBufferMemoryRequirements requirementsInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
        .pCreateInfo = &bufferInfo,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    device->fn.vkGetDeviceBufferMemoryRequirements(device->device, &requirementsInfo, &requirements);
    return requirements.memoryRequirements;
}

static bool GPU_SupportsGpuHeapMemory(GPUDeviceState* device) {
    const VkMemoryRequirements ordinary = GPU_DeviceBufferMemoryRequirements(device, GPU_UNIVERSAL_BUFFER_USAGE);
    uint32_t memoryType = 0;
    if (!GPU_FindMemoryType(
            device,
            ordinary.memoryTypeBits,
            GPU_CPU_VISIBLE_MEMORY_PROPERTIES,
            0,
            ordinary.size,
            &memoryType,
            0
        ) ||
        !GPU_FindMemoryType(
            device,
            ordinary.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            0,
            ordinary.size,
            &memoryType,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        )) {
        return false;
    }
    const VkMemoryRequirements descriptor = GPU_DeviceBufferMemoryRequirements(
        device,
        GPU_UNIVERSAL_BUFFER_USAGE | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT
    );
    return GPU_FindMemoryType(
        device,
        descriptor.memoryTypeBits,
        GPU_CPU_VISIBLE_MEMORY_PROPERTIES,
        0,
        descriptor.size,
        &memoryType,
        0
    );
}

static bool GPU_DeviceFitsImageFormatProperties(
    const VkImageCreateInfo* imageInfo,
    const VkImageFormatProperties* properties
) {
    return imageInfo->extent.width <= properties->maxExtent.width &&
           imageInfo->extent.height <= properties->maxExtent.height &&
           imageInfo->extent.depth <= properties->maxExtent.depth && imageInfo->mipLevels <= properties->maxMipLevels &&
           imageInfo->arrayLayers <= properties->maxArrayLayers;
}

static bool GPU_SupportsImageCreateInfo(
    GPUDeviceState* device,
    const VkImageCreateInfo* imageInfo,
    VkImageFormatProperties* output
) {
    const VkPhysicalDeviceImageFormatInfo2 formatInfo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = imageInfo->format,
        .type = imageInfo->imageType,
        .tiling = imageInfo->tiling,
        .usage = imageInfo->usage,
        .flags = imageInfo->flags,
    };
    VkImageFormatProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    const VkResult result =
        device->fn.vkGetPhysicalDeviceImageFormatProperties2(device->physicalDevice, &formatInfo, &properties);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) return false;
    if (GPU_ErrorFromVk(device, result, "vkGetPhysicalDeviceImageFormatProperties2", 0) != GPUError_None) return false;
    if (output) *output = properties.imageFormatProperties;
    return GPU_DeviceFitsImageFormatProperties(imageInfo, &properties.imageFormatProperties);
}

static VkMemoryRequirements GPU_ImageMemoryRequirements(GPUDeviceState* device, const VkImageCreateInfo* imageInfo) {
    const VkDeviceImageMemoryRequirements requirementsInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
        .pCreateInfo = imageInfo,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    device->fn.vkGetDeviceImageMemoryRequirements(device->device, &requirementsInfo, &requirements);
    return requirements.memoryRequirements;
}

static void GPU_DeviceIncludeTextureHeapAlignment(GPUDeviceState* device, const VkMemoryRequirements* requirements) {
    if (requirements->alignment > device->textureHeapAlignment) device->textureHeapAlignment = requirements->alignment;
}

static bool GPU_SelectTextureMemoryType(GPUDeviceState* device) {
    const VkFormatFeatureFlags2 colorFeatures = device->formatFeatures[GPUFormat_Rgba8Unorm];
    if ((colorFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) == 0) return false;
    // Probes cover DCC-capable color, broad 3D, and sampled depth layouts.
    // Resource Memory Association makes the color mask common to ordinary optimal-tiled images. Intersect every public
    // depth/stencil format below.
    const uint32_t probe2dSize = device->physicalProperties.limits.maxImageDimension2D < 2048
                                     ? device->physicalProperties.limits.maxImageDimension2D
                                     : 2048;
    VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((colorFeatures & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) != 0)
        colorUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {.width = probe2dSize, .height = probe2dSize, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = colorUsage,
        .sharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!GPU_SupportsImageCreateInfo(device, &imageInfo, NULL)) {
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!GPU_SupportsImageCreateInfo(device, &imageInfo, NULL)) return false;
    }
    const VkMemoryRequirements colorRequirements = GPU_ImageMemoryRequirements(device, &imageInfo);
    uint32_t memoryTypeBits = colorRequirements.memoryTypeBits;
    GPU_DeviceIncludeTextureHeapAlignment(device, &colorRequirements);

    const VkFormatFeatureFlags2 broadFeatures = device->formatFeatures[GPUFormat_Rgba32Float];
    const VkFormatFeatureFlags2 broadRequiredFeatures = GPU_RequiredFormatFeatures(
        GPUTextureUsage_Sampled | GPUTextureUsage_Storage | GPUTextureUsage_TransferDestination
    );
    if ((broadFeatures & broadRequiredFeatures) == broadRequiredFeatures) {
        const uint32_t probe3dSize = device->physicalProperties.limits.maxImageDimension3D < 2048
                                         ? device->physicalProperties.limits.maxImageDimension3D
                                         : 2048;
        imageInfo.imageType = VK_IMAGE_TYPE_3D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent =
            (VkExtent3D){.width = probe3dSize, .height = probe3dSize, .depth = probe3dSize < 4 ? probe3dSize : 4};
        imageInfo.mipLevels = 32u - GPU_DeviceCountLeadingZeros(probe3dSize);
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (GPU_SupportsImageCreateInfo(device, &imageInfo, NULL)) {
            const VkMemoryRequirements requirements = GPU_ImageMemoryRequirements(device, &imageInfo);
            GPU_DeviceIncludeTextureHeapAlignment(device, &requirements);
        }
    }

    static const GPUFormat depthStencilFormats[] = {
        GPUFormat_D16Unorm,
        GPUFormat_D24UnormS8Uint,
        GPUFormat_D32Float,
        GPUFormat_S8Uint,
        GPUFormat_D32FloatS8Uint,
    };
    const VkFormatFeatureFlags2 storageFeatures = GPU_RequiredFormatFeatures(GPUTextureUsage_Storage);
    for (uint32_t index = 0; index < GPUArrayCount(depthStencilFormats); ++index) {
        const GPUFormat format = depthStencilFormats[index];
        const VkFormatFeatureFlags2 features = device->formatFeatures[format];
        const bool combined = GPU_HasDepthAspect(format) && GPU_HasStencilAspect(format);
        VkImageUsageFlags compatibilityUsage = 0;
        if ((features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) != 0)
            compatibilityUsage = VK_IMAGE_USAGE_SAMPLED_BIT;
        else if ((features & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
            compatibilityUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        else if ((features & storageFeatures) == storageFeatures)
            compatibilityUsage = VK_IMAGE_USAGE_STORAGE_BIT;
        else if (!combined && (features & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT) != 0)
            compatibilityUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        else if (!combined && (features & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT) != 0)
            compatibilityUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (compatibilityUsage != 0) {
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = GPU_ToVkFormat(format);
            imageInfo.extent = (VkExtent3D){.width = 1, .height = 1, .depth = 1};
            imageInfo.mipLevels = 1;
            imageInfo.usage = compatibilityUsage;
            VkImageFormatProperties compatibilityProperties = {0};
            if (GPU_SupportsImageCreateInfo(device, &imageInfo, &compatibilityProperties)) {
                imageInfo.extent = (VkExtent3D){.width = 512, .height = 512, .depth = 1};
                if ((features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) != 0 &&
                    (features & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
                    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

                bool supported = imageInfo.usage == compatibilityUsage
                                     ? GPU_DeviceFitsImageFormatProperties(&imageInfo, &compatibilityProperties)
                                     : GPU_SupportsImageCreateInfo(device, &imageInfo, NULL);
                if (!supported && imageInfo.usage != compatibilityUsage) {
                    imageInfo.usage = compatibilityUsage;
                    supported = GPU_DeviceFitsImageFormatProperties(&imageInfo, &compatibilityProperties);
                }
                if (!supported) {
                    imageInfo.extent = (VkExtent3D){.width = 1, .height = 1, .depth = 1};
                    imageInfo.usage = compatibilityUsage;
                }

                const VkMemoryRequirements requirements = GPU_ImageMemoryRequirements(device, &imageInfo);
                memoryTypeBits &= requirements.memoryTypeBits;
                GPU_DeviceIncludeTextureHeapAlignment(device, &requirements);
            }
        }
    }
    return GPU_FindMemoryType(
        device,
        memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        0,
        1,
        &device->textureMemoryType,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    );
}

static uint64_t GPU_DeviceLayoutRegion(uint64_t* offset, uint64_t size) {
    const uint64_t start = GPU_AlignUp(*offset, GPU_ALLOCATION_ALIGNMENT);
    *offset = start + size;
    return start;
}

static GPUDeviceDesc GPU_ResolveDeviceDesc(const GPUDeviceDesc* desc) {
    GPUDeviceDesc resolved = *desc;
    GPUDeviceLimits* limits = &resolved.limits;
    if (!resolved.desiredSwapchainImageCount) resolved.desiredSwapchainImageCount = 2;
    if (!resolved.desiredQueueCount) resolved.desiredQueueCount = 1;
    if (!limits->maxTimelineSemaphores) limits->maxTimelineSemaphores = 64;
    if (!limits->maxGpuHeaps) limits->maxGpuHeaps = 256;
    if (!limits->maxTextureHeaps) limits->maxTextureHeaps = 64;
    if (!limits->maxTextures) limits->maxTextures = 4096;
    if (!limits->maxRenderViews) limits->maxRenderViews = 4096;
    if (!limits->maxTextureDescriptorHeaps) limits->maxTextureDescriptorHeaps = 8;
    if (!limits->maxSamplerDescriptorHeaps) limits->maxSamplerDescriptorHeaps = 8;
    if (!limits->maxPsos) limits->maxPsos = 1024;
    if (!limits->maxCommandPools) limits->maxCommandPools = 32;
    if (!limits->maxCommandBuffersPerPool) limits->maxCommandBuffersPerPool = 64;
    if (!limits->maxSubmitCommandBuffers) limits->maxSubmitCommandBuffers = 64;
    if (!limits->maxSubmitWaits) limits->maxSubmitWaits = 16;
    if (!limits->maxRetiredSwapchainImages) limits->maxRetiredSwapchainImages = 64;
    return resolved;
}

static void GPU_ComputeDeviceLayout(const GPUDeviceDesc* desc, GPUDeviceLayout* layout) {
    const GPUDeviceLimits* limits = &desc->limits;
    uint64_t offset = 0;
    layout->queueCapacity = desc->desiredQueueCount + desc->desiredComputeQueueCount + desc->desiredCopyQueueCount;
    const uint64_t commandBufferSize =
        sizeof(GPUCommandBuffer) + (uint64_t)desc->timestampQueryCount * (sizeof(uint64_t*) + sizeof(uint64_t));
    layout->commandPoolSlotSize =
        GPU_PoolSlotSize(sizeof(GPUCommandPool) + (uint64_t)limits->maxCommandBuffersPerPool * commandBufferSize);
    layout->queues = GPU_DeviceLayoutRegion(&offset, (uint64_t)layout->queueCapacity * sizeof(GPUQueue));
    layout->commandSubmitInfos = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)layout->queueCapacity * limits->maxSubmitCommandBuffers * sizeof(VkCommandBufferSubmitInfo)
    );
    layout->waitSubmitInfos = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)layout->queueCapacity * (limits->maxSubmitWaits + 1) * sizeof(VkSemaphoreSubmitInfo)
    );
    layout->retiredImages = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)limits->maxRetiredSwapchainImages * sizeof(GPUDeferredSwapchainImage)
    );
    layout->timelineSemaphores = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)GPU_PoolSlotSize(sizeof(GPUTimelineSemaphore)) * limits->maxTimelineSemaphores
    );
    layout->gpuHeaps =
        GPU_DeviceLayoutRegion(&offset, (uint64_t)GPU_PoolSlotSize(sizeof(GPUGpuHeapOwner)) * limits->maxGpuHeaps);
    layout->textureHeaps = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)GPU_PoolSlotSize(sizeof(GPUTextureHeapOwner)) * limits->maxTextureHeaps
    );
    layout->textures =
        GPU_DeviceLayoutRegion(&offset, (uint64_t)GPU_PoolSlotSize(sizeof(GPUTexture)) * limits->maxTextures);
    layout->renderViews =
        GPU_DeviceLayoutRegion(&offset, (uint64_t)GPU_PoolSlotSize(sizeof(GPURenderView)) * limits->maxRenderViews);
    layout->textureDescriptorHeaps = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)GPU_PoolSlotSize(sizeof(GPUTextureDescriptorHeap)) * limits->maxTextureDescriptorHeaps
    );
    layout->samplerDescriptorHeaps = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)GPU_PoolSlotSize(sizeof(GPUSamplerDescriptorHeap)) * limits->maxSamplerDescriptorHeaps
    );
    layout->psos = GPU_DeviceLayoutRegion(&offset, (uint64_t)GPU_PoolSlotSize(sizeof(GPUPSO)) * limits->maxPsos);
    layout->commandPools =
        GPU_DeviceLayoutRegion(&offset, (uint64_t)layout->commandPoolSlotSize * limits->maxCommandPools);
    layout->total = GPU_AlignUp(offset, GPU_ALLOCATION_ALIGNMENT);
}

static GPUError GPU_DeviceEnumerateDeviceExtensions(
    const GPUDeviceState* device,
    VkPhysicalDevice physicalDevice,
    VkExtensionProperties* values,
    uint32_t capacity,
    uint32_t* count
) {
    *count = capacity;
    const VkResult result = device->fn.vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, count, values);
    return result == VK_INCOMPLETE ? GPUError_Unsupported
                                   : GPU_ErrorFromVk(device, result, "vkEnumerateDeviceExtensionProperties", 0);
}

static GPUError GPU_DeviceEnumerateInstanceExtensions(
    const GPUDeviceState* device,
    VkExtensionProperties* values,
    uint32_t capacity,
    uint32_t* count
) {
    *count = capacity;
    const VkResult result = device->fn.vkEnumerateInstanceExtensionProperties(NULL, count, values);
    return result == VK_INCOMPLETE ? GPUError_Unsupported
                                   : GPU_ErrorFromVk(device, result, "vkEnumerateInstanceExtensionProperties", 0);
}

#if defined(GPU_DEBUG)
static GPUError GPU_DeviceEnumerateInstanceLayers(
    const GPUDeviceState* device,
    VkLayerProperties* values,
    uint32_t capacity,
    uint32_t* count
) {
    *count = capacity;
    const VkResult result = device->fn.vkEnumerateInstanceLayerProperties(count, values);
    return result == VK_INCOMPLETE ? GPUError_Unsupported
                                   : GPU_ErrorFromVk(device, result, "vkEnumerateInstanceLayerProperties", 0);
}
#endif

static GPUError GPU_DeviceEnumeratePhysicalDevices(
    const GPUDeviceState* device,
    VkPhysicalDevice* values,
    uint32_t capacity,
    uint32_t* count
) {
    *count = capacity;
    const VkResult result = device->fn.vkEnumeratePhysicalDevices(device->instance, count, values);
    return result == VK_INCOMPLETE ? GPUError_Unsupported
                                   : GPU_ErrorFromVk(device, result, "vkEnumeratePhysicalDevices", 0);
}

typedef struct GPUDeviceQueriedFeatures {
    VkPhysicalDeviceFeatures2 core;
    VkPhysicalDeviceVulkan11Features vulkan11;
    VkPhysicalDeviceVulkan12Features vulkan12;
    VkPhysicalDeviceVulkan13Features vulkan13;
    VkPhysicalDeviceVulkan14Features vulkan14;
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeap;
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR addressCommands;
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedPointers;
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedImageLayouts;
    VkPhysicalDeviceMeshShaderFeaturesEXT meshShader;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchainMaintenance1;
} GPUDeviceQueriedFeatures;

static void GPU_DeviceInitQueriedFeatures(
    GPUDeviceQueriedFeatures* features,
    bool presentation,
    bool includeUnifiedImageLayouts
) {
    memset(features, 0, sizeof(*features));
    features->core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features->vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features->vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features->vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features->vulkan14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    features->descriptorHeap.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
    features->addressCommands.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR;
    features->untypedPointers.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR;
    features->unifiedImageLayouts.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR;
    features->meshShader.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    features->swapchainMaintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
    features->core.pNext = &features->vulkan11;
    features->vulkan11.pNext = &features->vulkan12;
    features->vulkan12.pNext = &features->vulkan13;
    features->vulkan13.pNext = &features->vulkan14;
    features->vulkan14.pNext = &features->descriptorHeap;
    features->descriptorHeap.pNext = &features->addressCommands;
    features->addressCommands.pNext = &features->untypedPointers;
    features->untypedPointers.pNext =
        includeUnifiedImageLayouts ? (void*)&features->unifiedImageLayouts : (void*)&features->meshShader;
    features->unifiedImageLayouts.pNext = &features->meshShader;
    features->meshShader.pNext = presentation ? &features->swapchainMaintenance1 : NULL;
}

typedef struct GPUDeviceCandidate {
    VkPhysicalDevice physicalDevice;
    uint32_t queueFamilies[GPU_QUEUE_TYPE_COUNT];
    uint32_t queueCounts[GPU_QUEUE_TYPE_COUNT];
    bool timestamps[GPU_QUEUE_TYPE_COUNT];
    GPUExtent3 copyTextureGranularity;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    bool unifiedImageLayouts;
    bool imageCubeArray;
    bool textureCompressionBc;
    bool textureCompressionAstc;
    bool textureCompressionEtc2;
    bool storageInputOutput16;
    bool khrSwapchainMaintenance1;
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProperties;
    VkPhysicalDeviceVulkan12Properties vulkan12Properties;
} GPUDeviceCandidate;

static GPUError GPU_DeviceInspectCandidate(
    const GPUDeviceState* device,
    VkPhysicalDevice physicalDevice,
    bool khrSurfaceMaintenance1,
    bool extSurfaceMaintenance1,
    const GPUDeviceDesc* desc,
    GPUDeviceCandidate* output
) {
    const VkSurfaceKHR surface = device->surface;
    VkExtensionProperties extensions[GPU_MAX_DEVICE_EXTENSIONS];
    uint32_t extensionCount = 0;
    const GPUError extensionError = GPU_DeviceEnumerateDeviceExtensions(
        device,
        physicalDevice,
        extensions,
        GPU_MAX_DEVICE_EXTENSIONS,
        &extensionCount
    );
    if (extensionError != GPUError_None) return extensionError;
    const bool unifiedImageLayoutsExtension =
        GPU_HasExtension(extensions, extensionCount, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
    static const char* const requiredExtensions[] = {
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
    };
    for (uint32_t index = 0; index < GPUArrayCount(requiredExtensions); ++index) {
        if (!GPU_HasExtension(extensions, extensionCount, requiredExtensions[index])) return GPUError_Unsupported;
    }
    const bool khrSwapchainMaintenance1 =
        khrSurfaceMaintenance1 &&
        GPU_HasExtension(extensions, extensionCount, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    const bool extSwapchainMaintenance1 =
        extSurfaceMaintenance1 &&
        GPU_HasExtension(extensions, extensionCount, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    if (surface && (!GPU_HasExtension(extensions, extensionCount, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
                    (!khrSwapchainMaintenance1 && !extSwapchainMaintenance1))) {
        return GPUError_Unsupported;
    }

    GPUDeviceCandidate* result = output;
    memset(result, 0, sizeof(*result));
    result->physicalDevice = physicalDevice;
    result->copyTextureGranularity = (GPUExtent3){.x = 1, .y = 1, .z = 1};
    result->heapProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
    result->vulkan12Properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    result->heapProperties.pNext = &result->vulkan12Properties;
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &result->heapProperties,
    };
    device->fn.vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
    result->properties = properties2.properties;
    result->heapProperties.pNext = NULL;
    result->vulkan12Properties.pNext = NULL;
    if (result->properties.apiVersion < VK_API_VERSION_1_4) return GPUError_Unsupported;

    device->fn.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &result->memoryProperties);
    bool cpuVisibleMemory = false;
    for (uint32_t index = 0; index < result->memoryProperties.memoryTypeCount; ++index) {
        if (GPU_IsUsableMemoryType(&result->memoryProperties, index) &&
            (result->memoryProperties.memoryTypes[index].propertyFlags & GPU_CPU_VISIBLE_MEMORY_PROPERTIES) ==
                GPU_CPU_VISIBLE_MEMORY_PROPERTIES) {
            cpuVisibleMemory = true;
            break;
        }
    }
    if (!cpuVisibleMemory) return GPUError_Unsupported;

    GPUDeviceQueriedFeatures features;
    GPU_DeviceInitQueriedFeatures(&features, surface != VK_NULL_HANDLE, unifiedImageLayoutsExtension);
    device->fn.vkGetPhysicalDeviceFeatures2(physicalDevice, &features.core);
    const bool requiredFeatures =
        features.core.features.shaderInt16 == VK_TRUE && features.core.features.samplerAnisotropy == VK_TRUE &&
        features.core.features.depthBiasClamp == VK_TRUE && features.core.features.independentBlend == VK_TRUE &&
        features.core.features.fragmentStoresAndAtomics == VK_TRUE &&
        features.core.features.vertexPipelineStoresAndAtomics == VK_TRUE &&
        features.core.features.shaderStorageImageReadWithoutFormat == VK_TRUE &&
        features.core.features.shaderStorageImageWriteWithoutFormat == VK_TRUE &&
        features.core.features.multiDrawIndirect == VK_TRUE &&
        features.core.features.drawIndirectFirstInstance == VK_TRUE &&
        features.vulkan11.storageBuffer16BitAccess == VK_TRUE && features.vulkan11.storagePushConstant16 == VK_TRUE &&
        features.vulkan11.shaderDrawParameters == VK_TRUE && features.vulkan12.shaderFloat16 == VK_TRUE &&
        features.vulkan12.scalarBlockLayout == VK_TRUE && features.vulkan12.bufferDeviceAddress == VK_TRUE &&
        features.vulkan12.timelineSemaphore == VK_TRUE && features.vulkan13.synchronization2 == VK_TRUE &&
        features.vulkan13.dynamicRendering == VK_TRUE && features.vulkan13.maintenance4 == VK_TRUE &&
        features.vulkan14.maintenance5 == VK_TRUE && features.descriptorHeap.descriptorHeap == VK_TRUE &&
        features.addressCommands.deviceAddressCommands == VK_TRUE &&
        features.untypedPointers.shaderUntypedPointers == VK_TRUE && features.meshShader.meshShader == VK_TRUE &&
        (features.core.features.textureCompressionBC == VK_TRUE ||
         features.core.features.textureCompressionASTC_LDR == VK_TRUE) &&
        (!surface || features.swapchainMaintenance1.swapchainMaintenance1 == VK_TRUE);
    if (!requiredFeatures) return GPUError_Unsupported;
    result->unifiedImageLayouts =
        unifiedImageLayoutsExtension && features.unifiedImageLayouts.unifiedImageLayouts == VK_TRUE;
    result->imageCubeArray = features.core.features.imageCubeArray == VK_TRUE;
    result->textureCompressionBc = features.core.features.textureCompressionBC == VK_TRUE;
    result->textureCompressionAstc = features.core.features.textureCompressionASTC_LDR == VK_TRUE;
    result->textureCompressionEtc2 = features.core.features.textureCompressionETC2 == VK_TRUE;
    result->storageInputOutput16 = features.vulkan11.storageInputOutput16 == VK_TRUE;
    result->khrSwapchainMaintenance1 = khrSwapchainMaintenance1;

    uint32_t availableQueueCount = 0;
    device->fn.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &availableQueueCount, NULL);
    if (availableQueueCount > GPU_MAX_QUEUE_FAMILIES) return GPUError_Unsupported;
    VkQueueFamilyProperties queues[GPU_MAX_QUEUE_FAMILIES] = {0};
    uint32_t queueCount = availableQueueCount;
    device->fn.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queues);
    for (uint32_t index = 0; index < queueCount; ++index) {
        if (queues[index].queueCount == 0) continue;
        const VkQueueFlags flags = queues[index].queueFlags;
        uint32_t type = 0;
        if (flags & VK_QUEUE_GRAPHICS_BIT) {
            if (!(flags & VK_QUEUE_COMPUTE_BIT)) continue;
        } else if (flags & VK_QUEUE_COMPUTE_BIT) {
            type = 1;
        } else if (
            (flags & VK_QUEUE_TRANSFER_BIT) &&
            !(flags & ~(VkQueueFlags)(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_PROTECTED_BIT))
        ) {
            type = 2;
        } else {
            continue;
        }
        if (result->queueCounts[type] != 0) continue;
        VkBool32 presentationSupported = VK_TRUE;
        if (surface && type == 0) {
            const GPUError presentationError = GPU_ErrorFromVk(
                device,
                device->fn.vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, index, surface, &presentationSupported),
                "vkGetPhysicalDeviceSurfaceSupportKHR",
                0
            );
            if (presentationError != GPUError_None) return presentationError;
        }
        if (presentationSupported == VK_TRUE) {
            result->queueFamilies[type] = index;
            result->queueCounts[type] = queues[index].queueCount;
            result->timestamps[type] = desc->timestampQueryCount != 0 && queues[index].timestampValidBits == 64 &&
                                       features.vulkan12.hostQueryReset;
            if (type == 2) {
                result->copyTextureGranularity = (GPUExtent3){
                    .x = queues[index].minImageTransferGranularity.width,
                    .y = queues[index].minImageTransferGranularity.height,
                    .z = queues[index].minImageTransferGranularity.depth,
                };
            }
        }
    }
    if (result->queueCounts[0] == 0 || (desc->desiredComputeQueueCount && result->queueCounts[1] == 0) ||
        (desc->desiredCopyQueueCount && result->queueCounts[2] == 0))
        return GPUError_Unsupported;

    return GPUError_None;
}

#define GPU_DEVICE_NO_MESSAGE ((GPUMessageKind) - 1)

static bool GPU_DeviceFailCreation(GPUDeviceState* device, GPUError error, GPUMessageKind kind, GPUError* outError) {
    if (kind == GPU_DEVICE_NO_MESSAGE && error == GPUError_Unsupported) kind = GPUMessageKind_DeviceUnsupported;
    if (kind != GPU_DEVICE_NO_MESSAGE) GPU_ReportKind(device, kind);
    GPUDeviceDestroy((GPUDevice*)device);
    if (outError) *outError = error;
    return false;
}

bool GPUDeviceInit(GPUDevice* deviceHandle, const GPUDeviceDesc* userDesc, GPUError* outError) {
    GPUAssert(deviceHandle && userDesc, "GPUDeviceInit requires device storage and a description");
    if (outError) *outError = GPUError_None;
    GPUDeviceState* state = (GPUDeviceState*)deviceHandle;
    if (state->initialized) {
        state->errorCallback = userDesc->errorCallback;
        state->errorUser = userDesc->errorUser;
        state->allocUser = userDesc->allocUser;
        return false;
    }
    const GPUDeviceDesc resolved = GPU_ResolveDeviceDesc(userDesc);
    const GPUDeviceDesc* desc = &resolved;
#if defined(_WIN32)
    const bool presentation = desc->win32.hwnd != NULL;
#else
    const bool presentation = false;
#endif
    GPUAssert(
        desc->desiredSwapchainImageCount <= GPU_MAX_SWAPCHAIN_IMAGES,
        "swapchain image count must fit the wrapper's presentation context array"
    );
    const GPUDeviceLimits* limits = &desc->limits;
    memset(state, 0, sizeof(*state));
    state->errorCallback = desc->errorCallback;
    state->errorUser = desc->errorUser;
    state->allocUser = desc->allocUser;

    GPUError error = GPU_LoaderOpen(state);
    if (error != GPUError_None) {
        GPUDeviceDestroy(deviceHandle);
        if (outError) *outError = error;
        return false;
    }
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    error =
        GPU_ErrorFromVk(state, state->fn.vkEnumerateInstanceVersion(&loaderVersion), "vkEnumerateInstanceVersion", 0);
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    if (loaderVersion < VK_API_VERSION_1_4) {
        GPU_ReportKind(state, GPUMessageKind_LoaderTooOld);
        GPUDeviceDestroy(deviceHandle);
        if (outError) *outError = GPUError_Unsupported;
        return false;
    }

    GPUDeviceLayout layout;
    GPU_ComputeDeviceLayout(desc, &layout);
    uint8_t* block = GPU_ALLOC(state->allocUser, layout.total, GPU_ALLOCATION_ALIGNMENT);
    GPUAssert(((uintptr_t)block % GPU_ALLOCATION_ALIGNMENT) == 0, "GPU_ALLOC must honor the requested alignment");
    memset(block, 0, layout.total);
    state->memory = block;
    state->queues = (GPUQueue*)(block + layout.queues);
    for (uint32_t index = 0; index < layout.queueCapacity; ++index) {
        state->queues[index].commandSubmitInfos = (VkCommandBufferSubmitInfo*)(block + layout.commandSubmitInfos) +
                                                  (uint64_t)index * limits->maxSubmitCommandBuffers;
        state->queues[index].waitSubmitInfos =
            (VkSemaphoreSubmitInfo*)(block + layout.waitSubmitInfos) + (uint64_t)index * (limits->maxSubmitWaits + 1);
    }
    state->swapchainDeleteQueue.entries = (GPUDeferredSwapchainImage*)(block + layout.retiredImages);
    state->swapchainDeleteQueue.capacity = limits->maxRetiredSwapchainImages;
    state->limits = *limits;
    GPU_PoolInit(
        &state->timelineSemaphorePool,
        block + layout.timelineSemaphores,
        GPU_PoolSlotSize(sizeof(GPUTimelineSemaphore)),
        limits->maxTimelineSemaphores
    );
    GPU_PoolInit(
        &state->gpuHeapPool,
        block + layout.gpuHeaps,
        GPU_PoolSlotSize(sizeof(GPUGpuHeapOwner)),
        limits->maxGpuHeaps
    );
    GPU_PoolInit(
        &state->textureHeapPool,
        block + layout.textureHeaps,
        GPU_PoolSlotSize(sizeof(GPUTextureHeapOwner)),
        limits->maxTextureHeaps
    );
    GPU_PoolInit(
        &state->texturePool,
        block + layout.textures,
        GPU_PoolSlotSize(sizeof(GPUTexture)),
        limits->maxTextures
    );
    GPU_PoolInit(
        &state->renderViewPool,
        block + layout.renderViews,
        GPU_PoolSlotSize(sizeof(GPURenderView)),
        limits->maxRenderViews
    );
    GPU_PoolInit(
        &state->textureDescriptorHeapPool,
        block + layout.textureDescriptorHeaps,
        GPU_PoolSlotSize(sizeof(GPUTextureDescriptorHeap)),
        limits->maxTextureDescriptorHeaps
    );
    GPU_PoolInit(
        &state->samplerDescriptorHeapPool,
        block + layout.samplerDescriptorHeaps,
        GPU_PoolSlotSize(sizeof(GPUSamplerDescriptorHeap)),
        limits->maxSamplerDescriptorHeaps
    );
    GPU_PoolInit(&state->psoPool, block + layout.psos, GPU_PoolSlotSize(sizeof(GPUPSO)), limits->maxPsos);
    GPU_PoolInit(
        &state->commandPoolPool,
        block + layout.commandPools,
        layout.commandPoolSlotSize,
        limits->maxCommandPools
    );
    state->heapProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
    state->textureHeapAlignment = 16;
    state->textureMemoryType = VK_MAX_MEMORY_TYPES;
    state->timestampQueryCount = desc->timestampQueryCount;
    state->presentContextCount = presentation ? desc->desiredSwapchainImageCount : 0;

    VkExtensionProperties instanceExtensions[GPU_MAX_INSTANCE_EXTENSIONS];
    uint32_t instanceExtensionCount = 0;
    error = GPU_DeviceEnumerateInstanceExtensions(
        state,
        instanceExtensions,
        GPU_MAX_INSTANCE_EXTENSIONS,
        &instanceExtensionCount
    );
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
#if defined(_WIN32)
    state->swapchainColorSpaceEnabled =
        presentation &&
        GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    if (presentation && desc->swapchainColorSpace != GPUColorSpace_Srgb && !state->swapchainColorSpaceEnabled)
        return GPU_DeviceFailCreation(state, GPUError_Unsupported, GPU_DEVICE_NO_MESSAGE, outError);
    const bool khrSurfaceMaintenance1 =
        presentation &&
        GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    const bool extSurfaceMaintenance1 =
        presentation &&
        GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    if (presentation &&
        (!GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_KHR_SURFACE_EXTENSION_NAME) ||
         !GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) ||
         !GPU_HasExtension(
             instanceExtensions,
             instanceExtensionCount,
             VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME
         ) ||
         (!khrSurfaceMaintenance1 && !extSurfaceMaintenance1))) {
        return GPU_DeviceFailCreation(state, GPUError_Unsupported, GPU_DEVICE_NO_MESSAGE, outError);
    }
#else
    const bool khrSurfaceMaintenance1 = false;
    const bool extSurfaceMaintenance1 = false;
#endif
#if defined(GPU_DEBUG)
    VkLayerProperties layers[GPU_MAX_INSTANCE_LAYERS];
    uint32_t layerCount = 0;
    error = GPU_DeviceEnumerateInstanceLayers(state, layers, GPU_MAX_INSTANCE_LAYERS, &layerCount);
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    const bool debugUtilsAvailable =
        GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool validationAvailable = GPU_HasLayer(layers, layerCount, "VK_LAYER_KHRONOS_validation");
#endif

    const char* enabledInstanceExtensions[7] = {0};
    uint32_t enabledInstanceExtensionCount = 0;
    const char* enabledLayers[1] = {0};
    uint32_t enabledLayerCount = 0;
#if defined(GPU_DEBUG)
    if (debugUtilsAvailable)
        enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (validationAvailable) enabledLayers[enabledLayerCount++] = "VK_LAYER_KHRONOS_validation";
#endif
#if defined(_WIN32)
    if (presentation) {
        if (state->swapchainColorSpaceEnabled)
            enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
        enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_KHR_SURFACE_EXTENSION_NAME;
        enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
        enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        if (khrSurfaceMaintenance1)
            enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
        if (extSurfaceMaintenance1)
            enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    }
#endif

    const VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "NoGraphicsAPI application",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "NoGraphicsAPI",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };
    const VkInstanceCreateInfo instanceInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = enabledLayerCount,
        .ppEnabledLayerNames = enabledLayerCount ? enabledLayers : NULL,
        .enabledExtensionCount = enabledInstanceExtensionCount,
        .ppEnabledExtensionNames = enabledInstanceExtensionCount ? enabledInstanceExtensions : NULL,
    };
    error = GPU_ErrorFromVk(
        state,
        state->fn.vkCreateInstance(&instanceInfo, NULL, &state->instance),
        "vkCreateInstance",
        0
    );
    if (error == GPUError_None) error = GPU_LoaderLoadInstanceFunctions(state);
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);

#if defined(GPU_DEBUG)
    if (debugUtilsAvailable) {
        if (!state->fn.vkCreateDebugUtilsMessengerEXT || !state->fn.vkDestroyDebugUtilsMessengerEXT)
            return GPU_DeviceFailCreation(state, GPUError_DriverError, GPU_DEVICE_NO_MESSAGE, outError);
        const VkDebugUtilsMessengerCreateInfoEXT debugInfo = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = GPU_CommonDebugCallback,
        };
        error = GPU_ErrorFromVk(
            state,
            state->fn.vkCreateDebugUtilsMessengerEXT(state->instance, &debugInfo, NULL, &state->debugMessenger),
            "vkCreateDebugUtilsMessengerEXT",
            0
        );
        if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    }
#endif

#if defined(_WIN32)
    if (presentation) {
        const VkWin32SurfaceCreateInfoKHR surfaceInfo = {
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = desc->win32.hinstance,
            .hwnd = desc->win32.hwnd,
        };
        error = GPU_ErrorFromVk(
            state,
            state->fn.vkCreateWin32SurfaceKHR(state->instance, &surfaceInfo, NULL, &state->surface),
            "vkCreateWin32SurfaceKHR",
            0
        );
        if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    }
#endif

    VkPhysicalDevice physicalDevices[GPU_MAX_PHYSICAL_DEVICES] = {0};
    uint32_t physicalDeviceCount = 0;
    error = GPU_DeviceEnumeratePhysicalDevices(state, physicalDevices, GPU_MAX_PHYSICAL_DEVICES, &physicalDeviceCount);
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);

    GPUDeviceCandidate selected = {0};
    bool hasSelected = false;
    for (uint32_t index = 0; index < physicalDeviceCount; ++index) {
        GPUDeviceCandidate candidate;
        error = GPU_DeviceInspectCandidate(
            state,
            physicalDevices[index],
            khrSurfaceMaintenance1,
            extSurfaceMaintenance1,
            desc,
            &candidate
        );
        if (error == GPUError_Unsupported) continue;
        if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
        if (!hasSelected || candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            selected = candidate;
            hasSelected = true;
        }
        if (candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
    }
    if (!hasSelected)
        return GPU_DeviceFailCreation(state, GPUError_Unsupported, GPUMessageKind_NoCompatibleGpu, outError);

    state->physicalDevice = selected.physicalDevice;
    state->physicalProperties = selected.properties;
    state->heapProperties = selected.heapProperties;
    state->maxTimelineValueDifference = selected.vulkan12Properties.maxTimelineSemaphoreValueDifference;
    state->heapProperties.pNext = NULL;
    state->memoryProperties = selected.memoryProperties;
    for (uint32_t value = GPUFormat_Undefined + 1; value < GPU_FORMAT_COUNT; ++value) {
        state->formatFeatures[value] = GPU_OptimalFormatFeatures(state, (GPUFormat)value);
    }

    GPUDeviceQueriedFeatures enabledFeatures;
    GPU_DeviceInitQueriedFeatures(&enabledFeatures, presentation, selected.unifiedImageLayouts);
    state->textureCompressionEtc2 = selected.textureCompressionEtc2;
    enabledFeatures.core.features.imageCubeArray = selected.imageCubeArray;
    enabledFeatures.core.features.samplerAnisotropy = VK_TRUE;
    enabledFeatures.core.features.shaderInt16 = VK_TRUE;
    enabledFeatures.core.features.depthBiasClamp = VK_TRUE;
    enabledFeatures.core.features.independentBlend = VK_TRUE;
    enabledFeatures.core.features.textureCompressionBC = selected.textureCompressionBc;
    enabledFeatures.core.features.textureCompressionASTC_LDR = selected.textureCompressionAstc;
    enabledFeatures.core.features.textureCompressionETC2 = selected.textureCompressionEtc2;
    enabledFeatures.core.features.fragmentStoresAndAtomics = VK_TRUE;
    enabledFeatures.core.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    enabledFeatures.core.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    enabledFeatures.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabledFeatures.core.features.multiDrawIndirect = VK_TRUE;
    enabledFeatures.core.features.drawIndirectFirstInstance = VK_TRUE;
    enabledFeatures.vulkan11.storageBuffer16BitAccess = VK_TRUE;
    enabledFeatures.vulkan11.storagePushConstant16 = VK_TRUE;
    enabledFeatures.vulkan11.storageInputOutput16 = selected.storageInputOutput16;
    enabledFeatures.vulkan11.shaderDrawParameters = VK_TRUE;
    enabledFeatures.vulkan12.shaderFloat16 = VK_TRUE;
    enabledFeatures.vulkan12.scalarBlockLayout = VK_TRUE;
    enabledFeatures.vulkan12.timelineSemaphore = VK_TRUE;
    enabledFeatures.vulkan12.hostQueryReset =
        selected.timestamps[0] || selected.timestamps[1] || selected.timestamps[2];
    enabledFeatures.vulkan12.bufferDeviceAddress = VK_TRUE;
    enabledFeatures.vulkan13.synchronization2 = VK_TRUE;
    enabledFeatures.vulkan13.dynamicRendering = VK_TRUE;
    enabledFeatures.vulkan13.maintenance4 = VK_TRUE;
    enabledFeatures.vulkan14.maintenance5 = VK_TRUE;
    enabledFeatures.descriptorHeap.descriptorHeap = VK_TRUE;
    enabledFeatures.addressCommands.deviceAddressCommands = VK_TRUE;
    enabledFeatures.untypedPointers.shaderUntypedPointers = VK_TRUE;
    enabledFeatures.unifiedImageLayouts.unifiedImageLayouts = selected.unifiedImageLayouts ? VK_TRUE : VK_FALSE;
    enabledFeatures.meshShader.taskShader = VK_TRUE;
    enabledFeatures.meshShader.meshShader = VK_TRUE;
    enabledFeatures.swapchainMaintenance1.swapchainMaintenance1 = VK_TRUE;

    const uint32_t requestedCounts[GPU_QUEUE_TYPE_COUNT] =
        {desc->desiredQueueCount, desc->desiredComputeQueueCount, desc->desiredCopyQueueCount};
    uint32_t queueCounts[GPU_QUEUE_TYPE_COUNT] = {0};
    for (uint32_t type = 0; type < GPU_QUEUE_TYPE_COUNT; ++type) {
        queueCounts[type] =
            requestedCounts[type] < selected.queueCounts[type] ? requestedCounts[type] : selected.queueCounts[type];
        state->queueCount += queueCounts[type];
    }
    // Every priority is 1.0. The command submit info region is unused until the first submission, so it serves as
    // scratch and is re-zeroed below.
    float* queuePriorities = (float*)(block + layout.commandSubmitInfos);
    for (uint32_t index = 0; index < state->queueCount; ++index) queuePriorities[index] = 1.0f;
    VkDeviceQueueCreateInfo queueInfos[GPU_QUEUE_TYPE_COUNT] = {0};
    uint32_t firstQueue = 0;
    for (uint32_t type = 0; type < GPU_QUEUE_TYPE_COUNT; ++type) {
        if (queueCounts[type] == 0) continue;
        state->queueFamilies[state->queueFamilyCount] = selected.queueFamilies[type];
        queueInfos[state->queueFamilyCount++] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = selected.queueFamilies[type],
            .queueCount = queueCounts[type],
            .pQueuePriorities = queuePriorities + firstQueue,
        };
        firstQueue += queueCounts[type];
    }
    const char* enabledDeviceExtensions[7] = {0};
    uint32_t enabledDeviceExtensionCount = 0;
    enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME;
    enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME;
    enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME;
    if (selected.unifiedImageLayouts) {
        enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME;
    }
    enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
#if defined(_WIN32)
    if (presentation) {
        enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        enabledDeviceExtensions[enabledDeviceExtensionCount++] = selected.khrSwapchainMaintenance1
                                                                     ? VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
                                                                     : VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
    }
#endif
    const VkDeviceCreateInfo deviceInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabledFeatures.core,
        .queueCreateInfoCount = state->queueFamilyCount,
        .pQueueCreateInfos = queueInfos,
        .enabledExtensionCount = enabledDeviceExtensionCount,
        .ppEnabledExtensionNames = enabledDeviceExtensions,
    };
    error = GPU_ErrorFromVk(
        state,
        state->fn.vkCreateDevice(state->physicalDevice, &deviceInfo, NULL, &state->device),
        "vkCreateDevice",
        0
    );
    if (error == GPUError_None) error = GPU_LoaderLoadDeviceFunctions(state);
    memset(
        block + layout.commandSubmitInfos,
        0,
        (uint64_t)layout.queueCapacity * limits->maxSubmitCommandBuffers * sizeof(VkCommandBufferSubmitInfo)
    );
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    firstQueue = 0;
    for (uint32_t type = 0; type < GPU_QUEUE_TYPE_COUNT; ++type) {
        for (uint32_t index = 0; index < queueCounts[type]; ++index) {
            GPUQueue* queue = &state->queues[firstQueue++];
            queue->familyIndex = selected.queueFamilies[type];
            queue->timestamps = selected.timestamps[type];
            state->fn.vkGetDeviceQueue(state->device, queue->familyIndex, index, &queue->queue);
        }
    }
    if (!GPU_SupportsGpuHeapMemory(state) || !GPU_SelectTextureMemoryType(state))
        return GPU_DeviceFailCreation(state, GPUError_Unsupported, GPU_DEVICE_NO_MESSAGE, outError);

    state->caps = (GPUDeviceCaps){
        .deviceName = state->physicalProperties.deviceName,
        .queueCount = state->queueCount,
        .generalQueueCount = queueCounts[0],
        .computeQueueCount = queueCounts[1],
        .copyQueueCount = queueCounts[2],
        .copyTextureGranularity = selected.copyTextureGranularity,
        .maxPushDataSize = state->heapProperties.maxPushDataSize,
        .textureHeapAlignment = state->textureHeapAlignment,
        .timestampPeriodNs = selected.properties.limits.timestampPeriod,
        .subTexelPrecisionBits = selected.properties.limits.subTexelPrecisionBits,
        .textureCompressionBc = selected.textureCompressionBc,
        .textureCompressionAstc = selected.textureCompressionAstc,
        .storageInputOutput16 = selected.storageInputOutput16,
        .indirectMeshDraw = true,
    };
    if (presentation) {
        error = GPU_CreateSwapchain(state, desc);
        if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    }
    state->initialized = true;
    return true;
}

void GPUDeviceDestroy(GPUDevice* deviceHandle) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    if (!device) return;
    GPUAssert(!device->acquiredSwapchain, "device destroyed while a swapchain image is acquired");
    if (device->device) {
        GPU_DrainContexts(device);
        GPU_DestroyOwnedSwapchain(device);
        GPU_SwapchainDeleteQueueCollect(
            device,
            &device->swapchainDeleteQueue,
            device->device,
            device->completedPresentationRetirement
        );
        GPUAssert(device->swapchainDeleteQueue.count == 0, "device->swapchainDeleteQueue.count == 0");
    }
    if (device->device && device->presentationRetirement)
        device->fn.vkDestroySemaphore(device->device, device->presentationRetirement, NULL);
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        GPU_DestroyPresentContext(device, &device->presentContexts[index]);
    }
    if (device->device) device->fn.vkDestroyDevice(device->device, NULL);
    if (device->instance && device->surface) device->fn.vkDestroySurfaceKHR(device->instance, device->surface, NULL);
    if (device->instance && device->debugMessenger && device->fn.vkDestroyDebugUtilsMessengerEXT)
        device->fn.vkDestroyDebugUtilsMessengerEXT(device->instance, device->debugMessenger, NULL);
    if (device->instance) device->fn.vkDestroyInstance(device->instance, NULL);
    GPU_LoaderClose(device);
    if (device->memory) GPU_FREE(device->allocUser, device->memory);
    memset(device, 0, sizeof(*device));
}

void GPUDeviceSetErrorCallback(GPUDevice* deviceHandle, GPUErrorCallback callback, void* user) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUDeviceSetErrorCallback called with a null device");
    device->errorCallback = callback;
    device->errorUser = user;
}

const GPUDeviceCaps* GPUDeviceGetCaps(const GPUDevice* deviceHandle) {
    const GPUDeviceState* device = (const GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUDeviceGetCaps called with a null device");
    return &device->caps;
}

bool GPUDeviceSupportsTextureFormat(const GPUDevice* deviceHandle, GPUFormat format, GPUTextureUsage usage) {
    const GPUDeviceState* device = (const GPUDeviceState*)deviceHandle;
    GPUAssert(
        device && format != GPUFormat_Undefined && (uint32_t)format < GPU_FORMAT_COUNT &&
            GPU_HasValidTextureUsageBits(usage),
        "device && format != GPUFormat_Undefined && (uint32_t)format < GPU_FORMAT_COUNT && "
        "GPU_HasValidTextureUsageBits(usage)"
    );

    const GPUTextureFormatInfo info = GPUFormatGetTextureInfo(format);
    if ((info.depth || info.stencil) && (usage & GPUTextureUsage_ColorAttachment)) {
        return false;
    }
    if (!info.depth && !info.stencil && (usage & GPUTextureUsage_DepthStencilAttachment)) {
        return false;
    }
    if (info.depth && info.stencil &&
        (usage & (GPUTextureUsage_TransferSource | GPUTextureUsage_TransferDestination))) {
        return false;
    }
    switch (GPU_TextureCompression(format)) {
    case GPUTextureCompression_None: break;
    case GPUTextureCompression_Etc2:
        if (!device->textureCompressionEtc2) return false;
        break;
    case GPUTextureCompression_Astc:
        if (!device->caps.textureCompressionAstc) return false;
        break;
    case GPUTextureCompression_Bc:
        if (!device->caps.textureCompressionBc) return false;
        break;
    }

    const VkFormatFeatureFlags2 available = device->formatFeatures[format];
    const VkFormatFeatureFlags2 required = GPU_RequiredFormatFeatures(usage);
    return (available & required) == required;
}

GPUTimelineSemaphore* GPUTimelineSemaphoreCreate(GPUDevice* deviceHandle, uint64_t initialValue) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUTimelineSemaphoreCreate called with a null device");

    const VkSemaphoreTypeCreateInfo timelineInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = initialValue,
    };
    const VkSemaphoreCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timelineInfo,
    };
    GPUTimelineSemaphore* result = (GPUTimelineSemaphore*)GPU_PoolAlloc(&device->timelineSemaphorePool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_TimelineSemaphores);
        return NULL;
    }
    result->state = device;
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateSemaphore(device->device, &createInfo, NULL, &result->semaphore),
            "vkCreateSemaphore (timeline)",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->timelineSemaphorePool, result);
        return NULL;
    }
    return result;
}

void GPUTimelineSemaphoreDestroy(GPUTimelineSemaphore* semaphore) {
    if (!semaphore) return;
    semaphore->state->fn.vkDestroySemaphore(semaphore->state->device, semaphore->semaphore, NULL);
    GPU_PoolFree(&semaphore->state->timelineSemaphorePool, semaphore);
}

uint64_t GPUTimelineSemaphoreGetCompletedValue(const GPUTimelineSemaphore* semaphore) {
    GPUAssert(
        semaphore && semaphore->state && semaphore->semaphore,
        "GPUTimelineSemaphoreGetCompletedValue received an invalid semaphore"
    );
    uint64_t value = 0;
    GPU_AssertVk(
        semaphore->state->fn.vkGetSemaphoreCounterValue(semaphore->state->device, semaphore->semaphore, &value)
    );
    return value;
}

void GPUTimelineSemaphoreWait(GPUTimelinePoint point) {
    GPUTimelineSemaphore* semaphore = point.semaphore;
    GPUAssert(
        semaphore && semaphore->state && semaphore->semaphore,
        "GPUTimelineSemaphoreWait requires a live timeline semaphore"
    );

    const VkSemaphore handle = semaphore->semaphore;
    const VkSemaphoreWaitInfo waitInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &point.value,
    };
    GPU_AssertVk(semaphore->state->fn.vkWaitSemaphores(semaphore->state->device, &waitInfo, ~(uint64_t)0));
}

void GPUDeviceWaitIdle(GPUDevice* deviceHandle) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUDeviceWaitIdle called with a null device");
    GPUAssert(!device->acquiredSwapchain, "GPUDeviceWaitIdle is not allowed while a swapchain image is acquired");
    GPU_AssertVk(device->fn.vkDeviceWaitIdle(device->device));
    GPU_DrainContexts(device);
    device->nextPresentContext = 0;
}

GPUGpuHeap GPUGpuHeapCreate(GPUDevice* deviceHandle, uint64_t byteCount, GPUMemoryType memory) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "device");
    VkMemoryPropertyFlags required = 0;
    VkMemoryPropertyFlags preferred = 0;
    VkMemoryPropertyFlags avoided = 0;
    switch (memory) {
    case GPUMemoryType_CpuVisible: required = GPU_CPU_VISIBLE_MEMORY_PROPERTIES; break;
    case GPUMemoryType_GpuOnly:
        required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        avoided = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        break;
    case GPUMemoryType_Readback:
        required = GPU_CPU_VISIBLE_MEMORY_PROPERTIES;
        preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    default: GPUAssert(false, "GPUGpuHeapCreate received an invalid memory type"); return (GPUGpuHeap){0};
    }

    GPUGpuHeapOwner* heap = (GPUGpuHeapOwner*)GPU_PoolAlloc(&device->gpuHeapPool);
    if (!heap) {
        GPU_ReportLimit(device, GPULimit_GpuHeaps);
        return (GPUGpuHeap){0};
    }
    heap->state = device;
    if (!GPU_CreateBackingBuffer(
            device,
            &heap->backing,
            byteCount,
            GPU_UNIVERSAL_BUFFER_USAGE,
            required,
            preferred,
            avoided
        )) {
        GPU_PoolFree(&device->gpuHeapPool, heap);
        return (GPUGpuHeap){0};
    }
    return (GPUGpuHeap){
        .range =
            {
                .cpu = (uint8_t*)heap->backing.mapped,
                .gpu = (uint8_t*)(uintptr_t)heap->backing.address,
                .size = byteCount,
            },
        .owner = heap,
    };
}

void GPUGpuHeapDestroy(const GPUGpuHeap* heap) {
    if (!heap->owner) return;
    GPUAssert(heap->owner->state, "heap->owner->state");
    GPUGpuHeapOwner* owner = (GPUGpuHeapOwner*)heap->owner;
    GPU_DestroyBackingBuffer(owner->state, &owner->backing);
    GPU_PoolFree(&owner->state->gpuHeapPool, owner);
}
