cmake_minimum_required(VERSION 3.10)

project(PolygonTerrainSample)
get_filename_component(SAMPLE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
include(${CMAKE_SOURCE_DIR}/cmake/WGSL2SpirV.cmake)

set(POLYGON_TERRAIN_SPHERE_SHADER "${CMAKE_BINARY_DIR}/samples/assets/polygonTerrain/polygon-terrain-sphere.spirv")
set(POLYGON_TERRAIN_EROSION_SHADER "${CMAKE_BINARY_DIR}/samples/assets/polygonTerrain/polygon-terrain-hydraulic-erosion.spirv")
set(POLYGON_TERRAIN_RASTER_SHADER "${CMAKE_BINARY_DIR}/samples/assets/polygonTerrain/polygon-terrain-preview-raster.spirv")
wgsl2spirv(
    NAME polygon_terrain_sphere_shader
    SRC ${SAMPLE_DIR}/PolygonTerrainSphere.wgsl
    DST ${POLYGON_TERRAIN_SPHERE_SHADER}
)
wgsl2spirv(
    NAME polygon_terrain_erosion_shader
    SRC ${SAMPLE_DIR}/PolygonTerrainHydraulicErosion.wgsl
    DST ${POLYGON_TERRAIN_EROSION_SHADER}
)
wgsl2spirv(
    NAME polygon_terrain_raster_shader
    SRC ${SAMPLE_DIR}/PolygonTerrainPreviewRaster.wgsl
    DST ${POLYGON_TERRAIN_RASTER_SHADER}
)

# The adaptive globe reuses the svraster planet passes, and those resolve their
# SPIR-V with os::io::readRelativeFile - relative to the *executable*. The
# runtime library compiles them into tests/assets, which is where the renderer
# sample finds them; this binary lives in samples/, so without this the CBT
# passes fail to open their shaders at pipeline creation and the globe silently
# stays on the fixed mesh. Link the whole shader tree in rather than copying
# four named files, so a new pass does not reintroduce the same failure.
set(POLYGON_TERRAIN_SHARED_SHADER_LINK "${CMAKE_BINARY_DIR}/samples/assets/shaders")
add_custom_target(
    polygon_terrain_shared_shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/samples/assets"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${POLYGON_TERRAIN_SHARED_SHADER_LINK}"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${CMAKE_BINARY_DIR}/tests/assets/shaders"
        "${POLYGON_TERRAIN_SHARED_SHADER_LINK}"
    COMMENT "Linking shared svraster SPIR-V into the polygon terrain sample assets"
    VERBATIM
)

add_executable(
    PolygonTerrainSample
    ${SAMPLE_DIR}/PolygonTerrainSample.cpp
    ${SAMPLE_DIR}/PolygonTerrainPreviewPasses.cpp
    ${SAMPLE_DIR}/PolygonTerrainAdaptiveGlobe.cpp
)
add_dependencies(PolygonTerrainSample polygon_terrain_sphere_shader polygon_terrain_erosion_shader polygon_terrain_raster_shader polygon_terrain_shared_shaders)
target_link_libraries(PolygonTerrainSample PRIVATE Engine)
target_compile_definitions(
    PolygonTerrainSample
    PRIVATE
        POLYGON_TERRAIN_SPHERE_SHADER_PATH="${POLYGON_TERRAIN_SPHERE_SHADER}"
        POLYGON_TERRAIN_EROSION_SHADER_PATH="${POLYGON_TERRAIN_EROSION_SHADER}"
        POLYGON_TERRAIN_RASTER_SHADER_PATH="${POLYGON_TERRAIN_RASTER_SHADER}"
        PLANET_STREAM_POWER_SHADER_PATH="${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/procedural/planet-stream-power.spirv"
)
target_sources(
    PolygonTerrainSample
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_widgets.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_sdl3.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(
    PolygonTerrainSample
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends
)
if(HAS_VULKAN)
    target_include_directories(PolygonTerrainSample PRIVATE ${Vulkan_INCLUDE_DIR})
endif()
