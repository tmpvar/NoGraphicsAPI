#pragma once

#include "shader_types.h"

#define queue_test_width 17u
#define queue_test_height 9u

struct QueueFamilyRoot {
    uint32_t* source;
    uint32_t* destination;
    uint32_t source_texture;
    uint32_t destination_texture;
};
GPU_SHARED_STRUCT(QueueFamilyRoot)
