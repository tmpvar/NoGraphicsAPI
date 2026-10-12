#pragma once
#include "shader_types.h"

struct RootData {
    uint32_t* output;
    uint32_t output_index;
    uint32_t seed;
    uint32_t values[60];
};
GPU_SHARED_STRUCT(RootData)
