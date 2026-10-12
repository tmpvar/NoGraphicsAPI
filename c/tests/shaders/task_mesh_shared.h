#pragma once

#include "task_shader_common.h"

struct TaskMeshRoot {
    TaskTestData* data;
    uint32_t count;
    uint32_t visible_mask;
    F32x4 color;
};
GPU_SHARED_STRUCT(TaskMeshRoot)
