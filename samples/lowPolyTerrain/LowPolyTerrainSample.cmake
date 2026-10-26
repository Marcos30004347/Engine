cmake_minimum_required(VERSION 3.10)

project(LowPolyTerrainSample)
get_filename_component(SAMPLE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
include(${CMAKE_SOURCE_DIR}/cmake/WGSL2SpirV.cmake)

set(LOW_POLY_TERRAIN_SHADER
    "${CMAKE_BINARY_DIR}/samples/assets/lowPolyTerrain/low-poly-terrain.spirv")
wgsl2spirv(
    NAME low_poly_terrain_shader
    SRC ${SAMPLE_DIR}/LowPolyTerrain.wgsl
    DST ${LOW_POLY_TERRAIN_SHADER}
)

add_executable(
    LowPolyTerrainSample
    ${SAMPLE_DIR}/LowPolyTerrainSample.cpp
    ${SAMPLE_DIR}/LowPolyTerrainView.cpp
)
add_dependencies(LowPolyTerrainSample low_poly_terrain_shader)
target_link_libraries(LowPolyTerrainSample PRIVATE Engine)
target_compile_definitions(
    LowPolyTerrainSample
    PRIVATE LOW_POLY_TERRAIN_SHADER_PATH="${LOW_POLY_TERRAIN_SHADER}"
        LOW_POLY_TERRAIN_DEFAULT_MAP_PATH="${CMAKE_SOURCE_DIR}/world-data")
target_sources(
    LowPolyTerrainSample
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_widgets.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_sdl3.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(
    LowPolyTerrainSample
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends
)
if(HAS_VULKAN)
    target_include_directories(LowPolyTerrainSample PRIVATE ${Vulkan_INCLUDE_DIR})
endif()

target_include_directories(LowPolyTerrainSample PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
