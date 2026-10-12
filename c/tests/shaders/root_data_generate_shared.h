#pragma once
#include "root_data_shared.h"

struct RootDataGenerate {
    RootData* destination;
    uint32_t* output;
    uint32_t* dispatch_arguments;
    uint32_t first_index;
    uint32_t seed;
};
GPU_SHARED_STRUCT(RootDataGenerate)
