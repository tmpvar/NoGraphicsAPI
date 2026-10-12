#pragma once
#include "shader_types.h"

#define shader_abi_thread_count 4u

struct ShaderAbiData {
    float scalar;
    F32x3 vector;
    F32x4 color;
    F32x3x3 matrix;
    F32x3 transformed;
};
GPU_SHARED_STRUCT(ShaderAbiData)

struct ShaderAbiRoot {
    float scalar;
    F32x3 vector;
    F32x4 color;
    ShaderAbiData* output;
    F32x3x3 matrix;
    F32x4* sampled;
    uint32_t texture_base;
    uint32_t sampler_base;
};
GPU_SHARED_STRUCT(ShaderAbiRoot)
