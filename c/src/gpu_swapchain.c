#include "gpu_api.h"
#include "gpu_internal.h"

// Swapchain delete queue. Fixed-capacity ring; capacity is GPUDeviceLimits::maxRetiredSwapchainImages.

static void GPU_SwapchainDeleteQueuePush(
    GPUSwapchainDeleteQueue* queue,
    uint64_t retireValue,
    VkSwapchainKHR swapchain,
    VkImageView view
) {
    GPUAssert(swapchain && view, "swapchain && view");
    GPUAssert(
        queue->count < queue->capacity,
        "retired swapchain images exceed GPUDeviceLimits::maxRetiredSwapchainImages"
    );
    if (queue->count != 0) {
        GPUAssert(
            queue->entries[(queue->first + queue->count - 1) % queue->capacity].retireValue <= retireValue,
            "swapchain deletion retire values must be monotonic"
        );
    }
    queue->entries[(queue->first + queue->count) % queue->capacity] = (GPUDeferredSwapchainImage){
        .retireValue = retireValue,
        .swapchain = swapchain,
        .view = view,
    };
    ++queue->count;
}

static void GPU_SwapchainDeleteQueueCollect(
    GPUDeviceState* state,
    GPUSwapchainDeleteQueue* queue,
    VkDevice device,
    uint64_t completedValue
) {
    while (queue->count != 0 && queue->entries[queue->first].retireValue <= completedValue) {
        const GPUDeferredSwapchainImage entry = queue->entries[queue->first];
        const bool finalImage =
            queue->count == 1 || queue->entries[(queue->first + 1) % queue->capacity].swapchain != entry.swapchain;
        state->fn.vkDestroyImageView(device, entry.view, NULL);
        if (finalImage) state->fn.vkDestroySwapchainKHR(device, entry.swapchain, NULL);
        memset(&queue->entries[queue->first], 0, sizeof(GPUDeferredSwapchainImage));
        queue->first = (queue->first + 1) % queue->capacity;
        --queue->count;
    }
    if (queue->count == 0) queue->first = 0;
}

static void GPU_PollPresentationRetirement(GPUDeviceState* device) {
    if (!device->presentationRetirement ||
        device->completedPresentationRetirement == device->presentationRetirementValue)
        return;
    uint64_t completed = 0;
    GPU_AssertVk(device->fn.vkGetSemaphoreCounterValue(device->device, device->presentationRetirement, &completed));
    GPUAssert(
        completed >= device->completedPresentationRetirement && completed <= device->presentationRetirementValue,
        "completed >= device->completedPresentationRetirement && completed <= device->presentationRetirementValue"
    );
    if (completed == device->completedPresentationRetirement) return;
    device->completedPresentationRetirement = completed;
    GPU_SwapchainDeleteQueueCollect(
        device,
        &device->swapchainDeleteQueue,
        device->device,
        device->completedPresentationRetirement
    );
}

static void GPU_WaitPresentationRetirement(GPUDeviceState* device, uint64_t value) {
    GPUAssert(value <= device->presentationRetirementValue, "value <= device->presentationRetirementValue");
    if (value <= device->completedPresentationRetirement) {
        GPU_SwapchainDeleteQueueCollect(
            device,
            &device->swapchainDeleteQueue,
            device->device,
            device->completedPresentationRetirement
        );
        return;
    }
    const VkSemaphoreWaitInfo waitInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &device->presentationRetirement,
        .pValues = &value,
    };
    GPU_AssertVk(device->fn.vkWaitSemaphores(device->device, &waitInfo, ~(uint64_t)0));
    device->completedPresentationRetirement = value;
    GPU_SwapchainDeleteQueueCollect(
        device,
        &device->swapchainDeleteQueue,
        device->device,
        device->completedPresentationRetirement
    );
}

static uint64_t GPU_NextPresentationRetirement(GPUDeviceState* device) {
    const uint64_t next = device->presentationRetirementValue + 1;
    if (next - device->completedPresentationRetirement > device->maxTimelineValueDifference) {
        GPU_PollPresentationRetirement(device);
        if (next - device->completedPresentationRetirement > device->maxTimelineValueDifference)
            GPU_WaitPresentationRetirement(device, next - device->maxTimelineValueDifference);
        GPUAssert(
            next - device->completedPresentationRetirement <= device->maxTimelineValueDifference,
            "next - device->completedPresentationRetirement <= device->maxTimelineValueDifference"
        );
    }
    device->presentationRetirementValue = next;
    return next;
}

static GPUError GPU_CreatePresentContext(GPUDeviceState* device, GPUPresentContext* context) {
    GPUAssert(
        !context->acquired && !context->rendered && !context->presented && !context->swapchain &&
            !context->presentPending,
        "!context->acquired && !context->rendered && !context->presented && !context->swapchain && "
        "!context->presentPending"
    );
    const VkSemaphoreCreateInfo semaphoreInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    GPUError error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateSemaphore(device->device, &semaphoreInfo, NULL, &context->acquired),
        "vkCreateSemaphore (acquire)",
        0
    );
    if (error != GPUError_None) return error;
    error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateSemaphore(device->device, &semaphoreInfo, NULL, &context->rendered),
        "vkCreateSemaphore (render)",
        0
    );
    if (error != GPUError_None) {
        GPU_DestroyPresentContext(device, context);
        return error;
    }
    const VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateFence(device->device, &fenceInfo, NULL, &context->presented),
        "vkCreateFence",
        0
    );
    if (error != GPUError_None) GPU_DestroyPresentContext(device, context);
    return error;
}

static void GPU_DestroyPresentContext(GPUDeviceState* device, GPUPresentContext* context) {
    GPUAssert(!context->presentPending, "!context->presentPending");
    if (device->device && context->acquired) device->fn.vkDestroySemaphore(device->device, context->acquired, NULL);
    if (device->device && context->rendered) device->fn.vkDestroySemaphore(device->device, context->rendered, NULL);
    if (device->device && context->presented) device->fn.vkDestroyFence(device->device, context->presented, NULL);
    memset(context, 0, sizeof(GPUPresentContext));
}

static void GPU_FinishPresentContext(GPUDeviceState* device, GPUPresentContext* context) {
    GPUAssert(!context->presentPending && context->swapchain, "!context->presentPending && context->swapchain");
    const VkSwapchainKHR completedSwapchain = context->swapchain;
    context->swapchain = VK_NULL_HANDLE;
    for (uint32_t index = 0; index < GPU_MAX_SWAPCHAIN_IMAGES; ++index) {
        GPURetiredSwapchain* retired = &device->retiredSwapchains[index];
        if (retired->handle != completedSwapchain) continue;
        for (uint32_t pending = 0; pending < GPU_MAX_SWAPCHAIN_IMAGES; ++pending) {
            if (device->presentContexts[pending].presentPending &&
                device->presentContexts[pending].swapchain == completedSwapchain)
                return;
        }
        GPU_QueueRetiredSwapchain(device, retired);
        return;
    }
}

static void GPU_WaitPresentContext(GPUDeviceState* device, GPUPresentContext* context) {
    if (!context->presentPending) return;
    GPU_AssertVk(device->fn.vkWaitForFences(device->device, 1, &context->presented, VK_TRUE, ~(uint64_t)0));
    context->presentPending = false;
    GPU_FinishPresentContext(device, context);
}

static void GPU_PollPresentContexts(GPUDeviceState* device) {
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        GPUPresentContext* context = &device->presentContexts[index];
        if (!context->presentPending) continue;
        const VkResult result = device->fn.vkGetFenceStatus(device->device, context->presented);
        if (result == VK_NOT_READY) continue;
        GPU_AssertVk(result);
        context->presentPending = false;
        GPU_FinishPresentContext(device, context);
    }
}

static void GPU_QueueRetiredSwapchain(GPUDeviceState* device, GPURetiredSwapchain* retired) {
    GPUAssert(retired->handle && retired->viewCount != 0, "retired->handle && retired->viewCount != 0");
    for (uint32_t index = 0; index < retired->viewCount; ++index) {
        GPUAssert(retired->views[index], "retired->views[index]");
        GPU_SwapchainDeleteQueuePush(
            &device->swapchainDeleteQueue,
            device->presentationRetirementValue,
            retired->handle,
            retired->views[index]
        );
    }
    memset(retired, 0, sizeof(GPURetiredSwapchain));
    GPU_SwapchainDeleteQueueCollect(
        device,
        &device->swapchainDeleteQueue,
        device->device,
        device->completedPresentationRetirement
    );
}

static void GPU_DrainContexts(GPUDeviceState* device) {
    GPU_WaitPresentationRetirement(device, device->presentationRetirementValue);
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        GPU_WaitPresentContext(device, &device->presentContexts[index]);
    }
    for (uint32_t index = 0; index < GPU_MAX_SWAPCHAIN_IMAGES; ++index) {
        GPUAssert(!device->retiredSwapchains[index].handle, "!device->retiredSwapchains[index].handle");
    }
    GPU_SwapchainDeleteQueueCollect(
        device,
        &device->swapchainDeleteQueue,
        device->device,
        device->completedPresentationRetirement
    );
}

static void GPU_SwapRetireHandle(GPUSwapchain* swapchain) {
    GPUDeviceState* device = swapchain->state;
    if (!swapchain->handle) {
        GPUAssert(swapchain->imageCount == 0, "swapchain->imageCount == 0");
        swapchain->width = 0;
        swapchain->height = 0;
        return;
    }

    GPU_PollPresentContexts(device);
    GPURetiredSwapchain retired = {
        .handle = swapchain->handle,
    };
    for (uint32_t index = 0; index < swapchain->imageCount; ++index) {
        retired.views[retired.viewCount++] = swapchain->renderViews[index].view;
        memset(&swapchain->renderViews[index], 0, sizeof(GPURenderView));
        swapchain->images[index] = VK_NULL_HANDLE;
        swapchain->initialized[index] = false;
    }
    swapchain->imageCount = 0;
    swapchain->handle = VK_NULL_HANDLE;
    swapchain->width = 0;
    swapchain->height = 0;

    bool presentPending = false;
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        if (device->presentContexts[index].presentPending &&
            device->presentContexts[index].swapchain == retired.handle) {
            presentPending = true;
            break;
        }
    }
    if (!presentPending) {
        GPU_QueueRetiredSwapchain(device, &retired);
        return;
    }
    for (uint32_t index = 0; index < GPU_MAX_SWAPCHAIN_IMAGES; ++index) {
        if (!device->retiredSwapchains[index].handle) {
            device->retiredSwapchains[index] = retired;
            return;
        }
    }
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        GPUPresentContext* context = &device->presentContexts[index];
        if (context->presentPending && context->swapchain == retired.handle) GPU_WaitPresentContext(device, context);
    }
    GPU_QueueRetiredSwapchain(device, &retired);
}

static void GPU_DestroyOwnedSwapchain(GPUDeviceState* device) {
    if (!device->swapchain) return;
    GPUSwapchain* swapchain = device->swapchain;
    GPUAssert(
        !swapchain->acquired && device->acquiredSwapchain != swapchain,
        "!swapchain->acquired && device->acquiredSwapchain != swapchain"
    );
    GPU_SwapRetireHandle(swapchain);
    memset(swapchain, 0, sizeof(GPUSwapchain));
    device->swapchain = NULL;
}

static VkCompositeAlphaFlagBitsKHR GPU_SwapChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    static const VkCompositeAlphaFlagBitsKHR choices[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (uint32_t index = 0; index < GPUArrayCount(choices); ++index) {
        if ((supported & (VkCompositeAlphaFlagsKHR)choices[index]) != 0) return choices[index];
    }
    return (VkCompositeAlphaFlagBitsKHR)0;
}

static bool GPU_SwapSurfaceConfigurationChanged(const GPUSwapchain* swapchain) {
    VkSurfaceCapabilitiesKHR capabilities = {0};
    GPU_RequireError(
        swapchain->state,
        GPU_ErrorFromVk(
            swapchain->state,
            swapchain->state->fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                swapchain->state->physicalDevice,
                swapchain->state->surface,
                &capabilities
            ),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
            0
        ),
        "Display surface query"
    );

    const VkExtent2D extent = capabilities.currentExtent;
    return extent.width == UINT_MAX || extent.height == UINT_MAX || extent.width != swapchain->width ||
           extent.height != swapchain->height || capabilities.currentTransform != swapchain->transform ||
           GPU_SwapChooseCompositeAlpha(capabilities.supportedCompositeAlpha) != swapchain->compositeAlpha;
}

static VkColorSpaceKHR GPU_SwapToVkColorSpace(GPUColorSpace colorSpace) {
    return colorSpace == GPUColorSpace_ExtendedSrgbLinear ? VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
                                                          : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}

// Non-sRGB color spaces require VK_EXT_swapchain_colorspace. Format support is only known from the surface query inside
// GPU_RecreateSwapchain.
static bool GPU_SupportsSwapchainMode(GPUDeviceState* device, GPUFormat format, VkColorSpaceKHR colorSpace) {
    (void)format;
    return colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR || device->swapchainColorSpaceEnabled;
}

static GPUError GPU_RecreateSwapchain(GPUSwapchain* swapchain) {
    GPUDeviceState* device = swapchain->state;
    GPUAssert(!swapchain->acquired && !device->acquiredSwapchain, "!swapchain->acquired && !device->acquiredSwapchain");
    const VkPresentModeKHR presentMode = GPU_SWAPCHAIN_PRESENT_MODE;
    const VkSurfacePresentModeKHR presentModeInfo = {
        .sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR,
        .presentMode = presentMode,
    };
    const VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        .pNext = &presentModeInfo,
        .surface = device->surface,
    };
    VkSurfaceCapabilities2KHR capabilitiesInfo = {
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
    };
    GPUError error = GPU_ErrorFromVk(
        device,
        device->fn.vkGetPhysicalDeviceSurfaceCapabilities2KHR(device->physicalDevice, &surfaceInfo, &capabilitiesInfo),
        "vkGetPhysicalDeviceSurfaceCapabilities2KHR",
        0
    );
    if (error != GPUError_None) return error;
    const VkSurfaceCapabilitiesKHR* capabilities = &capabilitiesInfo.surfaceCapabilities;

    const VkExtent2D extent = capabilities->currentExtent;
    if (extent.width == UINT_MAX) return GPUError_Unsupported;
    if ((capabilities->supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) return GPUError_Unsupported;

    VkSurfaceFormatKHR formats[GPU_MAX_SURFACE_FORMATS] = {0};
    uint32_t surfaceFormatCount = 0;
    error = GPU_ErrorFromVk(
        device,
        device->fn
            .vkGetPhysicalDeviceSurfaceFormatsKHR(device->physicalDevice, device->surface, &surfaceFormatCount, NULL),
        "vkGetPhysicalDeviceSurfaceFormatsKHR",
        0
    );
    if (error != GPUError_None) return error;
    if (surfaceFormatCount == 0 || surfaceFormatCount > GPU_MAX_SURFACE_FORMATS) return GPUError_Unsupported;
    error = GPU_ErrorFromVk(
        device,
        device->fn.vkGetPhysicalDeviceSurfaceFormatsKHR(
            device->physicalDevice,
            device->surface,
            &surfaceFormatCount,
            formats
        ),
        "vkGetPhysicalDeviceSurfaceFormatsKHR",
        0
    );
    if (error != GPUError_None) return error;

    const VkFormat requestedFormat = GPU_ToVkFormat(swapchain->format);
    bool formatSupported = false;
    for (uint32_t index = 0; index < surfaceFormatCount; ++index) {
        if ((formats[index].format == requestedFormat || formats[index].format == VK_FORMAT_UNDEFINED) &&
            formats[index].colorSpace == swapchain->colorSpace) {
            formatSupported = true;
            break;
        }
    }
    if (!formatSupported) return GPUError_Unsupported;

    if (extent.width == 0 || extent.height == 0) {
        swapchain->width = 0;
        swapchain->height = 0;
        swapchain->recreateRequired = true;
        return GPUError_None;
    }

    uint32_t requestedImageCount = device->presentContextCount;
    if (requestedImageCount < capabilities->minImageCount) requestedImageCount = capabilities->minImageCount;
    if (capabilities->maxImageCount != 0 && requestedImageCount > capabilities->maxImageCount)
        requestedImageCount = capabilities->maxImageCount;
    if (requestedImageCount == 0 || requestedImageCount > GPU_MAX_SWAPCHAIN_IMAGES) return GPUError_Unsupported;

    const VkCompositeAlphaFlagBitsKHR compositeAlpha =
        GPU_SwapChooseCompositeAlpha(capabilities->supportedCompositeAlpha);
    if (!compositeAlpha) {
        GPU_ReportKind(device, GPUMessageKind_SurfaceCompositeAlphaUnsupported);
        return GPUError_Unsupported;
    }
    const VkSwapchainPresentModesCreateInfoKHR presentModesInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR,
        .presentModeCount = 1,
        .pPresentModes = &presentMode,
    };
    const VkSwapchainCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = &presentModesInfo,
        .surface = device->surface,
        .minImageCount = requestedImageCount,
        .imageFormat = requestedFormat,
        .imageColorSpace = swapchain->colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
        .preTransform = capabilities->currentTransform,
        .compositeAlpha = compositeAlpha,
        .presentMode = presentMode,
        .clipped = VK_TRUE,
        .oldSwapchain = swapchain->handle,
    };
    VkSwapchainKHR newHandle = VK_NULL_HANDLE;
    error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateSwapchainKHR(device->device, &createInfo, NULL, &newHandle),
        "vkCreateSwapchainKHR",
        0
    );
    if (error != GPUError_None) {
        GPU_SwapRetireHandle(swapchain);
        return error;
    }

    VkImage images[GPU_MAX_SWAPCHAIN_IMAGES] = {0};
    uint32_t imageCount = 0;
    VkResult result = device->fn.vkGetSwapchainImagesKHR(device->device, newHandle, &imageCount, NULL);
    if (result != VK_SUCCESS || imageCount == 0 || imageCount > GPU_MAX_SWAPCHAIN_IMAGES) {
        device->fn.vkDestroySwapchainKHR(device->device, newHandle, NULL);
        GPU_SwapRetireHandle(swapchain);
        return result == VK_SUCCESS ? GPUError_Unsupported
                                    : GPU_ErrorFromVk(device, result, "vkGetSwapchainImagesKHR", 0);
    }
    result = device->fn.vkGetSwapchainImagesKHR(device->device, newHandle, &imageCount, images);
    if (result != VK_SUCCESS) {
        device->fn.vkDestroySwapchainKHR(device->device, newHandle, NULL);
        GPU_SwapRetireHandle(swapchain);
        return GPU_ErrorFromVk(device, result, "vkGetSwapchainImagesKHR", 0);
    }

    VkImageView views[GPU_MAX_SWAPCHAIN_IMAGES] = {0};
    for (uint32_t index = 0; index < imageCount; ++index) {
        const VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[index],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = requestedFormat,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        result = device->fn.vkCreateImageView(device->device, &viewInfo, NULL, &views[index]);
        if (result != VK_SUCCESS) {
            for (uint32_t created = 0; created < index; ++created) {
                device->fn.vkDestroyImageView(device->device, views[created], NULL);
            }
            device->fn.vkDestroySwapchainKHR(device->device, newHandle, NULL);
            GPU_SwapRetireHandle(swapchain);
            return GPU_ErrorFromVk(device, result, "vkCreateImageView (swapchain)", 0);
        }
    }

    GPU_SwapRetireHandle(swapchain);
    swapchain->handle = newHandle;
    swapchain->imageCount = imageCount;
    swapchain->width = extent.width;
    swapchain->height = extent.height;
    swapchain->transform = capabilities->currentTransform;
    swapchain->compositeAlpha = compositeAlpha;
    swapchain->recreateRequired = false;
    for (uint32_t index = 0; index < imageCount; ++index) {
        swapchain->images[index] = images[index];
        swapchain->renderViews[index] = (GPURenderView){
            .state = device,
            .view = views[index],
            .width = extent.width,
            .height = extent.height,
            .swapchainView = true,
        };
    }
    return GPUError_None;
}

// Creates the presentation retirement timeline, the present contexts, and the initial swapchain. The caller destroys
// everything on failure.
static GPUError GPU_CreateSwapchain(GPUDeviceState* device, const GPUDeviceDesc* desc) {
    const VkColorSpaceKHR colorSpace = GPU_SwapToVkColorSpace(desc->swapchainColorSpace);
    if (!GPU_SupportsSwapchainMode(device, desc->swapchainFormat, colorSpace)) return GPUError_Unsupported;

    const VkSemaphoreTypeCreateInfo typeInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    const VkSemaphoreCreateInfo semaphoreInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &typeInfo,
    };
    GPUError error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateSemaphore(device->device, &semaphoreInfo, NULL, &device->presentationRetirement),
        "vkCreateSemaphore (presentation)",
        0
    );
    if (error != GPUError_None) return error;
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        error = GPU_CreatePresentContext(device, &device->presentContexts[index]);
        if (error != GPUError_None) return error;
    }

    GPUSwapchain* swapchain = &device->swapchainStorage;
    memset(swapchain, 0, sizeof(GPUSwapchain));
    swapchain->state = device;
    swapchain->format = desc->swapchainFormat == GPUFormat_Undefined ? GPUFormat_Bgra8Srgb : desc->swapchainFormat;
    swapchain->colorSpace = colorSpace;
    swapchain->transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchain->compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    device->swapchain = swapchain;
    error = GPU_RecreateSwapchain(swapchain);
    if (error != GPUError_None) GPU_DestroyOwnedSwapchain(device);
    return error;
}

GPUError GPUDeviceSetSwapchainFormat(GPUDevice* deviceHandle, GPUFormat format, GPUColorSpace colorSpace) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && !device->acquiredSwapchain, "device && !device->acquiredSwapchain");
    const VkColorSpaceKHR requestedColorSpace = GPU_SwapToVkColorSpace(colorSpace);
    if (!device->swapchain || !GPU_SupportsSwapchainMode(device, format, requestedColorSpace))
        return GPUError_Unsupported;

    GPUSwapchain* swapchain = device->swapchain;
    if (swapchain->format == format && swapchain->colorSpace == requestedColorSpace) return GPUError_None;

    const GPUFormat oldFormat = swapchain->format;
    const VkColorSpaceKHR oldColorSpace = swapchain->colorSpace;
    swapchain->format = format;
    swapchain->colorSpace = requestedColorSpace;
    const GPUError error = GPU_RecreateSwapchain(swapchain);
    if (error != GPUError_None) {
        swapchain->format = oldFormat;
        swapchain->colorSpace = oldColorSpace;
        // vkCreateSwapchainKHR retires oldSwapchain even when creation fails.
        if (!swapchain->handle) GPU_RequireError(device, GPU_RecreateSwapchain(swapchain), "Swapchain recreation");
    }
    return error;
}

GPUExtent2 GPUDeviceGetDrawableExtent(GPUDevice* deviceHandle) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUDeviceGetDrawableExtent called with a null device");
    GPUAssert(!device->acquiredSwapchain, "GPUDeviceGetDrawableExtent must be called outside presentation");
    const GPUExtent2 none = {0};
    if (!device->swapchain) return none;

    VkSurfaceCapabilitiesKHR capabilities = {0};
    if (GPU_ErrorFromVk(
            device,
            device->fn
                .vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physicalDevice, device->surface, &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
            0
        ) != GPUError_None)
        return none;
    const VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT_MAX || extent.height == UINT_MAX) {
        GPU_ReportKind(device, GPUMessageKind_SurfaceExtentUnavailable);
        return none;
    }

    GPUSwapchain* swapchain = device->swapchain;
    if (swapchain->handle && (swapchain->width != extent.width || swapchain->height != extent.height))
        swapchain->recreateRequired = true;
    return (GPUExtent2){
        .x = extent.width,
        .y = extent.height,
    };
}

GPUSwapchainFrame GPUCmdAcquire(GPUCommandBuffer* commands) {
    GPUAssert(
        commands && commands->state && !commands->swapchain,
        "commands && commands->state && !commands->swapchain"
    );
    GPUDeviceState* device = commands->state;
    GPUAssert(
        device->swapchain && !device->swapchain->acquired && !device->acquiredSwapchain,
        "device->swapchain && !device->swapchain->acquired && !device->acquiredSwapchain"
    );
    GPU_PollPresentationRetirement(device);

    GPUSwapchain* swapchain = device->swapchain;
    GPUPresentContext* presentContext = NULL;
    const GPUSwapchainFrame none = {0};

    for (;;) {
        if (!swapchain->handle || swapchain->recreateRequired) {
            GPU_RequireError(device, GPU_RecreateSwapchain(swapchain), "Swapchain recreation");
            if (!swapchain->handle || swapchain->width == 0 || swapchain->height == 0) return none;
        }

        if (!presentContext) {
            presentContext = &device->presentContexts[device->nextPresentContext];
            GPU_WaitPresentContext(device, presentContext);
            GPUAssert(
                !presentContext->presentPending && !presentContext->swapchain,
                "!presentContext->presentPending && !presentContext->swapchain"
            );
        }

        uint32_t imageIndex = 0;
        const VkResult result = device->fn.vkAcquireNextImageKHR(
            device->device,
            swapchain->handle,
            ~(uint64_t)0,
            presentContext->acquired,
            VK_NULL_HANDLE,
            &imageIndex
        );
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain->recreateRequired = true;
            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            GPU_AbortVkFailure(device, result, "vkAcquireNextImageKHR", 0);
        GPUAssert(imageIndex < swapchain->imageCount, "swapchain returned an invalid image index");

        swapchain->imageIndex = imageIndex;
        swapchain->presentContext = presentContext;
        swapchain->acquired = true;
        swapchain->recreateRequired = result == VK_SUBOPTIMAL_KHR && GPU_SwapSurfaceConfigurationChanged(swapchain);
        device->acquiredSwapchain = swapchain;
        const VkImageMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout =
                swapchain->initialized[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain->images[imageIndex],
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        GPU_RecordImageBarriers(commands->state, commands->commandBuffer, &barrier, 1);
        commands->swapchain = swapchain;
        swapchain->transitionCommands = commands;
        return (GPUSwapchainFrame){
            .renderView = &swapchain->renderViews[imageIndex],
            .extent = {.x = swapchain->width, .y = swapchain->height},
        };
    }
}

void GPUDeviceSubmitAndPresent(GPUDevice* deviceHandle, const GPUSubmitDesc* desc) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "device");
    GPUAssert(desc->commands.data || desc->commands.count == 0, "desc->commands.data || desc->commands.count == 0");
    GPUSwapchain* swapchain = device->swapchain;
    GPUAssert(
        swapchain && swapchain->acquired && swapchain->transitionCommands && swapchain->presentContext,
        "swapchain && swapchain->acquired && swapchain->transitionCommands && swapchain->presentContext"
    );
#if defined(GPU_DEBUG)
    bool containsSwapchain = false;
    for (uint64_t index = 0; index < desc->commands.count; ++index) {
        GPUAssert(
            desc->commands.data[index] &&
                (!desc->commands.data[index]->swapchain || desc->commands.data[index] == swapchain->transitionCommands),
            "desc->commands.data[index] && (!desc->commands.data[index]->swapchain || desc->commands.data[index] == "
            "swapchain->transitionCommands)"
        );
        if (desc->commands.data[index] == swapchain->transitionCommands) containsSwapchain = true;
    }
    GPUAssert(containsSwapchain, "presentation submission must include the acquired command buffer");
#endif
    GPUPresentContext* presentContext = swapchain->presentContext;
    GPUAssert(
        !presentContext->presentPending && !presentContext->swapchain,
        "!presentContext->presentPending && !presentContext->swapchain"
    );
    GPU_AssertVk(device->fn.vkResetFences(device->device, 1, &presentContext->presented));
    GPU_SubmitCommands(device, desc, 0, presentContext->acquired, presentContext->rendered);
    swapchain->transitionCommands->swapchain = NULL;
    swapchain->transitionCommands = NULL;
    swapchain->initialized[swapchain->imageIndex] = true;

    const VkSwapchainPresentFenceInfoKHR fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .swapchainCount = 1,
        .pFences = &presentContext->presented,
    };
    const VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = &fenceInfo,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &presentContext->rendered,
        .swapchainCount = 1,
        .pSwapchains = &swapchain->handle,
        .pImageIndices = &swapchain->imageIndex,
    };
    const VkResult result = device->fn.vkQueuePresentKHR(device->queues[0].queue, &presentInfo);
    presentContext->presentPending = true;
    presentContext->swapchain = swapchain->handle;
    swapchain->presentContext = NULL;
    swapchain->acquired = false;
    device->acquiredSwapchain = NULL;
    device->nextPresentContext = (device->nextPresentContext + 1) % device->presentContextCount;
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        swapchain->recreateRequired = true;
    else if (result == VK_SUBOPTIMAL_KHR)
        swapchain->recreateRequired = swapchain->recreateRequired || GPU_SwapSurfaceConfigurationChanged(swapchain);
    else if (result != VK_SUCCESS)
        GPU_AbortVkFailure(device, result, "vkQueuePresentKHR", 0);
}
