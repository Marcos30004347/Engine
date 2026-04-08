cmake_minimum_required(VERSION 3.10)

project (VirtualGeometryRenderer)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

set(VG_FRAME_STAT_SHADER_DEFINES)
if(ENGINE_COLLECT_FRAME_STATISTICS)
    list(APPEND VG_FRAME_STAT_SHADER_DEFINES COLLECT_FRAME_STATISTICS)
endif()

set(VG_OCCLUSION_SHADER_DEFINES
    OCCLUSION_HIZ_MIP_BIAS=${ENGINE_VG_OCCLUSION_HIZ_MIP_BIAS}
    OCCLUSION_SCAN_MAX_DIM=${ENGINE_VG_OCCLUSION_SCAN_MAX_DIM}
    OCCLUSION_USE_9_TAP=${ENGINE_VG_OCCLUSION_USE_9_TAP}
    OCCLUSION_USE_SPHERE_BOUNDS=${ENGINE_VG_OCCLUSION_USE_SPHERE_BOUNDS}
)

if(ENGINE_VG_ENABLE_PROJECTED_SIZE_CULLING)
    list(APPEND VG_OCCLUSION_SHADER_DEFINES
        PROJECTED_SIZE_CULLING
        CULL_PROJECT_MIN_WIDTH=${ENGINE_VG_CULL_PROJECT_MIN_WIDTH}
        CULL_PROJECT_MIN_HEIGHT=${ENGINE_VG_CULL_PROJECT_MIN_HEIGHT}
    )
endif()

wgsl2spirv(
    NAME wgsl_renderToQuad_shadow_debug_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderToQuadShadowDebug-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderToQuadShadowDebug-fs.spirv
    DEFINES USE_REVERSE_Z
)

wgsl2spirv(
    NAME wgsl_renderToQuad_depth_debug_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderToQuadDepthDebug-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderToQuadDepthDebug-fs.spirv
    DEFINES USE_REVERSE_Z
)

wgsl2spirv(
    NAME wgsl_renderToQuad_scalar_debug_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderToQuadScalarDebug-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderToQuadScalarDebug-fs.spirv
    DEFINES USE_REVERSE_Z
)

wgsl2spirv(
    NAME wgsl_renderToQuad_selected_debug_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderToQuadSelectedDebug-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderToQuadSelectedDebug-fs.spirv
    DEFINES USE_REVERSE_Z
)

wgsl2spirv(
    NAME wgsl_frame_statistics_heatmap_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/frameStatisticsHeatmap-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/frameStatisticsHeatmap-cs.spirv
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_bookkeeping_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-bookkeeping.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-bookkeeping.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_debug_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-debug.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_draw_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-draw.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-draw.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_culling_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-culling-multipass.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-culling-multipass.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_meshlet_vs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-meshlet-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-meshlet-vs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_scratch_stencil_vs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-scratch-stencil-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-scratch-stencil-vs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_finish_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-finish.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-finish.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_layer_resolve_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-layer-resolve.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-layer-resolve.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_shadow_mask_pcf_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-shadow-mask-pcf.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-shadow-mask-pcf.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_screen_space_shadow_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-screen-space-shadow.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-screen-space-shadow.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

add_executable(VirtualGeometryRenderer ${TEST_DIR}/VirtualGeometryRenderer.cpp)
target_link_libraries(VirtualGeometryRenderer PRIVATE Engine)
target_sources(
    VirtualGeometryRenderer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_widgets.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_sdl3.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(
    VirtualGeometryRenderer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends
)
add_test(NAME VirtualGeometryRenderer COMMAND VirtualGeometryRenderer)

add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_depth_reduce_to_pot)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_cull_prepass_multipass)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_cull_prepass_multipass_draw_indirect_count_disabled)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_streaming_page_selection_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_depth_pyramid_spd)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_depth_pyramid_spd_source_depth)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_vertex_shader)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_vertex_shader_draw_indirect_count_disabled)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_material_fragment_shader)
if(NOT TARGET wgsl_virtualgeometry_material_vertex_shader)
    wgsl2spirv(
        NAME wgsl_virtualgeometry_material_vertex_shader
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-vs.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-material-vs.spirv
    )
endif()

if(NOT TARGET wgsl_virtualgeometry_material_depth_fragment_shader)
    wgsl2spirv(
        NAME wgsl_virtualgeometry_material_depth_fragment_shader
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-depth-fs.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-material-depth-fs.spirv
    )
endif()

if(NOT TARGET wgsl_virtualgeometry_material_tiles_compute_shader)
    wgsl2spirv(
        NAME wgsl_virtualgeometry_material_tiles_compute_shader
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-tiles-cs.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-material-tiles-cs.spirv
    )
endif()

add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_material_vertex_shader)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_material_depth_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_material_tiles_compute_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_vertex_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuadColor_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_shadow_debug_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_depth_debug_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_scalar_debug_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_selected_debug_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_frame_statistics_heatmap_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_bookkeeping_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_debug_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_draw_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_culling_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_meshlet_vs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_scratch_stencil_vs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_finish_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_layer_resolve_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_shadow_mask_pcf_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_screen_space_shadow_cs)
add_dependencies(Engine wgsl_virtualgeometry_streaming_page_selection_cs)
add_dependencies(Engine wgsl_virtualshadowmap_bookkeeping_cs)
add_dependencies(Engine wgsl_virtualshadowmap_debug_cs)
add_dependencies(Engine wgsl_virtualshadowmap_draw_cs)
add_dependencies(Engine wgsl_virtualshadowmap_culling_cs)
add_dependencies(Engine wgsl_virtualshadowmap_meshlet_vs)
add_dependencies(Engine wgsl_virtualshadowmap_scratch_stencil_vs)
add_dependencies(Engine wgsl_virtualshadowmap_finish_cs)
add_dependencies(Engine wgsl_virtualshadowmap_layer_resolve_cs)
add_dependencies(Engine wgsl_virtualshadowmap_shadow_mask_pcf_cs)
add_dependencies(Engine wgsl_virtualshadowmap_screen_space_shadow_cs)
