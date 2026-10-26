include(${CMAKE_SOURCE_DIR}/cmake/WGSL2SpirV.cmake)

file(GLOB_RECURSE ENGINE_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/runtime/*.c"
    "${CMAKE_SOURCE_DIR}/src/runtime/*.cpp"
    "${CMAKE_SOURCE_DIR}/thirdparty/rpmalloc/*.c"
    "${CMAKE_SOURCE_DIR}/thirdparty/o1heap/o1heap/*.c"
)

add_library(runtime ${ENGINE_RUNTIME_SOURCES})
engine_apply_runtime_target_defaults(runtime)

set(GPUSDF_BUILD_ENTRIES
    ClearCountersCS
    CollectClearBricksCS
    PrepareClearBricksCS
    ClearBrickEntryCS
    ClearBuildCountersCS
    ResetCascadeCS
    InitializeCascadeCS
    MarkCascadeUninitializedCS
    FreeCascadeCS
    ScrollCascadeCS
    ClearRefCountersCS
    ClearJobCounterCS
    InvalidateJobAreasCS
    CoarseCullingCS
    ScanJobsCS
    FinalizeJobScanCS
    VoxelizeCS
    ScanReferencesCS
    FinalizeReferenceScanCS
    CompactReferencesCS
    ClearBrickStorageCS
    EmitSdfCS
    CompressBricksCS
)

set(GPUSDF_COMBINE_ENTRIES
    CollectDirtyBricksCS
    PrepareEikonalArgsCS
    EikonalCS
    MergeCascadesCS
    PrepareMergeBricksArgsCS
    MergeBricksCS
    BuildTreeAabbCS
)

set(GPUSDF_DEBUG_SHADERS
    trace
    atlas
)

add_custom_target(GPUSignedDistanceFieldsShaders)
foreach(GPUSDF_ENTRY IN LISTS GPUSDF_BUILD_ENTRIES)
  wgsl2spirv(
      NAME gpusdf_build_${GPUSDF_ENTRY}
      SRC ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-build.wgsl
      DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/gpusigneddistancefields-build-${GPUSDF_ENTRY}.spirv
      INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/
  )
  add_dependencies(GPUSignedDistanceFieldsShaders gpusdf_build_${GPUSDF_ENTRY})
endforeach()

foreach(GPUSDF_ENTRY IN LISTS GPUSDF_COMBINE_ENTRIES)
  wgsl2spirv(
      NAME gpusdf_combine_${GPUSDF_ENTRY}
      SRC ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-combine.wgsl
      DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/gpusigneddistancefields-combine-${GPUSDF_ENTRY}.spirv
      INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/
  )
  add_dependencies(GPUSignedDistanceFieldsShaders gpusdf_combine_${GPUSDF_ENTRY})
endforeach()

wgsl2spirv(
    NAME gpusdf_trace
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/gpusigneddistancefields-trace.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/
)
wgsl2spirv(
    NAME gpusdf_debug_trace
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-debug-trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/gpusigneddistancefields-debug-trace.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/
)
wgsl2spirv(
    NAME gpusdf_debug_atlas
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-debug-atlas.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/gpusigneddistancefields-debug-atlas.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/
)
wgsl2spirv(
    NAME gpusdf_fallback_debug_prepare
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-fallback-debug-prepare.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/gpusigneddistancefields-fallback-debug-prepare.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/
)
wgsl2spirv(
    NAME gpusdf_fallback_debug_vertex
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-fallback-debug-vertex.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/gpusigneddistancefields-fallback-debug-vertex.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/
)
wgsl2spirv(
    NAME gpusdf_fallback_debug_fragment
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/gpusigneddistancefields-fallback-debug-fragment.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/gpusigneddistancefields-fallback-debug-fragment.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/gpusigneddistancefields/wgsl/
)
add_dependencies(GPUSignedDistanceFieldsShaders gpusdf_debug_trace)
add_dependencies(GPUSignedDistanceFieldsShaders gpusdf_trace)
add_dependencies(GPUSignedDistanceFieldsShaders gpusdf_debug_atlas)
add_dependencies(GPUSignedDistanceFieldsShaders gpusdf_fallback_debug_prepare)
add_dependencies(GPUSignedDistanceFieldsShaders gpusdf_fallback_debug_vertex)
add_dependencies(GPUSignedDistanceFieldsShaders gpusdf_fallback_debug_fragment)

add_custom_target(SDFShaders)
wgsl2spirv(
    NAME sdf_trace
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/sdf/wgsl/sdf-trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/sdf-trace.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/sdf/wgsl/
)
wgsl2spirv(
    NAME sdf_trace_debug
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/sdf/wgsl/sdf-trace-debug.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/sdf-trace-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/sdf/wgsl/
)
wgsl2spirv(
    NAME sdf_scene_grid_build
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/sdf/wgsl/sdf-scene-grid-build.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/sdf-scene-grid-build.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/sdf/wgsl/
)
add_dependencies(SDFShaders sdf_trace)
add_dependencies(SDFShaders sdf_trace_debug)
add_dependencies(SDFShaders sdf_scene_grid_build)

set(SOFTWARE_BVH_SHADER_SOURCES
    cwbvh-triangle-aabbs
    cwbvh-morton
    cwbvh-build
    hlbvh-build
)

add_custom_target(SoftwareBVHShaders)

wgsl2spirv(
    NAME softwarebvh_triangle_aabbs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/cwbvh/wgsl/triangleAabbs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/cwbvh-triangle-aabbs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/cwbvh/wgsl/
)
wgsl2spirv(
    NAME softwarebvh_morton
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/cwbvh/wgsl/mortonCodes.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/cwbvh-morton.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/cwbvh/wgsl/
)
wgsl2spirv(
    NAME softwarebvh_cwbvh
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/cwbvh/wgsl/cwbvh.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/cwbvh-build.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/cwbvh/wgsl/
)
wgsl2spirv(
    NAME softwarebvh_hlbvh
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/hlbvh/wgsl/hlbvh.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/hlbvh-build.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/hlbvh/wgsl/
)
wgsl2spirv(
    NAME softwarebvh_raytrace
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
)
wgsl2spirv(
    NAME softwarebvh_raytrace_triangle_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-triangle-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_DISABLE_OVERLAY
        SWRT_DISABLE_HEATMAP
)
wgsl2spirv(
    NAME softwarebvh_raytrace_triangle_debug
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-triangle-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_ENABLE_TRACE_STATS
)
wgsl2spirv(
    NAME softwarebvh_raytrace_triangle_bvh4_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-triangle-bvh4-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_TLAS_BVH4
        SWRT_DISABLE_OVERLAY
        SWRT_DISABLE_HEATMAP
)
wgsl2spirv(
    NAME softwarebvh_raytrace_triangle_bvh4_debug
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-triangle-bvh4-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_TLAS_BVH4
        SWRT_ENABLE_TRACE_STATS
)
wgsl2spirv(
    NAME softwarebvh_raytrace_world_cwbvh_triangle_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-world-cwbvh-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-triangle-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_WORLD_CWBVH
        SWRT_DISABLE_OVERLAY
        SWRT_DISABLE_HEATMAP
)
wgsl2spirv(
    NAME softwarebvh_raytrace_world_cwbvh_triangle_debug
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-world-cwbvh-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-triangle-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_WORLD_CWBVH
        SWRT_ENABLE_TRACE_STATS
)
wgsl2spirv(
    NAME softwarebvh_raytrace_world_cwbvh_subgroup_triangle_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-world-cwbvh-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-subgroup-triangle-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_WORLD_CWBVH
        SWRT_WORLD_CWBVH_SUBGROUP
        SWRT_DISABLE_OVERLAY
        SWRT_DISABLE_HEATMAP
)
wgsl2spirv(
    NAME softwarebvh_raytrace_world_cwbvh_subgroup_triangle_debug
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-world-cwbvh-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-subgroup-triangle-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_WORLD_CWBVH
        SWRT_WORLD_CWBVH_SUBGROUP
        SWRT_ENABLE_TRACE_STATS
)
wgsl2spirv(
    NAME softwarebvh_raytrace_rbvh_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-rbvh-card.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-rbvh-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_DISABLE_OVERLAY
        SWRT_DISABLE_HEATMAP
)
wgsl2spirv(
    NAME softwarebvh_raytrace_rbvh_debug
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-rbvh-card.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-rbvh-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_ENABLE_TRACE_STATS
)
wgsl2spirv(
    NAME softwarebvh_raytrace_rbvh_bvh4_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-rbvh-card.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-rbvh-bvh4-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_TLAS_BVH4
        SWRT_DISABLE_OVERLAY
        SWRT_DISABLE_HEATMAP
)
wgsl2spirv(
    NAME softwarebvh_raytrace_rbvh_bvh4_debug
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-rbvh-card.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-rbvh-bvh4-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_TLAS_BVH4
        SWRT_ENABLE_TRACE_STATS
)
wgsl2spirv(
    NAME softwarebvh_tlas
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-tlas.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-tlas.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
)
wgsl2spirv(
    NAME softwarebvh_world_blas_update
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-world-blas-update.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/software-world-blas-update.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
)
wgsl2spirv(
    NAME softwarebvh_radixsort2
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/sort/wgsl/radixsort2.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/radixsort2.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/sort/wgsl/
)
add_dependencies(SoftwareBVHShaders softwarebvh_triangle_aabbs)
add_dependencies(SoftwareBVHShaders softwarebvh_morton)
add_dependencies(SoftwareBVHShaders softwarebvh_cwbvh)
add_dependencies(SoftwareBVHShaders softwarebvh_hlbvh)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_triangle_release)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_triangle_debug)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_triangle_bvh4_release)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_triangle_bvh4_debug)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_rbvh_release)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_rbvh_debug)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_rbvh_bvh4_release)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_rbvh_bvh4_debug)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_world_cwbvh_triangle_release)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_world_cwbvh_triangle_debug)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_world_cwbvh_subgroup_triangle_release)
add_dependencies(SoftwareBVHShaders softwarebvh_raytrace_world_cwbvh_subgroup_triangle_debug)
add_dependencies(SoftwareBVHShaders softwarebvh_tlas)
add_dependencies(SoftwareBVHShaders softwarebvh_world_blas_update)
add_dependencies(SoftwareBVHShaders softwarebvh_radixsort2)

add_custom_target(GlobalIlluminationShaders)
wgsl2spirv(
    NAME world_hit_shading_resolve_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/wgsl/world-hit-shading-resolve-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/world-hit-shading-resolve-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sdf_hit_shading_resolve_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/wgsl/sdf-hit-shading-resolve-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/sdf-hit-shading-resolve-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sdf_raytrace_evaluate_material_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/wgsl/sdf-raytrace-evaluate-material-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/sdf-raytrace-evaluate-material-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME trace_shading_albedo_debug_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/wgsl/trace-shading-albedo-debug-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/trace-shading-albedo-debug-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME screen_space_hit_shading_resolve_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/wgsl/screen-space-hit-shading-resolve-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/screen-space-hit-shading-resolve-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_raygen_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-raygen-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-raygen-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_light_grid_update_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-light-grid-update-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-light-grid-update-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_light_grid_debug_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-light-grid-debug-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-light-grid-debug-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_hit_shading_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-hit-shading-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-hit-shading-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_trace_surface_resolve_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-trace-surface-resolve-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-trace-surface-resolve-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_compose_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-compose-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-compose-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
add_dependencies(GlobalIlluminationShaders world_hit_shading_resolve_cs)
add_dependencies(GlobalIlluminationShaders sdf_hit_shading_resolve_cs)
add_dependencies(GlobalIlluminationShaders sdf_raytrace_evaluate_material_cs)
add_dependencies(GlobalIlluminationShaders trace_shading_albedo_debug_cs)
add_dependencies(GlobalIlluminationShaders screen_space_hit_shading_resolve_cs)
add_dependencies(GlobalIlluminationShaders worldgi_raygen_cs)
add_dependencies(GlobalIlluminationShaders worldgi_light_grid_update_cs)
add_dependencies(GlobalIlluminationShaders worldgi_light_grid_debug_cs)
add_dependencies(GlobalIlluminationShaders worldgi_hit_shading_cs)
add_dependencies(GlobalIlluminationShaders worldgi_trace_surface_resolve_cs)
add_dependencies(GlobalIlluminationShaders worldgi_compose_cs)

set(SURFEL_ACCELERATION_SHADER_SOURCES
    acceleration_clear
    acceleration_count
    acceleration_prefix_sum
    acceleration_prefix_segments
    acceleration_prefix_merge
    acceleration_insert
)

set(SURFEL_ACCELERATION_SHADER_FILES
    surfel-acceleration-clear-cs.wgsl
    surfel-acceleration-count-cs.wgsl
    surfel-acceleration-prefix-sum-cs.wgsl
    surfel-acceleration-prefix-segments-cs.wgsl
    surfel-acceleration-prefix-merge-cs.wgsl
    surfel-acceleration-insert-cs.wgsl
)

list(LENGTH SURFEL_ACCELERATION_SHADER_SOURCES SURFEL_ACCELERATION_SHADER_COUNT)
math(EXPR SURFEL_ACCELERATION_SHADER_LAST_INDEX "${SURFEL_ACCELERATION_SHADER_COUNT} - 1")
foreach(SURFEL_ACCELERATION_SHADER_INDEX RANGE ${SURFEL_ACCELERATION_SHADER_LAST_INDEX})
  list(GET SURFEL_ACCELERATION_SHADER_SOURCES ${SURFEL_ACCELERATION_SHADER_INDEX} SURFEL_ACCELERATION_SHADER_NAME)
  list(GET SURFEL_ACCELERATION_SHADER_FILES ${SURFEL_ACCELERATION_SHADER_INDEX} SURFEL_ACCELERATION_SHADER_FILE)
  string(REPLACE ".wgsl" ".spirv" SURFEL_ACCELERATION_SHADER_OUTPUT ${SURFEL_ACCELERATION_SHADER_FILE})
  wgsl2spirv(
      NAME surfel_${SURFEL_ACCELERATION_SHADER_NAME}_cs
      SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/surfels/wgsl/${SURFEL_ACCELERATION_SHADER_FILE}
      DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/surfels/${SURFEL_ACCELERATION_SHADER_OUTPUT}
      INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
  )
  add_dependencies(GlobalIlluminationShaders surfel_${SURFEL_ACCELERATION_SHADER_NAME}_cs)
endforeach()

add_dependencies(runtime GPUSignedDistanceFieldsShaders)
add_dependencies(runtime SDFShaders)
add_dependencies(runtime SoftwareBVHShaders)
add_dependencies(runtime GlobalIlluminationShaders)
