cmake_minimum_required(VERSION 3.10)

project (VirtualGeometryCullingMultipleDispatchesPassTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

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
    NAME wgsl_virtualgeometry_depth_reduce_to_pot
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/hzb/wgsl/depth_reduce_to_pot.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/hzb-depth-reduce-pot.spirv
    DEFINES USE_REVERSE_Z OCCLUSION_CULLING FRUSTUM_CULLING
)

set(DEPTH_PYRAMID_SPD_VARIANTS "")
set(DEPTH_PYRAMID_SOURCE_DEPTH_SPD_VARIANTS "")
foreach(MAX_MIPS RANGE 4 8)
    set(VARIANT_TARGET wgsl_virtualgeometry_depth_pyramid_spd_mips${MAX_MIPS})
    set(SOURCE_DEPTH_VARIANT_TARGET wgsl_virtualgeometry_depth_pyramid_spd_source_depth_mips${MAX_MIPS})
    list(APPEND DEPTH_PYRAMID_SPD_VARIANTS ${VARIANT_TARGET})
    list(APPEND DEPTH_PYRAMID_SOURCE_DEPTH_SPD_VARIANTS ${SOURCE_DEPTH_VARIANT_TARGET})
    wgsl2spirv(
        NAME ${VARIANT_TARGET}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/hzb/wgsl/depth_pyramid_spd.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/hzb-depth-pyramid-mips${MAX_MIPS}.spirv
        DEFINES USE_REVERSE_Z OCCLUSION_CULLING FRUSTUM_CULLING MAX_MIP_LEVELS_PER_PASS=${MAX_MIPS}
    )
    wgsl2spirv(
        NAME ${SOURCE_DEPTH_VARIANT_TARGET}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/hzb/wgsl/depth_pyramid_spd_source_depth.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/hzb-depth-pyramid-source-depth-mips${MAX_MIPS}.spirv
        DEFINES USE_REVERSE_Z OCCLUSION_CULLING FRUSTUM_CULLING MAX_MIP_LEVELS_PER_PASS=${MAX_MIPS}
    )
endforeach()

add_custom_target(wgsl_virtualgeometry_depth_pyramid_spd)
add_dependencies(wgsl_virtualgeometry_depth_pyramid_spd ${DEPTH_PYRAMID_SPD_VARIANTS})

add_custom_target(wgsl_virtualgeometry_depth_pyramid_spd_source_depth)
add_dependencies(wgsl_virtualgeometry_depth_pyramid_spd_source_depth ${DEPTH_PYRAMID_SOURCE_DEPTH_SPD_VARIANTS})

wgsl2spirv(
    NAME wgsl_virtualgeometry_cull_prepass_multipass
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/culling_multipass.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-culling-multipass.spirv
    DEFINES USE_REVERSE_Z OCCLUSION_CULLING FRUSTUM_CULLING ${VG_OCCLUSION_SHADER_DEFINES}
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_cull_prepass_multipass_draw_indirect_count_disabled
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/culling_multipass.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-culling-multipass-draw_indirect_count_disabled.spirv
    DEFINES USE_REVERSE_Z OCCLUSION_CULLING FRUSTUM_CULLING DRAW_INDIRECT_COUNT_DISABLED ${VG_OCCLUSION_SHADER_DEFINES}
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
)

wgsl2spirv(
    NAME wgsl_virtualgeometry_streaming_page_selection_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/streaming-page-selection-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/streaming-page-selection-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
)

add_executable(VirtualGeometryCullingMultipleDispatchesPassTests ${TEST_DIR}/VirtualGeometryCullingMultipleDispatchesPassTests.cpp)
target_link_libraries(VirtualGeometryCullingMultipleDispatchesPassTests PRIVATE Engine)
target_compile_definitions(VirtualGeometryCullingMultipleDispatchesPassTests PRIVATE)
add_test(NAME VirtualGeometryCullingMultipleDispatchesPassTests COMMAND VirtualGeometryCullingMultipleDispatchesPassTests)

add_dependencies(VirtualGeometryCullingMultipleDispatchesPassTests copy_mesh_assets)

add_dependencies(VirtualGeometryCullingMultipleDispatchesPassTests wgsl_virtualgeometry_depth_reduce_to_pot)
add_dependencies(VirtualGeometryCullingMultipleDispatchesPassTests wgsl_virtualgeometry_depth_pyramid_spd)
add_dependencies(VirtualGeometryCullingMultipleDispatchesPassTests wgsl_virtualgeometry_depth_pyramid_spd_source_depth)
add_dependencies(VirtualGeometryCullingMultipleDispatchesPassTests wgsl_virtualgeometry_cull_prepass_multipass)
add_dependencies(VirtualGeometryCullingMultipleDispatchesPassTests wgsl_virtualgeometry_cull_prepass_multipass_draw_indirect_count_disabled)
add_dependencies(VirtualGeometryCullingMultipleDispatchesPassTests wgsl_virtualgeometry_streaming_page_selection_cs)
