cmake_minimum_required(VERSION 3.24)

project(GPU VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "gpu.h requires a 64-bit target")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(GPU_BUILD_EXAMPLES "Build gpu.h examples" ON)
option(GPU_BUILD_TESTS "Build gpu.h tests" ON)

find_package(Vulkan 1.4.357 REQUIRED)
message(STATUS "Using Vulkan ${Vulkan_VERSION}: ${Vulkan_LIBRARY}")
message(STATUS "Using Vulkan headers: ${Vulkan_INCLUDE_DIR}")

# Amalgamate c/src into the single header. The checked-in c/gpu.h is refreshed from the build output so it stays current.
file(GLOB GPU_MODULE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
set(GPU_HEADER ${CMAKE_CURRENT_BINARY_DIR}/gpu.h)
add_custom_command(
    OUTPUT ${GPU_HEADER}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${GPU_HEADER} -P ${CMAKE_CURRENT_SOURCE_DIR}/amalgamate.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GPU_HEADER} ${CMAKE_CURRENT_SOURCE_DIR}/gpu.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/amalgamate.cmake ${GPU_MODULE_SOURCES}
    VERBATIM
    COMMENT "Amalgamating gpu.h"
)

set(GPU_IMPL_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/gpu_impl.c)
file(CONFIGURE OUTPUT ${GPU_IMPL_SOURCE} CONTENT "#define GPU_IMPLEMENTATION\n#include \"gpu.h\"\n" @ONLY)

add_library(gpu STATIC ${GPU_IMPL_SOURCE} ${GPU_HEADER} ${GPU_MODULE_SOURCES})
add_library(GPU::gpu ALIAS gpu)
set_source_files_properties(${GPU_MODULE_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)
source_group("Modules" FILES ${GPU_MODULE_SOURCES})
target_include_directories(gpu PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(gpu PUBLIC Vulkan::Headers PRIVATE ${CMAKE_DL_LIBS})
# GPU_DEBUG enables asserts, validation layers, and debug-only checks. gpu.h also defaults it on when NDEBUG is absent.
target_compile_definitions(gpu PUBLIC $<$<CONFIG:Debug>:GPU_DEBUG=1>)

set(GPU_CLANG_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wno-missing-field-initializers)
function(GPU_set_warnings target)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
        # clang-cl maps a bare -Wall to -Weverything; /clang: forwards the flags to the clang driver unchanged.
        # CMake emits no C99 standard flag for MSVC-like front ends, so clang-cl gets it explicitly.
        list(TRANSFORM GPU_CLANG_WARNINGS PREPEND /clang: OUTPUT_VARIABLE warnings)
        target_compile_options(${target} PRIVATE /clang:-std=c99 ${warnings})
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE ${GPU_CLANG_WARNINGS})
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()
GPU_set_warnings(gpu)

# Compiles one Slang entry point to validated SPIR-V. Usage: GPU_compile_slang(<output.spv> <source.slang> <entry> <stage> [DEPENDS ...] [DEFINES ...] [OPTIONS ...])
find_program(GPU_SLANGC NAMES slangc REQUIRED)
find_program(GPU_SPIRV_VAL NAMES spirv-val REQUIRED)
set(GPU_SHADER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../include)
function(GPU_compile_slang output source entry stage)
    cmake_parse_arguments(SLANG "" "" "DEPENDS;DEFINES;OPTIONS" ${ARGN})
    get_filename_component(output_dir "${output}" DIRECTORY)
    get_filename_component(source_dir "${source}" DIRECTORY)
    set(options
        -target spirv -profile spirv_1_5 -emit-spirv-directly -fvk-use-entrypoint-name
        -entry ${entry} -stage ${stage}
        -fvk-use-c-layout -matrix-layout-row-major
        -capability spvDescriptorHeapEXT
        -I ${source_dir} -I ${CMAKE_CURRENT_SOURCE_DIR} -I ${GPU_SHADER_INCLUDE_DIR})
    if(stage STREQUAL "mesh" OR stage STREQUAL "amplification")
        list(APPEND options -capability spvMeshShadingEXT)
    endif()
    foreach(define IN LISTS SLANG_DEFINES)
        list(APPEND options -D${define})
    endforeach()
    list(APPEND options ${SLANG_OPTIONS})
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
        COMMAND ${GPU_SLANGC} ${source} ${options} -o ${output}
        COMMAND ${GPU_SPIRV_VAL} --target-env vulkan1.4 --scalar-block-layout ${output}
        DEPENDS ${source} ${SLANG_DEPENDS} ${GPU_SHADER_INCLUDE_DIR}/NoGraphicsAPI/shader.slang
        VERBATIM
        COMMENT "Compiling and validating Slang ${stage} shader ${entry} to SPIR-V"
    )
endfunction()

if(GPU_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(GPU_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND GPU_BUILD_EXAMPLES)
    cmake_path(GET Vulkan_INCLUDE_DIR PARENT_PATH vulkan_sdk_dir)
    get_target_property(example_binary_dir example_triangle BINARY_DIR)
    set_target_properties(example_triangle PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${example_binary_dir}"
        VS_DEBUGGER_ENVIRONMENT "VK_LAYER_PATH=${vulkan_sdk_dir}/Bin\nVK_LOADER_LAYERS_DISABLE=~implicit~")
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT example_triangle)
endif()
