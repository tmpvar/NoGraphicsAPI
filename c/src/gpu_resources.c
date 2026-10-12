#include "gpu_api.h"
#include "gpu_internal.h"

typedef struct GPUResPreparedTexture {
    VkFormat viewFormats[GPU_FORMAT_COUNT];
    VkImageFormatListCreateInfo formatList;
    VkImageCreateInfo imageInfo;
} GPUResPreparedTexture;

static GPUTextureDesc GPU_ResolveTextureDesc(const GPUTextureDesc* desc) {
    GPUTextureDesc resolved = *desc;
    if (resolved.type == GPUTextureType_Undefined) resolved.type = GPUTextureType_2D;
    if (!resolved.extent.x) resolved.extent.x = 1;
    if (!resolved.extent.y) resolved.extent.y = 1;
    if (!resolved.extent.z) resolved.extent.z = 1;
    if (!resolved.mipLevels) resolved.mipLevels = 1;
    if (!resolved.layerCount) resolved.layerCount = 1;
    if (resolved.format == GPUFormat_Undefined) resolved.format = GPUFormat_Rgba8Unorm;
    if (!resolved.usage) resolved.usage = GPUTextureUsage_Sampled;
    return resolved;
}

static void GPU_ResPrepareTexture(GPUDeviceState* device, const GPUTextureDesc* desc, GPUResPreparedTexture* output) {
    memset(output, 0, sizeof(*output));
    output->formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    output->imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;

    VkImageUsageFlags usage = 0;
    if (desc->usage & GPUTextureUsage_Sampled) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc->usage & GPUTextureUsage_Storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (desc->usage & GPUTextureUsage_ColorAttachment) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc->usage & GPUTextureUsage_DepthStencilAttachment) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc->usage & GPUTextureUsage_TransferSource) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc->usage & GPUTextureUsage_TransferDestination) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    const VkImageType imageType = GPU_ToVkImageType(desc->type);
    const VkFormat format = GPU_ToVkFormat(desc->format);
    const VkFormatFeatureFlags2 sampledFeatures = GPU_RequiredFormatFeatures(GPUTextureUsage_Sampled);
    const VkFormatFeatureFlags2 storageFeatures = GPU_RequiredFormatFeatures(GPUTextureUsage_Storage);
    uint32_t viewFormatCount = 1;
    output->viewFormats[0] = format;
    for (uint32_t value = GPUFormat_Undefined + 1; desc->mutableFormat && value < GPU_FORMAT_COUNT; ++value) {
        const GPUFormat viewFormat = (GPUFormat)value;
        if (viewFormat == desc->format || !GPU_CompatibleViewFormats(desc->format, viewFormat)) continue;
        const VkFormatFeatureFlags2 viewFeatures = device->formatFeatures[value];
        const bool sampled =
            (desc->usage & GPUTextureUsage_Sampled) && (viewFeatures & sampledFeatures) == sampledFeatures;
        const bool storage =
            (desc->usage & GPUTextureUsage_Storage) && (viewFeatures & storageFeatures) == storageFeatures;
        if (!sampled && !storage) continue;
        GPUAssert(viewFormatCount < GPU_FORMAT_COUNT, "viewFormatCount < GPU_FORMAT_COUNT");
        output->viewFormats[viewFormatCount++] = GPU_ToVkFormat(viewFormat);
    }

    VkImageCreateFlags imageFlags = 0;
    if (desc->aliasable) imageFlags |= VK_IMAGE_CREATE_ALIAS_BIT;
    if (desc->type == GPUTextureType_Cube || desc->type == GPUTextureType_CubeArray)
        imageFlags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (viewFormatCount > 1) imageFlags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

    output->formatList.viewFormatCount = viewFormatCount;
    output->formatList.pViewFormats = output->viewFormats;
    output->imageInfo = (VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = viewFormatCount > 1 ? &output->formatList : NULL,
        .flags = imageFlags,
        .imageType = imageType,
        .format = format,
        .extent =
            {
                .width = desc->extent.x,
                .height = desc->extent.y,
                .depth = desc->extent.z,
            },
        .mipLevels = desc->mipLevels,
        .arrayLayers = desc->layerCount,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
}

GPUTextureHeap GPUTextureHeapCreate(GPUDevice* deviceHandle, uint64_t byteCount) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUTextureHeapCreate called with a null device");
    GPUTextureHeapOwner* owner = (GPUTextureHeapOwner*)GPU_PoolAlloc(&device->textureHeapPool);
    if (!owner) {
        GPU_ReportLimit(device, GPULimit_TextureHeaps);
        return (GPUTextureHeap){0};
    }
    owner->state = device;
    const VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = byteCount,
        .memoryTypeIndex = device->textureMemoryType,
    };
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkAllocateMemory(device->device, &allocateInfo, NULL, &owner->memory),
            "vkAllocateMemory (texture heap)",
            byteCount
        ) != GPUError_None) {
        GPU_PoolFree(&device->textureHeapPool, owner);
        return (GPUTextureHeap){0};
    }
    return (GPUTextureHeap){
        .size = byteCount,
        .owner = owner,
    };
}

void GPUTextureHeapDestroy(const GPUTextureHeap* heap) {
    if (!heap->owner) return;
    GPUDeviceState* device = heap->owner->state;
    device->fn.vkFreeMemory(device->device, heap->owner->memory, NULL);
    GPU_PoolFree(&device->textureHeapPool, (void*)heap->owner);
}

GPUSizeAlign GPUTextureGetSizeAlign(GPUDevice* deviceHandle, const GPUTextureDesc* userDesc) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && userDesc, "GPUTextureGetSizeAlign called with a null device");
    const GPUTextureDesc resolved = GPU_ResolveTextureDesc(userDesc);
    const GPUTextureDesc* desc = &resolved;
    GPUResPreparedTexture texture;
    GPU_ResPrepareTexture(device, desc, &texture);
    VkImageFormatProperties properties;
    if (!GPU_SupportsImageCreateInfo(device, &texture.imageInfo, &properties)) {
        GPU_ReportKind(device, GPUMessageKind_TextureUnsupported);
        return (GPUSizeAlign){0};
    }
    const VkMemoryRequirements requirements = GPU_ImageMemoryRequirements(device, &texture.imageInfo);
    return (GPUSizeAlign){
        .size = requirements.size,
        .align = requirements.alignment,
    };
}

GPUTexture* GPUTextureCreate(
    GPUCommandBuffer* commands,
    const GPUTextureDesc* userDesc,
    const GPUTextureHeap* heap,
    uint64_t offset
) {
    GPUAssert(
        commands && commands->state && userDesc && heap && heap->owner,
        "commands && commands->state && userDesc && heap && heap->owner"
    );
    const GPUTextureDesc resolved = GPU_ResolveTextureDesc(userDesc);
    const GPUTextureDesc* desc = &resolved;
    GPUDeviceState* device = commands->state;
    GPUResPreparedTexture texture;
    GPU_ResPrepareTexture(device, desc, &texture);

    GPUTexture* result = (GPUTexture*)GPU_PoolAlloc(&device->texturePool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_Textures);
        return NULL;
    }
    result->state = device;
    result->width = desc->extent.x;
    result->height = desc->extent.y;
    result->depth = desc->extent.z;
    result->layerCount = desc->layerCount;
    result->type = desc->type;
    result->format = desc->format;
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateImage(device->device, &texture.imageInfo, NULL, &result->image),
            "vkCreateImage",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->texturePool, result);
        return NULL;
    }
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkBindImageMemory(device->device, result->image, heap->owner->memory, offset),
            "vkBindImageMemory",
            0
        ) != GPUError_None) {
        device->fn.vkDestroyImage(device->device, result->image, NULL);
        GPU_PoolFree(&device->texturePool, result);
        return NULL;
    }
    if (desc->aliasable) return result;

    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = result->image,
        .subresourceRange = {
            .aspectMask = GPU_ImageAspects(desc->format),
            .levelCount = desc->mipLevels,
            .layerCount = desc->layerCount,
        },
    };
    GPU_RecordImageBarriers(commands->state, commands->commandBuffer, &barrier, 1);
    return result;
}

void GPUTextureDestroy(GPUTexture* texture) {
    if (!texture) return;
    texture->state->fn.vkDestroyImage(texture->state->device, texture->image, NULL);
    GPU_PoolFree(&texture->state->texturePool, texture);
}

void GPUCmdActivateTextureAlias(GPUCommandBuffer* commands, GPUTexture* texture) {
    GPUAssert(
        commands && texture && commands->state == texture->state,
        "commands && texture && commands->state == texture->state"
    );
    // The outgoing image can have a different format and size, so its writes need a global memory dependency.
    const VkMemoryBarrier2 memoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memoryBarrier,
    };
    commands->state->fn.vkCmdPipelineBarrier2(commands->commandBuffer, &dependency);
    const VkImageMemoryBarrier2 imageBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->image,
        .subresourceRange = {
            .aspectMask = GPU_ImageAspects(texture->format),
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    GPU_RecordImageBarriers(commands->state, commands->commandBuffer, &imageBarrier, 1);
}

GPURenderView* GPURenderViewCreate(GPUTexture* texture, const GPURenderViewDesc* desc) {
    GPUAssert(texture && texture->state && desc, "texture && texture->state && desc");
    GPUDeviceState* device = texture->state;

    uint32_t width = texture->width >> desc->mipLevel;
    uint32_t height = texture->height >> desc->mipLevel;
    if (width == 0) width = 1;
    if (height == 0) height = 1;
    GPURenderView* result = (GPURenderView*)GPU_PoolAlloc(&device->renderViewPool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_RenderViews);
        return NULL;
    }
    result->state = device;
    result->width = width;
    result->height = height;
    const VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = GPU_ToVkFormat(texture->format),
        .subresourceRange = {
            .aspectMask = GPU_ImageAspects(texture->format),
            .baseMipLevel = desc->mipLevel,
            .levelCount = 1,
            .baseArrayLayer = desc->slice,
            .layerCount = 1,
        },
    };
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateImageView(device->device, &viewInfo, NULL, &result->view),
            "vkCreateImageView",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->renderViewPool, result);
        return NULL;
    }
    return result;
}

void GPURenderViewDestroy(GPURenderView* renderView) {
    if (!renderView) return;
    GPUAssert(!renderView->swapchainView, "swapchain render views are owned by their swapchain");
    renderView->state->fn.vkDestroyImageView(renderView->state->device, renderView->view, NULL);
    GPU_PoolFree(&renderView->state->renderViewPool, renderView);
}

GPUTextureDescriptorHeap* GPUTextureDescriptorHeapCreate(GPUDevice* deviceHandle, uint32_t capacity) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && capacity, "device && capacity");
    GPUTextureDescriptorHeap* heap = (GPUTextureDescriptorHeap*)GPU_PoolAlloc(&device->textureDescriptorHeapPool);
    if (!heap) {
        GPU_ReportLimit(device, GPULimit_TextureDescriptorHeaps);
        return NULL;
    }
    heap->state = device;
    heap->storage = GPU_AllocateDescriptorHeap(
        device,
        (uint64_t)capacity * device->heapProperties.imageDescriptorSize,
        GPUDescriptorHeapType_Texture
    );
    heap->capacity = capacity;
    if (!heap->storage.owner) {
        GPU_PoolFree(&device->textureDescriptorHeapPool, heap);
        return NULL;
    }
    return heap;
}

void GPUTextureDescriptorHeapDestroy(GPUTextureDescriptorHeap* heap) {
    if (!heap) return;
    GPUGpuHeapDestroy(&heap->storage);
    GPU_PoolFree(&heap->state->textureDescriptorHeapPool, heap);
}

GPUSamplerDescriptorHeap* GPUSamplerDescriptorHeapCreate(GPUDevice* deviceHandle, uint32_t capacity) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && capacity, "device && capacity");
    GPUSamplerDescriptorHeap* heap = (GPUSamplerDescriptorHeap*)GPU_PoolAlloc(&device->samplerDescriptorHeapPool);
    if (!heap) {
        GPU_ReportLimit(device, GPULimit_SamplerDescriptorHeaps);
        return NULL;
    }
    heap->state = device;
    heap->storage = GPU_AllocateDescriptorHeap(
        device,
        (uint64_t)capacity * device->heapProperties.samplerDescriptorSize,
        GPUDescriptorHeapType_Sampler
    );
    heap->capacity = capacity;
    if (!heap->storage.owner) {
        GPU_PoolFree(&device->samplerDescriptorHeapPool, heap);
        return NULL;
    }
    return heap;
}

void GPUSamplerDescriptorHeapDestroy(GPUSamplerDescriptorHeap* heap) {
    if (!heap) return;
    GPUGpuHeapDestroy(&heap->storage);
    GPU_PoolFree(&heap->state->samplerDescriptorHeapPool, heap);
}

void GPUTextureDescriptorHeapCopy(
    const GPUTextureDescriptorHeap* source,
    uint32_t sourceIndex,
    GPUTextureDescriptorHeap* destination,
    uint32_t destinationIndex,
    uint32_t count
) {
    GPUAssert(
        source && destination && source->state == destination->state,
        "source && destination && source->state == destination->state"
    );
    GPUAssert(
        sourceIndex <= source->capacity && count <= source->capacity - sourceIndex,
        "sourceIndex <= source->capacity && count <= source->capacity - sourceIndex"
    );
    GPUAssert(
        destinationIndex <= destination->capacity && count <= destination->capacity - destinationIndex,
        "destinationIndex <= destination->capacity && count <= destination->capacity - destinationIndex"
    );
    const uint64_t stride = source->state->heapProperties.imageDescriptorSize;
    memmove(
        destination->storage.range.cpu + (uint64_t)destinationIndex * stride,
        source->storage.range.cpu + (uint64_t)sourceIndex * stride,
        (size_t)(count * stride)
    );
}

void GPUSamplerDescriptorHeapCopy(
    const GPUSamplerDescriptorHeap* source,
    uint32_t sourceIndex,
    GPUSamplerDescriptorHeap* destination,
    uint32_t destinationIndex,
    uint32_t count
) {
    GPUAssert(
        source && destination && source->state == destination->state,
        "source && destination && source->state == destination->state"
    );
    GPUAssert(
        sourceIndex <= source->capacity && count <= source->capacity - sourceIndex,
        "sourceIndex <= source->capacity && count <= source->capacity - sourceIndex"
    );
    GPUAssert(
        destinationIndex <= destination->capacity && count <= destination->capacity - destinationIndex,
        "destinationIndex <= destination->capacity && count <= destination->capacity - destinationIndex"
    );
    const uint64_t stride = source->state->heapProperties.samplerDescriptorSize;
    memmove(
        destination->storage.range.cpu + (uint64_t)destinationIndex * stride,
        source->storage.range.cpu + (uint64_t)sourceIndex * stride,
        (size_t)(count * stride)
    );
}

void GPUTextureDescriptorHeapWrite(
    GPUTextureDescriptorHeap* heap,
    uint32_t index,
    const GPUTexture* texture,
    GPUTextureDescriptorType type,
    const GPUTextureDescriptorDesc* desc
) {
    GPUAssert(
        heap && texture && desc && index < heap->capacity && heap->state == texture->state,
        "heap && texture && desc && index < heap->capacity && heap->state == texture->state"
    );
    GPUDeviceState* device = heap->state;

    const VkImageUsageFlags descriptorUsage =
        (VkImageUsageFlags)(type == GPUTextureDescriptorType_Sampled ? VK_IMAGE_USAGE_SAMPLED_BIT
                                                                     : VK_IMAGE_USAGE_STORAGE_BIT);

    VkImageAspectFlags descriptorAspect = 0;
    switch (desc->aspect) {
    case GPUTextureAspect_Automatic:
        descriptorAspect = GPU_HasDepthAspect(texture->format)     ? VK_IMAGE_ASPECT_DEPTH_BIT
                           : GPU_HasStencilAspect(texture->format) ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                                   : VK_IMAGE_ASPECT_COLOR_BIT;
        break;
    case GPUTextureAspect_Color: descriptorAspect = VK_IMAGE_ASPECT_COLOR_BIT; break;
    case GPUTextureAspect_Depth: descriptorAspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
    case GPUTextureAspect_Stencil: descriptorAspect = VK_IMAGE_ASPECT_STENCIL_BIT; break;
    }
    const VkImageViewUsageCreateInfo viewUsage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = descriptorUsage,
    };
    const VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &viewUsage,
        .image = texture->image,
        .viewType = GPU_ToVkImageViewType(texture->type),
        .format = GPU_ToVkFormat(desc->format == GPUFormat_Undefined ? texture->format : desc->format),
        .subresourceRange = {
            .aspectMask = descriptorAspect,
            .baseMipLevel = desc->baseMip,
            .levelCount = desc->mipCount == 0 ? VK_REMAINING_MIP_LEVELS : desc->mipCount,
            .baseArrayLayer = desc->baseLayer,
            .layerCount = desc->layerCount == 0 ? VK_REMAINING_ARRAY_LAYERS : desc->layerCount,
        },
    };
    const VkImageDescriptorInfoEXT imageDescriptor = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
        .pView = &viewInfo,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkResourceDescriptorInfoEXT descriptorInfo = {
        .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
        .type = type == GPUTextureDescriptorType_Sampled ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                         : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .data = {.pImage = &imageDescriptor},
    };
    const VkHostAddressRangeEXT destination = {
        .address = heap->storage.range.cpu + (uint64_t)index * device->heapProperties.imageDescriptorSize,
        .size = device->heapProperties.imageDescriptorSize,
    };
    GPU_AssertVk(device->fn.vkWriteResourceDescriptorsEXT(device->device, 1, &descriptorInfo, &destination));
}

void GPUSamplerDescriptorHeapWrite(GPUSamplerDescriptorHeap* heap, uint32_t index, const GPUSamplerDesc* desc) {
    GPUAssert(heap && desc && index < heap->capacity, "heap && desc && index < heap->capacity");
    GPUDeviceState* device = heap->state;
    const VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = GPU_ToVkFilter(desc->magFilter),
        .minFilter = GPU_ToVkFilter(desc->minFilter),
        .mipmapMode = GPU_ToVkMipmapMode(desc->mipFilter),
        .addressModeU = (VkSamplerAddressMode)desc->addressU,
        .addressModeV = (VkSamplerAddressMode)desc->addressV,
        .addressModeW = (VkSamplerAddressMode)desc->addressW,
        .anisotropyEnable = desc->anisotropic ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = desc->anisotropic ? 4.0f : 1.0f,
        .compareEnable = desc->compareEnabled ? VK_TRUE : VK_FALSE,
        .compareOp =
            GPU_ToVkCompareOp(desc->compare == GPUCompareOp_Undefined ? GPUCompareOp_LessEqual : desc->compare),
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    const VkHostAddressRangeEXT destination = {
        .address = heap->storage.range.cpu + (uint64_t)index * device->heapProperties.samplerDescriptorSize,
        .size = device->heapProperties.samplerDescriptorSize,
    };
    GPU_AssertVk(device->fn.vkWriteSamplerDescriptorsEXT(device->device, 1, &samplerInfo, &destination));
}
