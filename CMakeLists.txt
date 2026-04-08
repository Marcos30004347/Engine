cmake_minimum_required(VERSION 3.14)

project(Engine)

include(cmake/WGSL2SpirV.cmake)

option(ENGINE_ENABLE_ASAN "Enable AddressSanitizer when supported by the active toolchain" OFF)
option(ENGINE_COLLECT_FRAME_STATISTICS "Enable virtual geometry and VSM frame statistics heatmaps" OFF)
set(ENGINE_VG_OCCLUSION_HIZ_MIP_BIAS "1" CACHE STRING "Bias virtual-geometry occlusion to a finer Hi-Z mip level")
set(ENGINE_VG_OCCLUSION_SCAN_MAX_DIM "4" CACHE STRING "Scan the full Hi-Z rectangle when its mip footprint is at most this many texels per axis")
set(ENGINE_VG_OCCLUSION_USE_9_TAP "1" CACHE STRING "Enable 9-tap Hi-Z sampling for large occlusion footprints (0 or 1)")
set(ENGINE_VG_OCCLUSION_USE_SPHERE_BOUNDS "1" CACHE STRING "Use sphere bounds instead of world-space AABBs for virtual-geometry occlusion (0 or 1)")
option(ENGINE_VG_ENABLE_PROJECTED_SIZE_CULLING "Cull virtual geometry and VSM work with tiny projected footprints" ON)
set(ENGINE_VG_CULL_PROJECT_MIN_WIDTH "5" CACHE STRING "Minimum projected footprint width in pixels/texels before culling")
set(ENGINE_VG_CULL_PROJECT_MIN_HEIGHT "5" CACHE STRING "Minimum projected footprint height in pixels/texels before culling")

get_property(ENGINE_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT ENGINE_IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Set default build type to Debug" FORCE)
endif()

if(ENGINE_ENABLE_ASAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        string(APPEND CMAKE_CXX_FLAGS " -fsanitize=address -fno-optimize-sibling-calls -fno-omit-frame-pointer -g -O0")
        string(APPEND CMAKE_C_FLAGS " -fsanitize=address -fno-optimize-sibling-calls -fno-omit-frame-pointer -g -O0")
    elseif(MSVC)
        string(APPEND CMAKE_CXX_FLAGS " /fsanitize=address /Zi")
        string(APPEND CMAKE_C_FLAGS " /fsanitize=address /Zi")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG -flto -fvisibility=hidden")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG -flto -fvisibility=hidden")
elseif(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Ox /DNDEBUG /GL")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Ox /DNDEBUG /GL")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(ENGINE_COLLECT_FRAME_STATISTICS)
    add_compile_definitions(COLLECT_FRAME_STATISTICS)
endif()

add_subdirectory(thirdparty/fcontext)
add_subdirectory(thirdparty/SDL)
add_subdirectory(thirdparty/lz4)
add_subdirectory(thirdparty/miniz)

add_library(simdpp INTERFACE)
target_include_directories(simdpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libsimdpp)

find_package(Vulkan QUIET)

if(Vulkan_FOUND)
    message(STATUS "Vulkan found: ${Vulkan_LIBRARY}")
    set(HAS_VULKAN TRUE)
else()
    message(WARNING "Vulkan SDK not found. Vulkan backend will not be built.")
    set(HAS_VULKAN FALSE)
endif()

function(engine_apply_common_includes target_name)
    target_include_directories(${target_name}
        PUBLIC
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/runtime
        PRIVATE
            ${CMAKE_SOURCE_DIR}/thirdparty/fcontext/include
            ${CMAKE_SOURCE_DIR}/thirdparty
            ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf
            ${CMAKE_SOURCE_DIR}/thirdparty/o1heap
    )
endfunction()

function(engine_apply_runtime_target_defaults target_name)
    engine_apply_common_includes(${target_name})

    target_compile_definitions(${target_name} PRIVATE ENABLE_OVERRIDE=0)
    target_compile_definitions(${target_name} PRIVATE SDL3_AVAILABLE)

    target_link_libraries(${target_name}
        PUBLIC fcontext
        PUBLIC SDL3::SDL3
        PRIVATE simdpp
        PRIVATE lz4
        PRIVATE miniz
    )

    if(HAS_VULKAN)
        target_include_directories(${target_name} PRIVATE ${Vulkan_INCLUDE_DIR})
        target_link_libraries(${target_name} PRIVATE Vulkan::Vulkan)
    endif()

    if(APPLE)
        set(VULKAN_SDK_PATH $ENV{VULKAN_SDK})

        if(VULKAN_SDK_PATH)
            message(STATUS "Vulkan SDK found at: ${VULKAN_SDK_PATH}")
        else()
            message(WARNING "VULKAN_SDK environment variable is not set!")
        endif()

        set(MOLTENVK_LIB "${VULKAN_SDK_PATH}/lib/libMoltenVK.dylib")
        target_link_libraries(${target_name} PRIVATE ${MOLTENVK_LIB})
    endif()

    if(ENABLE_PROFILING)
        find_library(PROFILER_LIB profiler)
        if(PROFILER_LIB)
            message(STATUS "gperftools profiler found: ${PROFILER_LIB}")
            target_link_libraries(${target_name} PRIVATE ${PROFILER_LIB})
            target_compile_definitions(${target_name} PRIVATE ENABLE_PROFILING)
        else()
            message(WARNING "gperftools profiler not found, disabling profiling support")
        endif()
    endif()
endfunction()

function(engine_copy_sdl_runtime target_name)
    if(WIN32 AND TARGET SDL3-shared)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3-shared>"
                "$<TARGET_FILE_DIR:${target_name}>"
        )
    endif()
endfunction()

add_subdirectory(src/runtime)
add_subdirectory(src/editor)
add_subdirectory(src/cli)

add_library(Engine INTERFACE)
target_link_libraries(Engine INTERFACE cli)

enable_testing()
add_subdirectory(tests)

add_custom_target(copy_mesh_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets/meshes
            ${CMAKE_BINARY_DIR}/tests/assets/meshes
)
