#include "gpu_api.h"
#include "gpu_internal.h"

GPUCommandPool* GPUCommandPoolCreate(GPUDevice* deviceHandle, uint32_t queueIndex) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && queueIndex < device->queueCount, "GPUCommandPoolCreate requires an available queue index");
    GPUCommandPool* pool = (GPUCommandPool*)GPU_PoolAlloc(&device->commandPoolPool);
    if (!pool) {
        GPU_ReportLimit(device, GPULimit_CommandPools);
        return NULL;
    }
    pool->state = device;
    pool->timestamps = device->queues[queueIndex].timestamps;
    pool->buffers = (GPUCommandBuffer*)(pool + 1);
    uint64_t** destinations = (uint64_t**)(pool->buffers + device->limits.maxCommandBuffersPerPool);
    uint64_t* results =
        (uint64_t*)(destinations + (uint64_t)device->limits.maxCommandBuffersPerPool * device->timestampQueryCount);
    for (uint32_t index = 0; index < device->limits.maxCommandBuffersPerPool; ++index) {
        pool->buffers[index].timestampDestinations = destinations + (uint64_t)index * device->timestampQueryCount;
        pool->buffers[index].timestampResults = results + (uint64_t)index * device->timestampQueryCount;
    }
    const VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = device->queues[queueIndex].familyIndex,
    };
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateCommandPool(device->device, &poolInfo, NULL, &pool->commandPool),
            "vkCreateCommandPool",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->commandPoolPool, pool);
        return NULL;
    }
    return pool;
}

void GPUCommandPoolDestroy(GPUCommandPool* pool) {
    if (!pool) return;
    for (uint32_t index = 0; index < pool->bufferCount; ++index) {
        GPUCommandBuffer* commands = &pool->buffers[index];
        GPUAssert(
            !commands->swapchain,
            "an acquired swapchain image must be presented before destroying its command pool"
        );
        if (commands->timestampPool)
            pool->state->fn.vkDestroyQueryPool(pool->state->device, commands->timestampPool, NULL);
    }
    pool->state->fn.vkDestroyCommandPool(pool->state->device, pool->commandPool, NULL);
    GPU_PoolFree(&pool->state->commandPoolPool, pool);
}

void GPUCommandPoolReset(GPUCommandPool* pool) {
    GPUAssert(pool && pool->state && pool->commandPool, "pool && pool->state && pool->commandPool");
    for (GPUCommandBuffer* commands = pool->first; commands; commands = commands->next) {
        GPUAssert(
            !commands->swapchain,
            "an acquired swapchain image must be presented before resetting its command pool"
        );
        commands->submitted = false;
        commands->timestampCount = 0;
    }
    GPU_AssertVk(pool->state->fn.vkResetCommandPool(pool->state->device, pool->commandPool, 0));
    pool->nextBuffer = pool->first;
}

void GPUCommandPoolReadTimestamps(GPUCommandPool* pool) {
    GPUAssert(pool && pool->state, "pool && pool->state");
    for (GPUCommandBuffer* commands = pool->first; commands; commands = commands->next) {
        if (!commands->submitted || commands->timestampCount == 0) continue;
        GPU_AssertVk(pool->state->fn.vkGetQueryPoolResults(
            pool->state->device,
            commands->timestampPool,
            0,
            commands->timestampCount,
            sizeof(uint64_t) * commands->timestampCount,
            commands->timestampResults,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT
        ));
        for (uint32_t index = 0; index < commands->timestampCount; ++index)
            *commands->timestampDestinations[index] = commands->timestampResults[index];
        commands->timestampCount = 0;
    }
}

GPUCommandBuffer* GPUCmdBegin(GPUCommandPool* pool) {
    GPUAssert(pool && pool->state && pool->commandPool, "pool && pool->state && pool->commandPool");
    GPUCommandBuffer* commands = pool->nextBuffer;
    if (commands) {
        pool->nextBuffer = commands->next;
    } else {
        GPUAssert(
            pool->bufferCount < pool->state->limits.maxCommandBuffersPerPool,
            "GPUCmdBegin exceeds maxCommandBuffersPerPool"
        );
        commands = &pool->buffers[pool->bufferCount];
        commands->state = pool->state;
        commands->commandPool = pool->commandPool;
        const VkCommandBufferAllocateInfo allocateInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool->commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        const VkResult allocated =
            pool->state->fn.vkAllocateCommandBuffers(pool->state->device, &allocateInfo, &commands->commandBuffer);
        if (GPU_ErrorFromVk(pool->state, allocated, "vkAllocateCommandBuffers", 0) != GPUError_None) {
            commands->commandBuffer = VK_NULL_HANDLE;
            return NULL;
        }
        if (pool->timestamps) {
            const VkQueryPoolCreateInfo queryInfo = {
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = pool->state->timestampQueryCount,
            };
            if (GPU_ErrorFromVk(
                    pool->state,
                    pool->state->fn.vkCreateQueryPool(pool->state->device, &queryInfo, NULL, &commands->timestampPool),
                    "vkCreateQueryPool",
                    0
                ) != GPUError_None) {
                pool->state->fn
                    .vkFreeCommandBuffers(pool->state->device, pool->commandPool, 1, &commands->commandBuffer);
                commands->commandBuffer = VK_NULL_HANDLE;
                commands->timestampPool = VK_NULL_HANDLE;
                return NULL;
            }
        }
        ++pool->bufferCount;
        if (pool->last)
            pool->last->next = commands;
        else
            pool->first = commands;
        pool->last = commands;
    }
    GPUAssert(!commands->swapchain, "!commands->swapchain");
    const VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    GPU_AssertVk(pool->state->fn.vkBeginCommandBuffer(commands->commandBuffer, &beginInfo));
    commands->timestampCount = 0;
    commands->submitted = false;
    commands->hasEpilogue = false;
    if (commands->timestampPool)
        pool->state->fn
            .vkResetQueryPool(pool->state->device, commands->timestampPool, 0, pool->state->timestampQueryCount);
    return commands;
}

void GPUCmdEnd(GPUCommandBuffer* commands) {
    GPUAssert(commands, "commands");
    VkCommandBuffer commandBuffer = commands->commandBuffer;
    if (commands->swapchain) {
        GPU_AssertVk(commands->state->fn.vkEndCommandBuffer(commandBuffer));
        if (!commands->epilogue) {
            const VkCommandBufferAllocateInfo allocateInfo = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = commands->commandPool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            GPU_RequireVk(
                commands->state,
                commands->state->fn
                    .vkAllocateCommandBuffers(commands->state->device, &allocateInfo, &commands->epilogue),
                "vkAllocateCommandBuffers (epilogue)",
                0
            );
        }
        const VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        commandBuffer = commands->epilogue;
        GPU_AssertVk(commands->state->fn.vkBeginCommandBuffer(commandBuffer, &beginInfo));
        commands->hasEpilogue = true;
        GPUSwapchain* swapchain = commands->swapchain;
        GPUAssert(
            swapchain->acquired && swapchain->transitionCommands == commands,
            "swapchain->acquired && swapchain->transitionCommands == commands"
        );
        const VkImageMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain->images[swapchain->imageIndex],
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
        };
        GPU_RecordImageBarriers(commands->state, commandBuffer, &barrier, 1);
    }
    GPU_AssertVk(commands->state->fn.vkEndCommandBuffer(commandBuffer));
}

void GPUCmdWriteTimestamp(GPUCommandBuffer* commands, uint64_t* cpuDestination, GPUStage stage) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    if (!commands->timestampPool) return;
    GPUAssert(
        commands->timestampCount < commands->state->timestampQueryCount,
        "commands->timestampCount < commands->state->timestampQueryCount"
    );
    GPUAssert(cpuDestination, "cpuDestination");
    commands->timestampDestinations[commands->timestampCount] = cpuDestination;
    commands->state->fn.vkCmdWriteTimestamp2(
        commands->commandBuffer,
        GPU_ToVkStages(stage),
        commands->timestampPool,
        commands->timestampCount++
    );
}

static void GPU_SubmitCommands(
    GPUDeviceState* device,
    const GPUSubmitDesc* desc,
    uint32_t queueIndex,
    VkSemaphore waitSemaphore,
    VkSemaphore signalSemaphore
) {
    GPUQueue* queue = &device->queues[queueIndex];
    GPUTimelineSemaphore* completion = desc->completion.semaphore;
    GPUAssert(completion, "completion");
    GPUAssert(desc->waits.data || desc->waits.count == 0, "desc->waits.data || desc->waits.count == 0");
    uint64_t commandCount = desc->commands.count;
    for (uint64_t index = 0; index < desc->commands.count; ++index) {
        GPUAssert(desc->commands.data[index], "desc->commands.data[index]");
        if (desc->commands.data[index]->hasEpilogue) ++commandCount;
    }
    GPUAssert(commandCount <= device->limits.maxSubmitCommandBuffers, "submit exceeds maxSubmitCommandBuffers");
    for (uint64_t index = 0; index < desc->commands.count; ++index) {
        queue->commandSubmitInfos[index] = (VkCommandBufferSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = desc->commands.data[index]->commandBuffer,
            .deviceMask = 1,
        };
    }
    // Presentation transitions must follow every suspended/resumed segment.
    uint64_t epilogueIndex = desc->commands.count;
    for (uint64_t index = 0; index < desc->commands.count; ++index) {
        GPUCommandBuffer* commands = desc->commands.data[index];
        if (commands->hasEpilogue) {
            queue->commandSubmitInfos[epilogueIndex++] = (VkCommandBufferSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = commands->epilogue,
                .deviceMask = 1,
            };
        }
    }
    const uint64_t waitCount = desc->waits.count + (waitSemaphore ? 1 : 0);
    GPUAssert(waitCount <= (size_t)device->limits.maxSubmitWaits + 1, "GPUDeviceSubmit exceeds maxSubmitWaits");
    for (uint64_t index = 0; index < desc->waits.count; ++index) {
        const GPUTimelinePoint point = desc->waits.data[index];
        GPUAssert(point.semaphore, "point.semaphore");
        queue->waitSubmitInfos[index] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = point.semaphore->semaphore,
            .value = point.value,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
    }
    if (waitSemaphore) {
        queue->waitSubmitInfos[desc->waits.count] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = waitSemaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
    }
    VkSemaphoreSubmitInfo signalInfos[3] = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = completion->semaphore,
            .value = desc->completion.value,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        },
    };
    uint32_t signalCount = 1;
    if (signalSemaphore) {
        signalInfos[signalCount++] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = signalSemaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        signalInfos[signalCount++] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = device->presentationRetirement,
            .value = GPU_NextPresentationRetirement(device),
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
    }
    const VkSubmitInfo2 submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = (uint32_t)waitCount,
        .pWaitSemaphoreInfos = queue->waitSubmitInfos,
        .commandBufferInfoCount = (uint32_t)commandCount,
        .pCommandBufferInfos = queue->commandSubmitInfos,
        .signalSemaphoreInfoCount = signalCount,
        .pSignalSemaphoreInfos = signalInfos,
    };
    GPU_AssertVk(device->fn.vkQueueSubmit2(queue->queue, 1, &submitInfo, VK_NULL_HANDLE));
    for (uint64_t index = 0; index < desc->commands.count; ++index) desc->commands.data[index]->submitted = true;
}

void GPUDeviceSubmit(GPUDevice* deviceHandle, const GPUSubmitDesc* desc, uint32_t queueIndex) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && desc && queueIndex < device->queueCount, "GPUDeviceSubmit requires an available queue index");
    GPUAssert(desc->commands.data || desc->commands.count == 0, "desc->commands.data || desc->commands.count == 0");
#if defined(GPU_DEBUG)
    for (uint64_t index = 0; index < desc->commands.count; ++index)
        GPUAssert(
            desc->commands.data[index] && !desc->commands.data[index]->swapchain,
            "swapchain commands require GPUDeviceSubmitAndPresent"
        );
#endif
    GPU_SubmitCommands(device, desc, queueIndex, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void GPUCmdBindPSO(GPUCommandBuffer* commands, const GPUPSO* pso) {
    GPUAssert(commands && pso, "commands && pso");
    commands->state->fn.vkCmdBindPipeline(commands->commandBuffer, pso->bindPoint, pso->pso);
}

static void GPU_CmdEmitRootPointer(GPUCommandBuffer* commands, const void* root) {
    if (!root) return;
    GPUAssert(((uintptr_t)root & 15u) == 0, "((uintptr_t)root & 15u) == 0");
    const VkPushDataInfoEXT info = {
        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .data = {.address = &root, .size = sizeof(root)},
    };
    commands->state->fn.vkCmdPushDataEXT(commands->commandBuffer, &info);
}

void GPUCmdSetTextureDescriptorHeap(GPUCommandBuffer* commands, GPUTextureDescriptorHeap* heap) {
    GPUAssert(commands && heap && commands->state == heap->state, "commands && heap && commands->state == heap->state");
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT* properties = &commands->state->heapProperties;
    VkBindHeapInfoEXT bindInfo = {0};
    GPU_MakeHeapBindInfo(
        GPUGpuRangeOfHeap(&heap->storage),
        properties->imageDescriptorAlignment > properties->bufferDescriptorAlignment
            ? properties->imageDescriptorAlignment
            : properties->bufferDescriptorAlignment,
        properties->minResourceHeapReservedRange,
        &bindInfo
    );
    commands->state->fn.vkCmdBindResourceHeapEXT(commands->commandBuffer, &bindInfo);
}

void GPUCmdSetSamplerDescriptorHeap(GPUCommandBuffer* commands, GPUSamplerDescriptorHeap* heap) {
    GPUAssert(commands && heap && commands->state == heap->state, "commands && heap && commands->state == heap->state");
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT* properties = &commands->state->heapProperties;
    VkBindHeapInfoEXT bindInfo = {0};
    GPU_MakeHeapBindInfo(
        GPUGpuRangeOfHeap(&heap->storage),
        properties->samplerDescriptorAlignment,
        properties->minSamplerHeapReservedRange,
        &bindInfo
    );
    commands->state->fn.vkCmdBindSamplerHeapEXT(commands->commandBuffer, &bindInfo);
}

void GPUCmdSetViewport(GPUCommandBuffer* commands, const GPUViewport* viewport) {
    GPUAssert(commands && viewport, "commands && viewport");
    const VkViewport vkViewport = {
        .x = viewport->x,
        .y = viewport->y,
        .width = viewport->width,
        .height = viewport->height,
        .minDepth = viewport->minDepth,
        .maxDepth = viewport->maxDepth == 0.0f ? 1.0f : viewport->maxDepth,
    };
    commands->state->fn.vkCmdSetViewportWithCount(commands->commandBuffer, 1, &vkViewport);
}

void GPUCmdSetScissor(GPUCommandBuffer* commands, const GPUScissor* scissor) {
    GPUAssert(commands && scissor, "commands && scissor");
    const VkRect2D vkScissor = {
        .offset = {.x = scissor->x, .y = scissor->y},
        .extent = {.width = scissor->width, .height = scissor->height},
    };
    commands->state->fn.vkCmdSetScissorWithCount(commands->commandBuffer, 1, &vkScissor);
}

void GPUCmdSetDepthStencil(GPUCommandBuffer* commands, const GPUDepthStencilState* state) {
    GPUAssert(commands && state, "commands && state");

    commands->state->fn.vkCmdSetDepthTestEnable(commands->commandBuffer, state->depthTest);
    if (state->depthTest) {
        commands->state->fn.vkCmdSetDepthWriteEnable(commands->commandBuffer, state->depthWrite);
        commands->state->fn.vkCmdSetDepthCompareOp(
            commands->commandBuffer,
            GPU_ToVkCompareOp(
                state->depthCompare == GPUCompareOp_Undefined ? GPUCompareOp_LessEqual : state->depthCompare
            )
        );
    }
    commands->state->fn.vkCmdSetStencilTestEnable(commands->commandBuffer, state->stencilTest);
    if (!state->stencilTest) return;

    commands->state->fn.vkCmdSetStencilOp(
        commands->commandBuffer,
        VK_STENCIL_FACE_FRONT_BIT,
        (VkStencilOp)state->front.fail,
        (VkStencilOp)state->front.pass,
        (VkStencilOp)state->front.depthFail,
        GPU_ToVkCompareOp(state->front.compare == GPUCompareOp_Undefined ? GPUCompareOp_Always : state->front.compare)
    );
    commands->state->fn.vkCmdSetStencilOp(
        commands->commandBuffer,
        VK_STENCIL_FACE_BACK_BIT,
        (VkStencilOp)state->back.fail,
        (VkStencilOp)state->back.pass,
        (VkStencilOp)state->back.depthFail,
        GPU_ToVkCompareOp(state->back.compare == GPUCompareOp_Undefined ? GPUCompareOp_Always : state->back.compare)
    );
    commands->state->fn
        .vkCmdSetStencilCompareMask(commands->commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, state->stencilReadMask);
    commands->state->fn
        .vkCmdSetStencilWriteMask(commands->commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, state->stencilWriteMask);
    commands->state->fn
        .vkCmdSetStencilReference(commands->commandBuffer, VK_STENCIL_FACE_FRONT_BIT, state->front.reference);
    commands->state->fn
        .vkCmdSetStencilReference(commands->commandBuffer, VK_STENCIL_FACE_BACK_BIT, state->back.reference);
}

void GPUCmdBeginRenderPass(GPUCommandBuffer* commands, const GPURenderingDesc* desc, GPURenderingFlags flags) {
    GPUAssert(
        commands && desc && (desc->colors.count == 0 || desc->colors.data) &&
            desc->colors.count <= GPU_MAX_COLOR_ATTACHMENTS,
        "commands && desc && (desc->colors.count == 0 || desc->colors.data) && desc->colors.count <= "
        "GPU_MAX_COLOR_ATTACHMENTS"
    );
    const GPURenderView* areaView = desc->colors.count
                                        ? desc->colors.data[0].renderView
                                        : (desc->depth.renderView ? desc->depth.renderView : desc->stencil.renderView);
    GPUAssert(areaView, "GPUCmdBeginRenderPass requires an attachment to determine the render area");

    VkRenderingAttachmentInfo colorAttachments[GPU_MAX_COLOR_ATTACHMENTS] = {0};
    for (uint64_t index = 0; index < desc->colors.count; ++index) {
        const GPUColorAttachment* attachment = &desc->colors.data[index];
        GPUAssert(attachment->renderView, "attachment->renderView");
        colorAttachments[index] = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = attachment->renderView->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = (VkAttachmentLoadOp)attachment->load,
            .storeOp = (VkAttachmentStoreOp)attachment->store,
            .clearValue = {
                .color = {
                    .float32 = {attachment->clear.x, attachment->clear.y, attachment->clear.z, attachment->clear.w}
                }
            },
        };
    }

    const VkRenderingAttachmentInfo depthAttachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = desc->depth.renderView ? desc->depth.renderView->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = (VkAttachmentLoadOp)desc->depth.load,
        .storeOp = (VkAttachmentStoreOp)desc->depth.store,
        .clearValue = {.depthStencil = {.depth = desc->depth.clear}},
    };
    const VkRenderingAttachmentInfo stencilAttachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = desc->stencil.renderView ? desc->stencil.renderView->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = (VkAttachmentLoadOp)desc->stencil.load,
        .storeOp = (VkAttachmentStoreOp)desc->stencil.store,
        .clearValue = {.depthStencil = {.stencil = desc->stencil.clear}},
    };
    const VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .flags = (VkRenderingFlags)flags,
        .renderArea = {.extent = {.width = areaView->width, .height = areaView->height}},
        .layerCount = 1,
        .colorAttachmentCount = (uint32_t)desc->colors.count,
        .pColorAttachments = colorAttachments,
        .pDepthAttachment = desc->depth.renderView ? &depthAttachment : NULL,
        .pStencilAttachment = desc->stencil.renderView ? &stencilAttachment : NULL,
    };
    commands->state->fn.vkCmdBeginRendering(commands->commandBuffer, &renderingInfo);

    const GPUViewport viewport = {.width = (float)areaView->width, .height = (float)areaView->height, .maxDepth = 1.0f};
    const GPUScissor scissor = {.width = areaView->width, .height = areaView->height};
    const GPUDepthStencilState depthStencil = {0};
    GPUCmdSetViewport(commands, &viewport);
    GPUCmdSetScissor(commands, &scissor);
    GPUCmdSetDepthStencil(commands, &depthStencil);
}

void GPUCmdEndRenderPass(GPUCommandBuffer* commands) {
    GPUAssert(commands, "commands");
    commands->state->fn.vkCmdEndRendering(commands->commandBuffer);
}

void GPUCmdDraw(
    GPUCommandBuffer* commands,
    const void* root,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance
) {
    GPUAssert(commands, "commands");
    GPU_CmdEmitRootPointer(commands, root);
    commands->state->fn.vkCmdDraw(commands->commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void GPUCmdDrawIndexed(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange indices,
    GPUIndexType type,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance
) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    const VkBindIndexBuffer3InfoKHR bindInfo = {
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange = {.address = (VkDeviceAddress)(uintptr_t)indices.gpu, .size = indices.size},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .indexType = (VkIndexType)type,
    };
    commands->state->fn.vkCmdBindIndexBuffer3KHR(commands->commandBuffer, &bindInfo);
    commands->state->fn
        .vkCmdDrawIndexed(commands->commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void GPUCmdDrawIndirect(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange arguments,
    uint32_t drawCount,
    uint32_t stride
) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    const VkDrawIndirect2InfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange =
            {.address = (VkDeviceAddress)(uintptr_t)arguments.gpu,
             .size = arguments.size,
             .stride = stride == 0 ? sizeof(VkDrawIndirectCommand) : stride},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .drawCount = drawCount,
    };
    commands->state->fn.vkCmdDrawIndirect2KHR(commands->commandBuffer, &info);
}

void GPUCmdDrawIndexedIndirect(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange indices,
    GPUIndexType type,
    GPUGpuRange arguments,
    uint32_t drawCount,
    uint32_t stride
) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    const VkBindIndexBuffer3InfoKHR bindInfo = {
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange = {.address = (VkDeviceAddress)(uintptr_t)indices.gpu, .size = indices.size},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .indexType = (VkIndexType)type,
    };
    commands->state->fn.vkCmdBindIndexBuffer3KHR(commands->commandBuffer, &bindInfo);
    const VkDrawIndirect2InfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange =
            {.address = (VkDeviceAddress)(uintptr_t)arguments.gpu,
             .size = arguments.size,
             .stride = stride == 0 ? sizeof(VkDrawIndexedIndirectCommand) : stride},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .drawCount = drawCount,
    };
    commands->state->fn.vkCmdDrawIndexedIndirect2KHR(commands->commandBuffer, &info);
}

void GPUCmdDispatch(GPUCommandBuffer* commands, const void* root, GPUExtent3 groupCount) {
    GPUAssert(commands, "commands");
    GPU_CmdEmitRootPointer(commands, root);
    commands->state->fn.vkCmdDispatch(commands->commandBuffer, groupCount.x, groupCount.y, groupCount.z);
}

void GPUCmdDispatchIndirect(GPUCommandBuffer* commands, const void* root, GPUGpuRange arguments) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    const VkDispatchIndirect2InfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DISPATCH_INDIRECT_2_INFO_KHR,
        .addressRange = {.address = (VkDeviceAddress)(uintptr_t)arguments.gpu, .size = arguments.size},
        .addressFlags = GPU_ADDRESS_FLAGS,
    };
    commands->state->fn.vkCmdDispatchIndirect2KHR(commands->commandBuffer, &info);
}

void GPUCmdDrawMeshlets(GPUCommandBuffer* commands, const void* root, GPUExtent3 groupCount) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    commands->state->fn.vkCmdDrawMeshTasksEXT(commands->commandBuffer, groupCount.x, groupCount.y, groupCount.z);
}

void GPUCmdDrawMeshletsIndirect(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange arguments,
    uint32_t drawCount,
    uint32_t stride
) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPUAssert(commands->state->caps.indirectMeshDraw, "commands->state->caps.indirectMeshDraw");
    GPU_CmdEmitRootPointer(commands, root);
    const VkDrawIndirect2InfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange =
            {.address = (VkDeviceAddress)(uintptr_t)arguments.gpu,
             .size = arguments.size,
             .stride = stride == 0 ? sizeof(VkDrawMeshTasksIndirectCommandEXT) : stride},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .drawCount = drawCount,
    };
    commands->state->fn.vkCmdDrawMeshTasksIndirect2EXT(commands->commandBuffer, &info);
}

void GPUCmdCopyMemory(GPUCommandBuffer* commands, GPUGpuRange source, GPUGpuRange destination) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    const VkDeviceMemoryCopyKHR region = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
        .srcRange = {.address = (VkDeviceAddress)(uintptr_t)source.gpu, .size = source.size},
        .srcFlags = GPU_ADDRESS_FLAGS,
        .dstRange = {.address = (VkDeviceAddress)(uintptr_t)destination.gpu, .size = destination.size},
        .dstFlags = GPU_ADDRESS_FLAGS,
    };
    const VkCopyDeviceMemoryInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.vkCmdCopyMemoryKHR(commands->commandBuffer, &info);
}

void GPUCmdCopyMemoryToTexture(
    GPUCommandBuffer* commands,
    GPUGpuRange source,
    GPUTexture* destination,
    const GPUTextureCopyDesc* copy
) {
    GPUAssert(commands && commands->state && destination && copy, "commands && commands->state && destination && copy");
    const VkDeviceMemoryImageCopyKHR region = GPU_MakeTextureCopyRegion(destination, copy, source);
    const VkCopyDeviceMemoryImageInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .image = destination->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.vkCmdCopyMemoryToImageKHR(commands->commandBuffer, &info);
}

void GPUCmdCopyTextureToMemory(
    GPUCommandBuffer* commands,
    GPUTexture* source,
    GPUGpuRange destination,
    const GPUTextureCopyDesc* copy
) {
    GPUAssert(commands && commands->state && source && copy, "commands && commands->state && source && copy");
    const VkDeviceMemoryImageCopyKHR region = GPU_MakeTextureCopyRegion(source, copy, destination);
    const VkCopyDeviceMemoryImageInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .image = source->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.vkCmdCopyImageToMemoryKHR(commands->commandBuffer, &info);
}

void GPUCmdBarrier(
    GPUCommandBuffer* commands,
    GPUStage before,
    GPUAccess beforeAccess,
    GPUStage after,
    GPUAccess afterAccess
) {
    GPUAssert(commands, "commands");
    GPU_RecordBarrier(commands->state, commands->commandBuffer, before, beforeAccess, after, afterAccess);
}
