#pragma once

#include "shader_types.h"

#define task_test_batch_size 32u
#define task_test_capacity 96u
#define task_test_batches (task_test_capacity / task_test_batch_size)
#define task_test_width (task_test_capacity * 4u)
#define task_test_height 8u

struct TaskTestPayload {
    uint32_t ids[task_test_batch_size];
};
GPU_SHARED_STRUCT(TaskTestPayload)

struct TaskTestData {
    uint32_t visibility[task_test_capacity];
    uint32_t batch_counts[task_test_batches];
    uint32_t visits[task_test_capacity + 1];
};
GPU_SHARED_STRUCT(TaskTestData)
