set(shader ${CMAKE_SOURCE_DIR}/../examples/triangle/triangle.slang)
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(vertex_code ${shader_dir}/triangle.vert.spv)
set(fragment_code ${shader_dir}/triangle.frag.spv)

GPU_compile_slang(${vertex_code} ${shader} vertexMain vertex)
GPU_compile_slang(${fragment_code} ${shader} fragmentMain fragment)

GPU_add_example(example_triangle
    triangle.c
    ${shader}
    ${vertex_code}
    ${fragment_code}
)
target_compile_definitions(example_triangle PRIVATE
    GPU_VERTEX_SHADER_PATH="${vertex_code}"
    GPU_FRAGMENT_SHADER_PATH="${fragment_code}"
)
