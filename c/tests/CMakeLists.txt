find_package(Threads REQUIRED)

set(GPU_TEST_FAIL_REGEX "Validation Error;NoGraphicsAPI validation:")
set(GPU_TEST_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(GPU_TEST_SHADER_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)

# Adds a C99 test executable linked against gpu.h. Extra arguments are additional sources, such as compiled shader outputs.
function(GPU_add_test target source)
    add_executable(${target} ${source} ${ARGN})
    target_link_libraries(${target} PRIVATE gpu Threads::Threads)
    GPU_set_warnings(${target})
endfunction()

GPU_add_test(test_api api_test.c)
add_test(NAME test_api COMMAND test_api)

GPU_add_test(test_command_context command_context_test.c)
add_test(NAME test_command_context COMMAND test_command_context)

GPU_add_test(test_multithreading multithreading_test.c)
add_test(NAME test_multithreading COMMAND test_multithreading)
add_test(NAME test_multiple_queues COMMAND test_multithreading --multiple-queues)
add_test(NAME test_queue_family_threads COMMAND test_multithreading --queue-families)
set_tests_properties(test_command_context test_multithreading test_multiple_queues test_queue_family_threads PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "${GPU_TEST_FAIL_REGEX}")

set(queue_family_spv ${GPU_TEST_SHADER_BINARY_DIR}/queue_family.comp.spv)
GPU_compile_slang(${queue_family_spv} ${GPU_TEST_SHADER_DIR}/queue_family.slang computeMain compute
    DEPENDS ${GPU_TEST_SHADER_DIR}/queue_family_shared.h ${GPU_TEST_SHADER_DIR}/shader_types.h)
GPU_add_test(test_queue_families queue_family_test.c ${queue_family_spv})
target_compile_definitions(test_queue_families PRIVATE GPU_QUEUE_FAMILY_SPV="${queue_family_spv}")
add_test(NAME test_queue_families COMMAND test_queue_families)
add_test(NAME test_compute_queue COMMAND test_queue_families --compute)
add_test(NAME test_copy_queue COMMAND test_queue_families --copy)
set_tests_properties(test_queue_families test_compute_queue test_copy_queue PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "${GPU_TEST_FAIL_REGEX}")

set(continuation_vertex_spv ${GPU_TEST_SHADER_BINARY_DIR}/render_continuation.vert.spv)
set(continuation_fragment_spv ${GPU_TEST_SHADER_BINARY_DIR}/render_continuation.frag.spv)
GPU_compile_slang(${continuation_vertex_spv} ${GPU_TEST_SHADER_DIR}/render_continuation.slang vertexMain vertex)
GPU_compile_slang(${continuation_fragment_spv} ${GPU_TEST_SHADER_DIR}/render_continuation.slang fragmentMain fragment)
GPU_add_test(test_render_continuation render_continuation_test.c ${continuation_vertex_spv} ${continuation_fragment_spv})
target_compile_definitions(test_render_continuation PRIVATE
    GPU_CONTINUATION_VERTEX_SPV="${continuation_vertex_spv}"
    GPU_CONTINUATION_FRAGMENT_SPV="${continuation_fragment_spv}")
add_test(NAME test_render_continuation COMMAND test_render_continuation)
set_tests_properties(test_render_continuation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "${GPU_TEST_FAIL_REGEX}")

if(WIN32)
    GPU_add_test(test_presentation presentation_test.c)
    add_test(NAME test_presentation COMMAND test_presentation)
    add_test(NAME test_presentation_scrgb COMMAND test_presentation --scrgb)
    add_test(NAME test_presentation_color_space_switch COMMAND test_presentation --switch-color-space)
    add_test(NAME test_presentation_queue_families COMMAND test_presentation --queue-families)
    set_tests_properties(test_presentation test_presentation_scrgb test_presentation_color_space_switch test_presentation_queue_families PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "NoGraphicsAPI validation:")
endif()

GPU_add_test(test_texture_copy texture_copy_test.c)
add_test(NAME test_texture_copy COMMAND test_texture_copy)
set_tests_properties(test_texture_copy PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "${GPU_TEST_FAIL_REGEX}")

GPU_add_test(test_texture_alias texture_alias_test.c)
add_test(NAME test_texture_alias COMMAND test_texture_alias)
set_tests_properties(test_texture_alias PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "${GPU_TEST_FAIL_REGEX}")

set(task_shader_code ${GPU_TEST_SHADER_BINARY_DIR}/task_shader.task.spv)
set(task_mesh_code ${GPU_TEST_SHADER_BINARY_DIR}/task_mesh.mesh.spv)
set(task_fragment_code ${GPU_TEST_SHADER_BINARY_DIR}/task_mesh.frag.spv)
GPU_compile_slang(${task_shader_code} ${GPU_TEST_SHADER_DIR}/task_shader.slang taskMain amplification
    DEPENDS ${GPU_TEST_SHADER_DIR}/task_shader_shared.h ${GPU_TEST_SHADER_DIR}/task_shader_common.h ${GPU_TEST_SHADER_DIR}/shader_types.h)
GPU_compile_slang(${task_mesh_code} ${GPU_TEST_SHADER_DIR}/task_mesh.slang meshMain mesh
    DEPENDS ${GPU_TEST_SHADER_DIR}/task_mesh_shared.h ${GPU_TEST_SHADER_DIR}/task_shader_common.h ${GPU_TEST_SHADER_DIR}/shader_types.h)
GPU_compile_slang(${task_fragment_code} ${GPU_TEST_SHADER_DIR}/task_mesh.slang fragmentMain fragment
    DEPENDS ${GPU_TEST_SHADER_DIR}/task_mesh_shared.h ${GPU_TEST_SHADER_DIR}/task_shader_common.h ${GPU_TEST_SHADER_DIR}/shader_types.h)
GPU_add_test(test_task_shader task_shader_test.c ${task_shader_code} ${task_mesh_code} ${task_fragment_code})
target_compile_definitions(test_task_shader PRIVATE
    GPU_TASK_TEST_TASK_SHADER="${task_shader_code}"
    GPU_TASK_TEST_MESH_SHADER="${task_mesh_code}"
    GPU_TASK_TEST_FRAGMENT_SHADER="${task_fragment_code}")
add_test(NAME test_task_shader COMMAND test_task_shader)
set_tests_properties(test_task_shader PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "${GPU_TEST_FAIL_REGEX}")

set(root_data_code ${GPU_TEST_SHADER_BINARY_DIR}/root_data.comp.spv)
set(root_aligned_code ${GPU_TEST_SHADER_BINARY_DIR}/root_data_aligned.comp.spv)
set(root_generate_code ${GPU_TEST_SHADER_BINARY_DIR}/root_data_generate.comp.spv)
GPU_compile_slang(${root_data_code} ${GPU_TEST_SHADER_DIR}/root_data.slang computeMain compute
    DEPENDS ${GPU_TEST_SHADER_DIR}/root_data_shared.h ${GPU_TEST_SHADER_DIR}/shader_types.h)
GPU_compile_slang(${root_aligned_code} ${GPU_TEST_SHADER_DIR}/root_data.slang computeMain compute
    DEFINES ROOT_DATA_ALIGNMENT=16 DEPENDS ${GPU_TEST_SHADER_DIR}/root_data_shared.h ${GPU_TEST_SHADER_DIR}/shader_types.h)
GPU_compile_slang(${root_generate_code} ${GPU_TEST_SHADER_DIR}/root_data_generate.slang computeMain compute
    DEPENDS ${GPU_TEST_SHADER_DIR}/root_data_generate_shared.h ${GPU_TEST_SHADER_DIR}/root_data_shared.h ${GPU_TEST_SHADER_DIR}/shader_types.h)
GPU_add_test(test_root_data root_data_test.c ${root_data_code} ${root_aligned_code} ${root_generate_code})
target_compile_definitions(test_root_data PRIVATE GPU_ROOT_DATA_PATH="${root_data_code}" GPU_ROOT_GENERATE_PATH="${root_generate_code}")
add_test(NAME test_root_data COMMAND test_root_data)
add_test(NAME test_root_data_aligned COMMAND test_root_data ${root_aligned_code})
set_tests_properties(test_root_data test_root_data_aligned PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "${GPU_TEST_FAIL_REGEX}")

set(shader_abi_code ${GPU_TEST_SHADER_BINARY_DIR}/shader_abi.comp.spv)
GPU_compile_slang(${shader_abi_code} ${GPU_TEST_SHADER_DIR}/shader_abi.slang computeMain compute
    DEPENDS ${GPU_TEST_SHADER_DIR}/shader_abi_shared.h ${GPU_TEST_SHADER_DIR}/shader_types.h)
GPU_add_test(test_shader_abi shader_abi_test.c ${shader_abi_code})
target_compile_definitions(test_shader_abi PRIVATE GPU_SHADER_ABI_PATH="${shader_abi_code}")
add_test(NAME test_shader_abi COMMAND test_shader_abi)
set_tests_properties(test_shader_abi PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "${GPU_TEST_FAIL_REGEX}")

# Compiles the library implementation itself instead of linking the gpu target, so the test can wrap the run-time Vulkan entry point table.
add_executable(test_startup_failures startup_failures_test.c ${GPU_HEADER} ${queue_family_spv} ${continuation_vertex_spv} ${continuation_fragment_spv})
target_include_directories(test_startup_failures PRIVATE $<TARGET_PROPERTY:gpu,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(test_startup_failures PRIVATE Threads::Threads)
GPU_set_warnings(test_startup_failures)
target_compile_definitions(test_startup_failures PRIVATE
    GPU_FAILURE_COMPUTE="${queue_family_spv}"
    GPU_FAILURE_VERTEX="${continuation_vertex_spv}"
    GPU_FAILURE_FRAGMENT="${continuation_fragment_spv}")
add_test(NAME test_startup_failures COMMAND test_startup_failures)
set_tests_properties(test_startup_failures PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 FAIL_REGULAR_EXPRESSION "${GPU_TEST_FAIL_REGEX}")
