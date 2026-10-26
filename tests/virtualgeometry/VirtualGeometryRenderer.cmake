cmake_minimum_required(VERSION 3.10)

project (VirtualGeometryRenderer)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

set(VG_FRAME_STAT_SHADER_DEFINES)
if(ENGINE_COLLECT_FRAME_STATISTICS)
    list(APPEND VG_FRAME_STAT_SHADER_DEFINES COLLECT_FRAME_STATISTICS)
endif()

set(VG_OCCLUSION_SHADER_DEFINES
    OCCLUSION_HIZ_MIP_BIAS=${ENGINE_VG_OCCLUSION_HIZ_MIP_BIAS}
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
    NAME wgsl_renderToQuad_vsm_pages_overlay_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderToQuadVsmPagesOverlay-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderToQuadVsmPagesOverlay-fs.spirv
    DEFINES USE_REVERSE_Z
)

wgsl2spirv(
    NAME wgsl_frame_statistics_heatmap_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/frameStatisticsHeatmap-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/frameStatisticsHeatmap-cs.spirv
)

wgsl2spirv(
    NAME wgsl_svgf_denoise_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/svgf-denoise-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svgf-denoise-cs.spirv
)

wgsl2spirv(
    NAME wgsl_svgf_atrous_filter_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/svgf-atrous-filter-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svgf-atrous-filter-cs.spirv
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_taa_resolve_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-taa-resolve-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-taa-resolve-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_taa_history_update_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-taa-history-update-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-taa-history-update-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_screen_space_raytrace_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/screen-space-raytrace-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/screen-space-raytrace-cs.spirv
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_screen_space_raytrace_software_fallback_prepare_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/screen-space-raytrace-software-fallback-prepare-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/screen-space-raytrace-software-fallback-prepare-cs.spirv
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_screen_space_raytrace_sdf_fallback_prepare_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/screen-space-raytrace-sdf-fallback-prepare-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/screen-space-raytrace-sdf-fallback-prepare-cs.spirv
)
wgsl2spirv(
    NAME wgsl_renderToQuad_vertex_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderToQuadPass-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderToQuadPass-vs.spirv
)

wgsl2spirv(
    NAME wgsl_renderToQuad_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderToQuadPass-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderToQuadPass-fs.spirv
)

wgsl2spirv(
    NAME wgsl_renderToQuadColor_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/renderColorToQuadPass-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/renderColorToQuadPass-fs.spirv
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_vertex_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-meshlet-vs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
    DEFINES USE_REVERSE_Z
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_vertex_shader_draw_indirect_count_disabled
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-meshlet-vs-draw_indirect_count_disabled.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
    DEFINES USE_REVERSE_Z DRAW_INDIRECT_COUNT_DISABLED=1
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_fragment_shader
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-meshlet-fs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
    DEFINES ${VG_FRAME_STAT_SHADER_DEFINES}
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_vertex_shader_motion
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-meshlet-vs-motion.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
    DEFINES USE_REVERSE_Z OUTPUT_MOTION_VECTORS=1
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_vertex_shader_motion_draw_indirect_count_disabled
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-meshlet-vs-motion-draw_indirect_count_disabled.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
    DEFINES USE_REVERSE_Z OUTPUT_MOTION_VECTORS=1 DRAW_INDIRECT_COUNT_DISABLED=1
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_fragment_shader_motion
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-fs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-meshlet-fs-motion.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
    DEFINES OUTPUT_MOTION_VECTORS=1 ${VG_FRAME_STAT_SHADER_DEFINES}
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_software_raster_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-software-raster-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-software-raster-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/
    DEFINES USE_REVERSE_Z
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_visibility_merge_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-visibility-merge-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-visibility-merge-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/
    DEFINES USE_REVERSE_Z
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_visibility_resolve
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-visibility-resolve.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-visibility-resolve.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/
    DEFINES USE_REVERSE_Z OUTPUT_MOTION_VECTORS=1
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_gbuffer_readback_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-gbuffer-readback-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-gbuffer-readback-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/
)

if(NOT TARGET wgsl_virtualgeometry_page_decode_cs)
    wgsl2spirv(
        NAME wgsl_virtualgeometry_page_decode_cs
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-page-decode-cs.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-page-decode-cs.spirv
        INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
    )
endif()

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
    NAME wgsl_virtualshadowmap_culling_draw_indirect_count_disabled_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-culling-multipass.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-culling-multipass-draw_indirect_count_disabled.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
    DEFINES DRAW_INDIRECT_COUNT_DISABLED=1
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_culling_per_page_draw_indirect_count_disabled_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-culling-multipass.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-culling-multipass-per-page-draw_indirect_count_disabled.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
    DEFINES VSM_PER_PAGE_DRAW_COMMANDS=1 DRAW_INDIRECT_COUNT_DISABLED=1
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_meshlet_draw_indirect_count_disabled_vs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-meshlet-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-meshlet-vs-draw_indirect_count_disabled.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
    DEFINES DRAW_INDIRECT_COUNT_DISABLED=1
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_meshlet_per_page_draw_indirect_count_disabled_vs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-meshlet-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-meshlet-vs-per-page-draw_indirect_count_disabled.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
    DEFINES VSM_CLIP_SPACE_ATLAS_DEPTH=1 VSM_ENABLE_CLIP_DISTANCES=1 DRAW_INDIRECT_COUNT_DISABLED=1
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_scratch_stencil_vs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-scratch-stencil-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-scratch-stencil-vs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_scratch_stencil_per_page_atlas_depth_vs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-scratch-stencil-vs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-scratch-stencil-vs-per-page-atlas-depth.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
    DEFINES VSM_CLIP_SPACE_ATLAS_DEPTH=1
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
    NAME wgsl_virtualshadowmap_layer_resolve_per_page_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-layer-resolve.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-layer-resolve-per-page.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/
    DEFINES VSM_CLIP_SPACE_ATLAS_DEPTH=1
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
    DEFINES WAVE_SIZE=64 SAMPLE_COUNT=60 HARD_SHADOW_SAMPLES=4 FADE_OUT_SAMPLES=8
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_ssss_penumbra_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-ssss-penumbra.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-ssss-penumbra.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/
    DEFINES VSM_SSSS_KERNEL_SIZE=11
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_ssss_blur_horizontal_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-ssss-blur-horizontal.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-ssss-blur-horizontal.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/
    DEFINES VSM_SSSS_KERNEL_SIZE=11
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_ssss_blur_vertical_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-ssss-blur-vertical.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-ssss-blur-vertical.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/
    DEFINES VSM_SSSS_KERNEL_SIZE=11
)

wgsl2spirv(
    NAME wgsl_virtualshadowmap_ssss_debug_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/vsm-ssss-debug.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/vsm-ssss-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualshadowmap/wgsl/
    DEFINES VSM_SSSS_KERNEL_SIZE=11
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
# Not a ctest: the virtual-geometry renderer is an interactive windowed
# sample - its frame loop runs until the window is closed - so ctest would
# hang on it forever. The target stays buildable and is run by hand.

add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_depth_reduce_to_pot)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_cull_prepass_multipass)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_cull_prepass_multipass_draw_indirect_count_disabled)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_streaming_page_selection_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_page_decode_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_depth_pyramid_spd)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_depth_pyramid_spd_source_depth)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_vertex_shader)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_vertex_shader_draw_indirect_count_disabled)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_fragment_shader)
if(NOT TARGET wgsl_material_pass_compute_shader)
    wgsl2spirv(
        NAME wgsl_material_pass_compute_shader
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/material-pass-cs.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/material-pass-cs.spirv
    )
endif()

if(NOT TARGET wgsl_default_material_compute_shader)
    wgsl2spirv(
        NAME wgsl_default_material_compute_shader
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/material-default.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/material-default.spirv
        INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/ ${CMAKE_SOURCE_DIR}/assets/shaders/
    )
endif()

if(NOT TARGET wgsl_material_debug_basecolor_compute_shader)
    wgsl2spirv(
        NAME wgsl_material_debug_basecolor_compute_shader
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/material-debug-basecolor.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/material-debug-basecolor.spirv
        INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/ ${CMAKE_SOURCE_DIR}/assets/shaders/
    )
endif()

if(NOT TARGET wgsl_material_debug_normal_compute_shader)
    wgsl2spirv(
        NAME wgsl_material_debug_normal_compute_shader
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/material-debug-normal.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/material-debug-normal.spirv
        INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/ ${CMAKE_SOURCE_DIR}/assets/shaders/
    )
endif()

add_dependencies(VirtualGeometryRenderer wgsl_material_pass_compute_shader)
add_dependencies(VirtualGeometryRenderer wgsl_default_material_compute_shader)
add_dependencies(VirtualGeometryRenderer wgsl_material_debug_basecolor_compute_shader)
add_dependencies(VirtualGeometryRenderer wgsl_material_debug_normal_compute_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_vertex_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuadColor_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_shadow_debug_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_depth_debug_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_scalar_debug_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_selected_debug_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_renderToQuad_vsm_pages_overlay_fragment_shader)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_vertex_shader_motion)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_vertex_shader_motion_draw_indirect_count_disabled)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_fragment_shader_motion)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_taa_resolve_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_taa_history_update_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_screen_space_raytrace_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualgeometry_screen_space_raytrace_software_fallback_prepare_cs)
add_dependencies(VirtualGeometryRenderer wgsl_frame_statistics_heatmap_cs)
add_dependencies(VirtualGeometryRenderer wgsl_svgf_denoise_cs)
add_dependencies(VirtualGeometryRenderer wgsl_svgf_atrous_filter_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_bookkeeping_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_debug_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_draw_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_culling_draw_indirect_count_disabled_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_culling_per_page_draw_indirect_count_disabled_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_meshlet_draw_indirect_count_disabled_vs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_meshlet_per_page_draw_indirect_count_disabled_vs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_scratch_stencil_vs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_scratch_stencil_per_page_atlas_depth_vs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_finish_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_layer_resolve_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_layer_resolve_per_page_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_shadow_mask_pcf_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_screen_space_shadow_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_ssss_penumbra_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_ssss_blur_horizontal_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_ssss_blur_vertical_cs)
add_dependencies(VirtualGeometryRenderer wgsl_virtualshadowmap_ssss_debug_cs)
add_dependencies(Engine wgsl_virtualgeometry_streaming_page_selection_cs)
add_dependencies(Engine wgsl_virtualgeometry_page_decode_cs)
add_dependencies(Engine wgsl_virtualshadowmap_bookkeeping_cs)
add_dependencies(Engine wgsl_virtualshadowmap_debug_cs)
add_dependencies(Engine wgsl_virtualshadowmap_draw_cs)
add_dependencies(Engine wgsl_virtualshadowmap_culling_draw_indirect_count_disabled_cs)
add_dependencies(Engine wgsl_virtualshadowmap_culling_per_page_draw_indirect_count_disabled_cs)
add_dependencies(Engine wgsl_virtualshadowmap_meshlet_draw_indirect_count_disabled_vs)
add_dependencies(Engine wgsl_virtualshadowmap_meshlet_per_page_draw_indirect_count_disabled_vs)
add_dependencies(Engine wgsl_virtualshadowmap_scratch_stencil_vs)
add_dependencies(Engine wgsl_virtualshadowmap_scratch_stencil_per_page_atlas_depth_vs)
add_dependencies(Engine wgsl_virtualshadowmap_finish_cs)
add_dependencies(Engine wgsl_virtualshadowmap_layer_resolve_cs)
add_dependencies(Engine wgsl_virtualshadowmap_layer_resolve_per_page_cs)
add_dependencies(Engine wgsl_virtualshadowmap_shadow_mask_pcf_cs)
add_dependencies(Engine wgsl_virtualshadowmap_screen_space_shadow_cs)
add_dependencies(Engine wgsl_virtualshadowmap_ssss_penumbra_cs)
add_dependencies(Engine wgsl_virtualshadowmap_ssss_blur_horizontal_cs)
add_dependencies(Engine wgsl_virtualshadowmap_ssss_blur_vertical_cs)
add_dependencies(Engine wgsl_virtualshadowmap_ssss_debug_cs)
add_dependencies(Engine wgsl_material_pass_compute_shader)
add_dependencies(Engine wgsl_default_material_compute_shader)
add_dependencies(Engine wgsl_virtualgeometry_vertex_shader_motion)
add_dependencies(Engine wgsl_virtualgeometry_vertex_shader_motion_draw_indirect_count_disabled)
add_dependencies(Engine wgsl_virtualgeometry_fragment_shader_motion)
add_dependencies(Engine wgsl_virtualgeometry_software_raster_cs)
add_dependencies(Engine wgsl_virtualgeometry_visibility_merge_cs)
add_dependencies(Engine wgsl_virtualgeometry_visibility_resolve)
add_dependencies(Engine wgsl_virtualgeometry_gbuffer_readback_cs)
add_dependencies(Engine wgsl_virtualgeometry_taa_resolve_cs)
add_dependencies(Engine wgsl_virtualgeometry_taa_history_update_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_space_raytrace_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_space_raytrace_software_fallback_prepare_cs)
add_dependencies(Engine wgsl_svgf_denoise_cs)
add_dependencies(Engine wgsl_svgf_atrous_filter_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_probe_raygen_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_probe_screen_space_trace_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_probe_sdf_fallback_prepare_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_probe_trace_resolve_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_probe_trace_stats_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_probe_reservoir_accumulate_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_probe_reservoir_spatial_reuse_cs)
add_dependencies(Engine wgsl_virtualgeometry_spatial_radiance_cache_cs)
add_dependencies(Engine wgsl_virtualgeometry_spatial_radiance_cache_debug_cs)
add_dependencies(Engine wgsl_virtualgeometry_spatial_radiance_cache_lookup_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_probe_resolve_targets_cs)
add_dependencies(Engine wgsl_virtualgeometry_screen_probe_ao_cs)
