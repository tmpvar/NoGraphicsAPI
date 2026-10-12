// Storage types shared by the test shaders and their C hosts. Slang sees its built-in vectors; C sees structs with the
// scalar layout that -fvk-use-c-layout expects, so shared structs have identical offsets on both sides.
#pragma once

#if defined(__SLANG__)
typedef float2 F32x2;
typedef float3 F32x3;
typedef float4 F32x4;
typedef float3x3 F32x3x3;
typedef float4x4 F32x4x4;
#define GPU_SHARED_STRUCT(name)
#else
#include "gpu.h"
typedef struct F32x2 {
    float x, y;
} F32x2;
typedef struct F32x3 {
    float x, y, z;
} F32x3;
typedef struct F32x4 {
    float x, y, z, w;
} F32x4;
typedef struct F32x3x3 {
    F32x3 rows[3]; // Row-major, matching -matrix-layout-row-major.
} F32x3x3;
typedef struct F32x4x4 {
    F32x4 rows[4];
} F32x4x4;
GPUStaticAssert(
    sizeof(F32x3) == 12 && sizeof(F32x4) == 16 && sizeof(F32x3x3) == 36 && sizeof(F32x4x4) == 64,
    shared_vector_layout
);
#define GPU_SHARED_STRUCT(name) typedef struct name name;
#endif
