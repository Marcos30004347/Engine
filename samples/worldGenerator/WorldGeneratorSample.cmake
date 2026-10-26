cmake_minimum_required(VERSION 3.10)

project(WorldGeneratorSample)
get_filename_component(SAMPLE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
include(${CMAKE_SOURCE_DIR}/cmake/WGSL2SpirV.cmake)

set(WORLD_GENERATOR_MAP_SHADER
    "${CMAKE_BINARY_DIR}/samples/assets/worldGenerator/world-generator-map.spirv")
wgsl2spirv(
    NAME world_generator_map_shader
    SRC ${SAMPLE_DIR}/WorldGeneratorMap.wgsl
    DST ${WORLD_GENERATOR_MAP_SHADER}
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/procedural/wgsl/
)

add_executable(
    WorldGeneratorSample
    ${SAMPLE_DIR}/WorldGeneratorSample.cpp
    ${SAMPLE_DIR}/WorldGeneratorViews.cpp
)
add_dependencies(WorldGeneratorSample world_generator_map_shader)
target_link_libraries(WorldGeneratorSample PRIVATE Engine)
target_compile_definitions(
    WorldGeneratorSample
    PRIVATE WORLD_GENERATOR_MAP_SHADER_PATH="${WORLD_GENERATOR_MAP_SHADER}"
)
target_sources(
    WorldGeneratorSample
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_widgets.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_sdl3.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(
    WorldGeneratorSample
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends
        ${CMAKE_SOURCE_DIR}/thirdparty/miniz
)
if(HAS_VULKAN)
    target_include_directories(WorldGeneratorSample PRIVATE ${Vulkan_INCLUDE_DIR})
endif()
