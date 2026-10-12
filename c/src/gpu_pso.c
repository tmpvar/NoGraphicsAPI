#include "gpu_api.h"
#include "gpu_internal.h"

static void GPU_PsoAssertSpirv(GPUByteSpan code) {
    GPUAssert(
        code.data && code.size >= 5 * sizeof(uint32_t) && code.size % sizeof(uint32_t) == 0 &&
            (size_t)code.data % sizeof(uint32_t) == 0,
        "PSO stages require aligned SPIR-V binaries"
    );
    GPUAssert(*(const uint32_t*)code.data == 0x07230203u, "PSO stage does not contain SPIR-V");
    (void)code;
}

static const VkDescriptorSetAndBindingMappingEXT GPU_PsoRootMapping = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
    .bindingCount = 1,
    .resourceMask =
        VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT,
    .source = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT,
    .sourceData = {.pushAddressOffset = 0},
};

static GPUPSO* GPU_PsoCreateRaster(
    GPUDeviceState* device,
    const GPUShaderStage* firstStage,
    const GPUShaderStage* fragment,
    GPUColorTargetDescSpan colorTargets,
    GPUFormat depthFormat,
    GPUFormat stencilFormat,
    const GPURasterizationState* rasterizationState,
    bool mesh,
    const GPUShaderStage* task
) {
    GPUAssert(device, "PSO creation called with a null device");
    GPU_PsoAssertSpirv(firstStage->code);
    if (task->code.size) GPU_PsoAssertSpirv(task->code);
    if (fragment->code.size) GPU_PsoAssertSpirv(fragment->code);
    GPUAssert(
        (colorTargets.count == 0 || colorTargets.data) && colorTargets.count <= GPU_MAX_COLOR_ATTACHMENTS,
        "color targets must fit the wrapper's attachment array"
    );

    GPUPSO* result = (GPUPSO*)GPU_PoolAlloc(&device->psoPool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_Psos);
        return NULL;
    }

    const VkShaderModuleCreateInfo taskModuleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = task->code.size,
        .pCode = (const uint32_t*)task->code.data,
    };
    const VkShaderModuleCreateInfo firstStageModuleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = firstStage->code.size,
        .pCode = (const uint32_t*)firstStage->code.data,
    };
    const VkShaderModuleCreateInfo fragmentModuleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fragment->code.size,
        .pCode = (const uint32_t*)fragment->code.data,
    };
    const VkShaderDescriptorSetAndBindingMappingInfoEXT mappings[] = {
        {.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
         .pNext = &taskModuleInfo,
         .mappingCount = 1,
         .pMappings = &GPU_PsoRootMapping},
        {.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
         .pNext = &firstStageModuleInfo,
         .mappingCount = 1,
         .pMappings = &GPU_PsoRootMapping},
        {.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
         .pNext = &fragmentModuleInfo,
         .mappingCount = 1,
         .pMappings = &GPU_PsoRootMapping},
    };
    const VkPipelineShaderStageCreateInfo stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &mappings[0],
            .stage = VK_SHADER_STAGE_TASK_BIT_EXT,
            .pName = task->entryPoint ? task->entryPoint : "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &mappings[1],
            .stage = mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,
            .pName = firstStage->entryPoint ? firstStage->entryPoint : "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &mappings[2],
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pName = fragment->entryPoint ? fragment->entryPoint : "main",
        },
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode =
            (VkCullModeFlags)(rasterizationState->cull == GPUCullMode_None ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT),
        .frontFace = rasterizationState->cull == GPUCullMode_CounterClockwise ? VK_FRONT_FACE_CLOCKWISE
                                                                              : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = rasterizationState->depthBiasConstant != 0.0f ||
                           rasterizationState->depthBiasClamp != 0.0f || rasterizationState->depthBiasSlope != 0.0f,
        .depthBiasConstantFactor = rasterizationState->depthBiasConstant,
        .depthBiasClamp = rasterizationState->depthBiasClamp,
        .depthBiasSlopeFactor = rasterizationState->depthBiasSlope,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    VkPipelineColorBlendAttachmentState colorAttachments[GPU_MAX_COLOR_ATTACHMENTS] = {0};
    for (uint64_t index = 0; index < colorTargets.count; ++index) {
        const GPUColorTargetDesc* target = &colorTargets.data[index];
        colorAttachments[index] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = target->blend.enabled,
            .srcColorBlendFactor = GPU_ToVkBlendFactor(target->blend.color.source),
            .dstColorBlendFactor = GPU_ToVkBlendFactor(target->blend.color.destination),
            .colorBlendOp = (VkBlendOp)target->blend.color.operation,
            .srcAlphaBlendFactor = GPU_ToVkBlendFactor(target->blend.alpha.source),
            .dstAlphaBlendFactor = GPU_ToVkBlendFactor(target->blend.alpha.destination),
            .alphaBlendOp = (VkBlendOp)target->blend.alpha.operation,
            .colorWriteMask = target->writeMask ? target->writeMask : 0xf,
        };
    }
    const VkPipelineColorBlendStateCreateInfo colorBlend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = (uint32_t)colorTargets.count,
        .pAttachments = colorTargets.count ? colorAttachments : NULL,
    };
    static const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    const VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = (uint32_t)GPUArrayCount(dynamicStates),
        .pDynamicStates = dynamicStates,
    };
    VkFormat colorFormats[GPU_MAX_COLOR_ATTACHMENTS] = {0};
    for (uint64_t index = 0; index < colorTargets.count; ++index) {
        colorFormats[index] = GPU_ToVkFormat(colorTargets.data[index].format);
    }
    const VkPipelineRenderingCreateInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = (uint32_t)colorTargets.count,
        .pColorAttachmentFormats = colorTargets.count ? colorFormats : NULL,
        .depthAttachmentFormat = GPU_ToVkFormat(depthFormat),
        .stencilAttachmentFormat = GPU_ToVkFormat(stencilFormat),
    };
    const VkPipelineCreateFlags2CreateInfo flagsInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = &renderingInfo,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const VkGraphicsPipelineCreateInfo psoInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &flagsInfo,
        .stageCount = 1u + (task->code.size ? 1u : 0u) + (fragment->code.size ? 1u : 0u),
        .pStages = stages + (task->code.size ? 0u : 1u),
        .pVertexInputState = mesh ? NULL : &vertexInput,
        .pInputAssemblyState = mesh ? NULL : &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlend,
        .pDynamicState = &dynamicState,
        .basePipelineIndex = -1,
    };
    result->state = device;
    result->bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1, &psoInfo, NULL, &result->pso),
            "vkCreateGraphicsPipelines",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->psoPool, result);
        return NULL;
    }
    return result;
}

GPUPSO* GPUPSOCreateGraphics(GPUDevice* deviceHandle, const GPUGraphicsPSODesc* desc) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    const GPUShaderStage noTask = {0};
    return GPU_PsoCreateRaster(
        device,
        &desc->vertex,
        &desc->fragment,
        desc->colorTargets,
        desc->depthFormat,
        desc->stencilFormat,
        &desc->rasterization,
        false,
        &noTask
    );
}

GPUPSO* GPUPSOCreateMesh(GPUDevice* deviceHandle, const GPUMeshPSODesc* desc) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    return GPU_PsoCreateRaster(
        device,
        &desc->mesh,
        &desc->fragment,
        desc->colorTargets,
        desc->depthFormat,
        desc->stencilFormat,
        &desc->rasterization,
        true,
        &desc->task
    );
}

GPUPSO* GPUPSOCreateCompute(GPUDevice* deviceHandle, const GPUShaderStage* compute) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUPSOCreateCompute called with a null device");
    GPU_PsoAssertSpirv(compute->code);

    GPUPSO* result = (GPUPSO*)GPU_PoolAlloc(&device->psoPool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_Psos);
        return NULL;
    }

    const VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = compute->code.size,
        .pCode = (const uint32_t*)compute->code.data,
    };
    const VkShaderDescriptorSetAndBindingMappingInfoEXT mapping = {
        .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
        .pNext = &moduleInfo,
        .mappingCount = 1,
        .pMappings = &GPU_PsoRootMapping,
    };
    const VkPipelineCreateFlags2CreateInfo flagsInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const VkComputePipelineCreateInfo psoInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = &flagsInfo,
        .stage =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = &mapping,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .pName = compute->entryPoint ? compute->entryPoint : "main",
            },
        .basePipelineIndex = -1,
    };
    result->state = device;
    result->bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateComputePipelines(device->device, VK_NULL_HANDLE, 1, &psoInfo, NULL, &result->pso),
            "vkCreateComputePipelines",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->psoPool, result);
        return NULL;
    }
    return result;
}

void GPUPSODestroy(GPUPSO* pso) {
    if (!pso) return;
    pso->state->fn.vkDestroyPipeline(pso->state->device, pso->pso, NULL);
    GPU_PoolFree(&pso->state->psoPool, pso);
}
