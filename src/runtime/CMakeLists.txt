include(${CMAKE_SOURCE_DIR}/cmake/WGSL2SpirV.cmake)

file(GLOB_RECURSE ENGINE_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/runtime/*.c"
    "${CMAKE_SOURCE_DIR}/src/runtime/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/voxel/*.c"
    "${CMAKE_SOURCE_DIR}/src/voxel/*.cpp"
    "${CMAKE_SOURCE_DIR}/thirdparty/rpmalloc/*.c"
    "${CMAKE_SOURCE_DIR}/thirdparty/o1heap/o1heap/*.c"
)

add_library(runtime ${ENGINE_RUNTIME_SOURCES})
engine_apply_runtime_target_defaults(runtime)

add_custom_target(AtmosphereShaders)
wgsl2spirv(
    NAME atmosphere_transmittance
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/atmosphere_transmittance.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/atmosphere/atmosphere-transmittance.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/
)
wgsl2spirv(
    NAME atmosphere_multiple_scattering
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/atmosphere_multiple_scattering.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/atmosphere/atmosphere-multiple-scattering.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/
)
wgsl2spirv(
    NAME atmosphere_sky_view
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/atmosphere_sky_view.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/atmosphere/atmosphere-sky-view.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/
)
wgsl2spirv(
    NAME atmosphere_aerial_perspective
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/atmosphere_aerial_perspective.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/atmosphere/atmosphere-aerial-perspective.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/
)
wgsl2spirv(
    NAME atmosphere_aerial_temporal
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/atmosphere_aerial_temporal.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/atmosphere/atmosphere-aerial-temporal.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/
)
wgsl2spirv(
    NAME atmosphere_ambient
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/atmosphere_ambient.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/atmosphere/atmosphere-ambient.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME atmosphere_lut_debug
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/atmosphere_lut_debug.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/atmosphere/atmosphere-lut-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/atmosphere/wgsl/
)
add_dependencies(AtmosphereShaders atmosphere_transmittance)
add_dependencies(AtmosphereShaders atmosphere_multiple_scattering)
add_dependencies(AtmosphereShaders atmosphere_sky_view)
add_dependencies(AtmosphereShaders atmosphere_aerial_perspective)
add_dependencies(AtmosphereShaders atmosphere_aerial_temporal)
add_dependencies(AtmosphereShaders atmosphere_ambient)
add_dependencies(AtmosphereShaders atmosphere_lut_debug)

# MetalFX is a native external-RHI pass: keep Objective-C++ and Apple
# frameworks out of all non-Apple builds and out of individual samples.
if(APPLE)
    set(NATIVE_METAL_PASS_SOURCES
        ${CMAKE_SOURCE_DIR}/src/runtime/rendering/gpgpu/MetalFXPass.mm
        ${CMAKE_SOURCE_DIR}/src/voxel/svraster/rendering/TerrainMetalComputeRasterPass.mm)
    target_sources(runtime PRIVATE ${NATIVE_METAL_PASS_SOURCES})
    set_source_files_properties(
        ${NATIVE_METAL_PASS_SOURCES}
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
    target_link_libraries(
        runtime
        PRIVATE "-framework Metal" "-framework MetalFX"
    )
endif()

add_custom_target(SparseVoxelRasterShaders)
wgsl2spirv(
    NAME terrain_gpu_build_synthetic
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_build_synthetic.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-build-synthetic.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders terrain_gpu_build_synthetic)
wgsl2spirv(
    NAME terrain_gpu_v2_scan
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_scan.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-v2-scan.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders terrain_gpu_v2_scan)
foreach(TERRAIN_V2_COLUMN_WORKGROUP 8x8 16x8 8x16)
    if(TERRAIN_V2_COLUMN_WORKGROUP STREQUAL "8x8")
        set(TERRAIN_V2_COLUMN_WORKGROUP_X 8)
        set(TERRAIN_V2_COLUMN_WORKGROUP_Z 8)
    elseif(TERRAIN_V2_COLUMN_WORKGROUP STREQUAL "16x8")
        set(TERRAIN_V2_COLUMN_WORKGROUP_X 16)
        set(TERRAIN_V2_COLUMN_WORKGROUP_Z 8)
    else()
        set(TERRAIN_V2_COLUMN_WORKGROUP_X 8)
        set(TERRAIN_V2_COLUMN_WORKGROUP_Z 16)
    endif()
    wgsl2spirv(
        NAME terrain_gpu_v2_columns_${TERRAIN_V2_COLUMN_WORKGROUP}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_columns.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-v2-columns-${TERRAIN_V2_COLUMN_WORKGROUP}.spirv
        DEFINES
            TERRAIN_V2_COLUMN_WORKGROUP_X=${TERRAIN_V2_COLUMN_WORKGROUP_X}
            TERRAIN_V2_COLUMN_WORKGROUP_Z=${TERRAIN_V2_COLUMN_WORKGROUP_Z}
        INCLUDES
            ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
            ${CMAKE_SOURCE_DIR}/assets/shaders/procedural/wgsl/
    )
    add_dependencies(
        SparseVoxelRasterShaders
        terrain_gpu_v2_columns_${TERRAIN_V2_COLUMN_WORKGROUP})
endforeach()
foreach(TERRAIN_V2_FINE_RASTER_WORKGROUP 64 128 256)
    wgsl2spirv(
        NAME terrain_gpu_v2_fine_raster_${TERRAIN_V2_FINE_RASTER_WORKGROUP}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_fine_raster.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-v2-fine-raster-${TERRAIN_V2_FINE_RASTER_WORKGROUP}.spirv
        DEFINES
            TERRAIN_V2_FINE_RASTER_WORKGROUP=${TERRAIN_V2_FINE_RASTER_WORKGROUP}
        INCLUDES
            ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
    )
    add_dependencies(
        SparseVoxelRasterShaders
        terrain_gpu_v2_fine_raster_${TERRAIN_V2_FINE_RASTER_WORKGROUP})
endforeach()
foreach(TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP 64 128 256)
    wgsl2spirv(
        NAME terrain_gpu_v2_generated_vg_emit_${TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_generated_vg_emit.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-v2-generated-vg-emit-${TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP}.spirv
        DEFINES
            TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP=${TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP}
        INCLUDES
            ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
            ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/
    )
    add_dependencies(
        SparseVoxelRasterShaders
        terrain_gpu_v2_generated_vg_emit_${TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP})
endforeach()
foreach(TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP 64 128 256)
    wgsl2spirv(
        NAME terrain_gpu_v2_raster_hierarchy_${TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_raster_hierarchy.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-v2-raster-hierarchy-${TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP}.spirv
        DEFINES
            TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP=${TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP}
        INCLUDES
            ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
    )
    add_dependencies(
        SparseVoxelRasterShaders
        terrain_gpu_v2_raster_hierarchy_${TERRAIN_V2_RASTER_HIERARCHY_WORKGROUP})
endforeach()
foreach(TERRAIN_V2_TREE64_WORKGROUP 64 128 256)
    wgsl2spirv(
        NAME terrain_gpu_v2_tree64_${TERRAIN_V2_TREE64_WORKGROUP}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_tree64.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-v2-tree64-${TERRAIN_V2_TREE64_WORKGROUP}.spirv
        DEFINES
            TERRAIN_V2_TREE64_WORKGROUP=${TERRAIN_V2_TREE64_WORKGROUP}
        INCLUDES
            ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
    )
    add_dependencies(
        SparseVoxelRasterShaders
        terrain_gpu_v2_tree64_${TERRAIN_V2_TREE64_WORKGROUP})
endforeach()
wgsl2spirv(
    NAME terrain_gpu_v2_publication
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_publication.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-v2-publication.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders terrain_gpu_v2_publication)
wgsl2spirv(
    NAME terrain_gpu_v2_renderer_adapter
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_renderer_adapter.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-v2-renderer-adapter.spirv
)
add_dependencies(SparseVoxelRasterShaders terrain_gpu_v2_renderer_adapter)
wgsl2spirv(
    NAME terrain_gpu_column_build
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_column_build.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-column-build.spirv
    INCLUDES
        ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
        ${CMAKE_SOURCE_DIR}/assets/shaders/procedural/wgsl/
)
add_dependencies(SparseVoxelRasterShaders terrain_gpu_column_build)
wgsl2spirv(
    NAME terrain_gpu_raster_build
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_raster_build.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-raster-build.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders terrain_gpu_raster_build)
foreach(TERRAIN_TREE64_STAGE reset masks compact validate)
    if(TERRAIN_TREE64_STAGE STREQUAL "reset")
        set(TERRAIN_TREE64_STAGE_INDEX 0)
    elseif(TERRAIN_TREE64_STAGE STREQUAL "masks")
        set(TERRAIN_TREE64_STAGE_INDEX 1)
    elseif(TERRAIN_TREE64_STAGE STREQUAL "compact")
        set(TERRAIN_TREE64_STAGE_INDEX 2)
    else()
        set(TERRAIN_TREE64_STAGE_INDEX 3)
    endif()
    wgsl2spirv(
        NAME terrain_gpu_tree64_${TERRAIN_TREE64_STAGE}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_tree64_build.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-tree64-${TERRAIN_TREE64_STAGE}.spirv
        DEFINES TERRAIN_TREE64_SHADER_STAGE=${TERRAIN_TREE64_STAGE_INDEX}
        INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
    )
endforeach()
add_custom_target(terrain_gpu_tree64_build)
add_dependencies(terrain_gpu_tree64_build
    terrain_gpu_tree64_reset terrain_gpu_tree64_masks
    terrain_gpu_tree64_compact terrain_gpu_tree64_validate)
add_dependencies(SparseVoxelRasterShaders terrain_gpu_tree64_build)
foreach(TERRAIN_JOINT_STAGE validate relocate finalize)
    if(TERRAIN_JOINT_STAGE STREQUAL "validate")
        set(TERRAIN_JOINT_STAGE_INDEX 0)
    elseif(TERRAIN_JOINT_STAGE STREQUAL "relocate")
        set(TERRAIN_JOINT_STAGE_INDEX 1)
    else()
        set(TERRAIN_JOINT_STAGE_INDEX 2)
    endif()
    wgsl2spirv(
        NAME terrain_gpu_joint_publication_${TERRAIN_JOINT_STAGE}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_joint_publication.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/terrain-gpu-joint-publication-${TERRAIN_JOINT_STAGE}.spirv
        DEFINES TERRAIN_JOINT_SHADER_STAGE=${TERRAIN_JOINT_STAGE_INDEX}
        INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
    )
endforeach()
add_custom_target(terrain_gpu_joint_publication)
add_dependencies(terrain_gpu_joint_publication
    terrain_gpu_joint_publication_validate
    terrain_gpu_joint_publication_relocate
    terrain_gpu_joint_publication_finalize)
add_dependencies(SparseVoxelRasterShaders terrain_gpu_joint_publication)
wgsl2spirv(
    NAME sparse_voxel_bootstrap
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_bootstrap.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-bootstrap.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_bootstrap)
wgsl2spirv(
    NAME sparse_voxel_lod
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_lod.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-lod.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_lod)
wgsl2spirv(
    NAME sparse_voxel_tlas_culling
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tlas_culling.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tlas-culling.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tlas_culling)
wgsl2spirv(
    NAME sparse_voxel_streaming
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_streaming.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-streaming.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_streaming)
wgsl2spirv(
    NAME sparse_voxel_occupancy
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_occupancy.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-occupancy.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_occupancy)
wgsl2spirv(
    NAME sparse_voxel_occupancy_dirty
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_occupancy_dirty.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-occupancy-dirty.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_occupancy_dirty)
wgsl2spirv(
    NAME sparse_voxel_occupancy_sources
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_occupancy_sources.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-occupancy-sources.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_occupancy_sources)
wgsl2spirv(
    NAME sparse_voxel_occupancy_scene
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_occupancy_scene.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-occupancy-scene.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_occupancy_scene)
wgsl2spirv(
    NAME sparse_voxel_occupancy_probe
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_occupancy_probe.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-occupancy-probe.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_occupancy_probe)
wgsl2spirv(
    NAME sparse_voxel_occupancy_mips
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_occupancy_mips.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-occupancy-mips.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_occupancy_mips)
wgsl2spirv(
    NAME sparse_voxel_occupancy_dda
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_occupancy_dda.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-occupancy-dda.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_occupancy_dda)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace.spirv
    DEFINES TREE64_TRACE_STATISTICS=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_storage_dispatch
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-storage-dispatch.spirv
    DEFINES TREE64_STORAGE_DISPATCH=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_storage_dispatch)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_padded
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-padded.spirv
    DEFINES TREE64_PADDED_NODE=1 TREE64_TRACE_STATISTICS=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_padded)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_mirrored_ancestor
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-mirrored-ancestor.spirv
    DEFINES TREE64_FAST_TRAVERSAL=1 TREE64_TRACE_STATISTICS=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_mirrored_ancestor)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_dynamic
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-dynamic.spirv
    DEFINES TREE64_DYNAMIC_LAYER=1 TREE64_TRACE_STATISTICS=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_dynamic)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_dynamic_storage_dispatch
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-dynamic-storage-dispatch.spirv
    DEFINES TREE64_DYNAMIC_LAYER=1 TREE64_STORAGE_DISPATCH=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_dynamic_storage_dispatch)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_dynamic_storage_dispatch_any_hit
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-dynamic-storage-dispatch-any-hit.spirv
    DEFINES TREE64_DYNAMIC_LAYER=1 TREE64_STORAGE_DISPATCH=1 TREE64_ANY_HIT_GLOBAL_DYNAMIC=1 TREE64_DYNAMIC_PADDED_NODE=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_dynamic_storage_dispatch_any_hit)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_dynamic_mirrored_ancestor
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-dynamic-mirrored-ancestor.spirv
    DEFINES TREE64_DYNAMIC_LAYER=1 TREE64_FAST_TRAVERSAL=1 TREE64_TRACE_STATISTICS=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_dynamic_mirrored_ancestor)
# Ancestor-stack traversal combined with an indirect, GPU-authored ray count.
# The scene shadow/GI chain needs both at once: it is by far the heaviest trace
# in a frame and it is the one dispatched from the compaction pass, so without
# these two variants it was stuck restarting the descent from the chunk root at
# every DDA step.
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_storage_dispatch_mirrored_ancestor
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-storage-dispatch-mirrored-ancestor.spirv
    DEFINES TREE64_STORAGE_DISPATCH=1 TREE64_FAST_TRAVERSAL=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_storage_dispatch_mirrored_ancestor)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_dynamic_storage_dispatch_mirrored_ancestor
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-dynamic-storage-dispatch-mirrored-ancestor.spirv
    DEFINES TREE64_DYNAMIC_LAYER=1 TREE64_STORAGE_DISPATCH=1 TREE64_FAST_TRAVERSAL=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_dynamic_storage_dispatch_mirrored_ancestor)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_dynamic_storage_dispatch_mirrored_ancestor_any_hit
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-dynamic-storage-dispatch-mirrored-ancestor-any-hit.spirv
    DEFINES TREE64_DYNAMIC_LAYER=1 TREE64_STORAGE_DISPATCH=1 TREE64_FAST_TRAVERSAL=1 TREE64_ANY_HIT_GLOBAL_DYNAMIC=1 TREE64_DYNAMIC_PADDED_NODE=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_dynamic_storage_dispatch_mirrored_ancestor_any_hit)
# Primary-ray variants for the Tree64 G-buffer resolver: the ray comes from the
# invocation index and a camera uniform instead of a full-resolution ray array.
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_primary
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-primary.spirv
    DEFINES TREE64_PRIMARY_RAYS=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_primary)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_dynamic_primary
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-dynamic-primary.spirv
    DEFINES TREE64_DYNAMIC_LAYER=1 TREE64_PRIMARY_RAYS=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_dynamic_primary)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_primary_mirrored_ancestor
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-primary-mirrored-ancestor.spirv
    DEFINES TREE64_PRIMARY_RAYS=1 TREE64_FAST_TRAVERSAL=1 TREE64_SUBGROUP_BROADCAST=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_primary_mirrored_ancestor)
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_dynamic_primary_mirrored_ancestor
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-dynamic-primary-mirrored-ancestor.spirv
    DEFINES TREE64_DYNAMIC_LAYER=1 TREE64_PRIMARY_RAYS=1 TREE64_FAST_TRAVERSAL=1 TREE64_SUBGROUP_BROADCAST=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_dynamic_primary_mirrored_ancestor)

# Material-aware variants are selected only when a Tree64 trace pass receives
# the material/context sidecar. Keeping them separate leaves disabled
# materials on the byte-identical Phase-0 shader path.
function(tree64_material_trace_variant TARGET_NAME OUTPUT_NAME)
    wgsl2spirv(
        NAME ${TARGET_NAME}_material
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/${OUTPUT_NAME}.material
        DEFINES TREE64_MATERIAL_GEOMETRY=1 ${ARGN}
        INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
    )
    add_dependencies(SparseVoxelRasterShaders ${TARGET_NAME}_material)
endfunction()

tree64_material_trace_variant(sparse_voxel_tree64_trace sparse-voxel-tree64-trace.spirv TREE64_TRACE_STATISTICS=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_storage_dispatch sparse-voxel-tree64-trace-storage-dispatch.spirv TREE64_STORAGE_DISPATCH=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_padded sparse-voxel-tree64-trace-padded.spirv TREE64_PADDED_NODE=1 TREE64_TRACE_STATISTICS=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_mirrored_ancestor sparse-voxel-tree64-trace-mirrored-ancestor.spirv TREE64_FAST_TRAVERSAL=1 TREE64_TRACE_STATISTICS=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_storage_dispatch_mirrored_ancestor sparse-voxel-tree64-trace-storage-dispatch-mirrored-ancestor.spirv TREE64_STORAGE_DISPATCH=1 TREE64_FAST_TRAVERSAL=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_primary sparse-voxel-tree64-trace-primary.spirv TREE64_PRIMARY_RAYS=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_primary_mirrored_ancestor sparse-voxel-tree64-trace-primary-mirrored-ancestor.spirv TREE64_PRIMARY_RAYS=1 TREE64_FAST_TRAVERSAL=1 TREE64_SUBGROUP_BROADCAST=1)

tree64_material_trace_variant(sparse_voxel_tree64_trace_dynamic sparse-voxel-tree64-trace-dynamic.spirv TREE64_DYNAMIC_LAYER=1 TREE64_TRACE_STATISTICS=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_dynamic_storage_dispatch sparse-voxel-tree64-trace-dynamic-storage-dispatch.spirv TREE64_DYNAMIC_LAYER=1 TREE64_STORAGE_DISPATCH=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_dynamic_storage_dispatch_any_hit sparse-voxel-tree64-trace-dynamic-storage-dispatch-any-hit.spirv TREE64_DYNAMIC_LAYER=1 TREE64_STORAGE_DISPATCH=1 TREE64_ANY_HIT_GLOBAL_DYNAMIC=1 TREE64_DYNAMIC_PADDED_NODE=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_dynamic_mirrored_ancestor sparse-voxel-tree64-trace-dynamic-mirrored-ancestor.spirv TREE64_DYNAMIC_LAYER=1 TREE64_FAST_TRAVERSAL=1 TREE64_TRACE_STATISTICS=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_dynamic_storage_dispatch_mirrored_ancestor sparse-voxel-tree64-trace-dynamic-storage-dispatch-mirrored-ancestor.spirv TREE64_DYNAMIC_LAYER=1 TREE64_STORAGE_DISPATCH=1 TREE64_FAST_TRAVERSAL=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_dynamic_storage_dispatch_mirrored_ancestor_any_hit sparse-voxel-tree64-trace-dynamic-storage-dispatch-mirrored-ancestor-any-hit.spirv TREE64_DYNAMIC_LAYER=1 TREE64_STORAGE_DISPATCH=1 TREE64_FAST_TRAVERSAL=1 TREE64_ANY_HIT_GLOBAL_DYNAMIC=1 TREE64_DYNAMIC_PADDED_NODE=1)

# BVH4 broad phase. Same traversal and same BLAS; only the instance broad phase
# differs, so the pair can be compared on one build by publishing or withholding
# the TLAS node count.
wgsl2spirv(
    NAME sparse_voxel_tree64_trace_dynamic_storage_dispatch_mirrored_ancestor_any_hit_tlas_bvh4
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-trace-dynamic-storage-dispatch-mirrored-ancestor-any-hit-tlas-bvh4.spirv
    DEFINES TREE64_DYNAMIC_LAYER=1 TREE64_STORAGE_DISPATCH=1 TREE64_FAST_TRAVERSAL=1 TREE64_ANY_HIT_GLOBAL_DYNAMIC=1 TREE64_DYNAMIC_PADDED_NODE=1 TREE64_TLAS_BVH4=1
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_trace_dynamic_storage_dispatch_mirrored_ancestor_any_hit_tlas_bvh4)
tree64_material_trace_variant(sparse_voxel_tree64_trace_dynamic_primary sparse-voxel-tree64-trace-dynamic-primary.spirv TREE64_DYNAMIC_LAYER=1 TREE64_PRIMARY_RAYS=1)
tree64_material_trace_variant(sparse_voxel_tree64_trace_dynamic_primary_mirrored_ancestor sparse-voxel-tree64-trace-dynamic-primary-mirrored-ancestor.spirv TREE64_DYNAMIC_LAYER=1 TREE64_PRIMARY_RAYS=1 TREE64_FAST_TRAVERSAL=1 TREE64_SUBGROUP_BROADCAST=1)

wgsl2spirv(
    NAME sparse_voxel_tree64_build
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_build.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-build.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_build)
wgsl2spirv(
    NAME sparse_voxel_tree64_batch_build
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_batch_build.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-batch-build.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_batch_build)
wgsl2spirv(
    NAME sparse_voxel_tree64_scene
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_scene.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-scene.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_scene)
wgsl2spirv(
    NAME sparse_voxel_tree64_publish
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_publish.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-publish.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_publish)
wgsl2spirv(
    NAME sparse_voxel_terrain_upload
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_terrain_upload.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-terrain-upload.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_terrain_upload)
wgsl2spirv(
    NAME sparse_voxel_occupancy_shadow_rays
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_occupancy_shadow_rays.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-occupancy-shadow-rays.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_occupancy_shadow_rays)
wgsl2spirv(
    NAME sparse_voxel_scene_ray_create
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_ray_create.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-scene-ray-create.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_scene_ray_create)
wgsl2spirv(
    NAME sparse_voxel_visible_receiver_map
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_visible_receiver_map.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-visible-receiver-map.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_visible_receiver_map)
wgsl2spirv(
    NAME sparse_voxel_scene_ray_worklist
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_ray_worklist.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-scene-ray-worklist.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_scene_ray_worklist)
wgsl2spirv(
    NAME sparse_voxel_di_reservoir
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_di_reservoir.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-di-reservoir.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_di_reservoir)
wgsl2spirv(
    NAME sparse_voxel_di_spatial_reservoir
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_di_spatial_reservoir.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-di-spatial-reservoir.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_di_spatial_reservoir)
wgsl2spirv(
    NAME sparse_voxel_scene_screen_trace
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_screen_trace.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-scene-screen-trace.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_scene_screen_trace)
wgsl2spirv(
    NAME sparse_voxel_scene_tree64_prepare
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_tree64_prepare.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-scene-tree64-prepare.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_scene_tree64_prepare)
wgsl2spirv(
    NAME sparse_voxel_scene_hit_merge
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_hit_merge.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-scene-hit-merge.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_scene_hit_merge)
wgsl2spirv(
    NAME sparse_voxel_scene_hit_temporal
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_hit_temporal.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-scene-hit-temporal.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_scene_hit_temporal)
wgsl2spirv(
    NAME sparse_voxel_scene_ray_compact
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_ray_compact.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-scene-ray-compact.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_scene_ray_compact)
wgsl2spirv(
    NAME sparse_voxel_scene_ray_statistics_reset
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_ray_statistics_reset.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-scene-ray-statistics-reset.spirv
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_scene_ray_statistics_reset)
wgsl2spirv(
    NAME sparse_voxel_scene_ray_statistics
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_ray_statistics.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-scene-ray-statistics.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_scene_ray_statistics)
wgsl2spirv(
    NAME sparse_voxel_binning
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_binning.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-binning.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_radix
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_radix.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-radix.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_resolve
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_resolve.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-resolve.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_tree64_primary
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_tree64_primary.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-tree64-primary.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
# Brick hierarchy resolver: cull, rasterize, then copy into the resolver
# contract. See docs/voxel-gbuffer-resolvers-plan.md part C.
wgsl2spirv(
    NAME sparse_voxel_brick_cull
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_cull.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-cull.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_brick_cull)
wgsl2spirv(
    NAME sparse_voxel_terrain_feedback_pack
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_terrain_feedback_pack.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-terrain-feedback-pack.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_terrain_feedback_pack)
# R9 H0: measurement-only screen-tile census over the visible brick set. Sizes
# the compute path's per-tile candidate lists, which is what decides how many
# payload bits a 32-bit packed atomicMin can spare.
wgsl2spirv(
    NAME sparse_voxel_brick_tile_bin
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_tile_bin.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-tile-bin.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_brick_tile_bin)
# R9 H1: tiled compute raster, written to comparison buffers only.
wgsl2spirv(
    NAME sparse_voxel_brick_tile_raster
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_tile_raster.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-tile-raster.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_brick_tile_raster)
if(APPLE)
    set(SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_SRC
        ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/metal/sparse_voxel_terrain_compute_raster_atomic64.metal)
    set(SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_AIR
        ${CMAKE_BINARY_DIR}/tests/assets/shaders/metal/svraster/sparse-voxel-terrain-compute-raster-atomic64.air)
    set(SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_LIB
        ${CMAKE_BINARY_DIR}/tests/assets/shaders/metal/svraster/sparse-voxel-terrain-compute-raster-atomic64.metallib)
    target_compile_definitions(runtime PUBLIC
        SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METALLIB_PATH="${SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_LIB}")
    add_custom_command(
        OUTPUT ${SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_LIB}
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_BINARY_DIR}/tests/assets/shaders/metal/svraster
        COMMAND xcrun -sdk macosx metal -c
            ${SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_SRC}
            -o ${SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_AIR}
        COMMAND xcrun -sdk macosx metallib
            ${SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_AIR}
            -o ${SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_LIB}
        DEPENDS ${SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_SRC}
        VERBATIM)
    add_custom_target(
        sparse_voxel_terrain_compute_raster_atomic64_metal
        DEPENDS ${SPARSE_VOXEL_TERRAIN_COMPUTE_RASTER_METAL_LIB})
    add_dependencies(
        SparseVoxelRasterShaders
        sparse_voxel_terrain_compute_raster_atomic64_metal)
endif()
wgsl2spirv(
    NAME sparse_voxel_brick_draw
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-draw.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_brick_draw)
# Generic parent-voxel-only raster for authored scenes that do not need exact
# micro-material geometry. Keep the historical conservative-depth contract for
# these fixtures; generated terrain selects the separate late-depth variant
# below.
wgsl2spirv(
    NAME sparse_voxel_brick_draw_no_micro
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-draw-no-micro.spirv
    DEFINES SPARSE_VOXEL_BRICK_NO_MICRO
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
    EXECUTION_MODES DepthLess
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_brick_draw_no_micro)
# Parent-voxel-only generated terrain raster. Ten-centimetre terrain voxels
# already are the shipping geometric unit, so this compiles out descriptor
# lookup and exact-micro DDA, but does not request conservative early-depth:
# close/grazing terrain relies on the fragment DDA for the exact surface.
wgsl2spirv(
    NAME sparse_voxel_brick_draw_no_micro_terrain
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-draw-no-micro-terrain.spirv
    DEFINES
        SPARSE_VOXEL_BRICK_NO_MICRO
        SPARSE_VOXEL_BRICK_TERRAIN_PROXY_STRADDLE_FALLBACK
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_brick_draw_no_micro_terrain)
# Diagnostic raster variant. All fragment-side traversal and overdraw counters
# are compiled out of the shipping shader above, so Metal encoder timings are
# never attributed to an instrumented binary by accident.
wgsl2spirv(
    NAME sparse_voxel_brick_draw_diagnostics
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-draw-diagnostics.spirv
    DEFINES SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_brick_draw_diagnostics)
wgsl2spirv(
    NAME sparse_voxel_brick_draw_no_micro_diagnostics
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-draw-no-micro-diagnostics.spirv
    DEFINES
        SPARSE_VOXEL_BRICK_NO_MICRO
        SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
    EXECUTION_MODES DepthLess
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_brick_draw_no_micro_diagnostics)
wgsl2spirv(
    NAME sparse_voxel_brick_draw_no_micro_terrain_diagnostics
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-draw-no-micro-terrain-diagnostics.spirv
    DEFINES
        SPARSE_VOXEL_BRICK_NO_MICRO
        SPARSE_VOXEL_BRICK_TERRAIN_PROXY_STRADDLE_FALLBACK
        SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_brick_draw_no_micro_terrain_diagnostics)
# Optional exact-micro distance cutoff. Keep this as a second compilation of
# the shipping source: the default target has no define, so its preprocessed
# token stream and fast path remain unchanged.
wgsl2spirv(
    NAME sparse_voxel_brick_draw_micro_distance
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-draw-micro-distance.spirv
    DEFINES SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_brick_draw_micro_distance)
wgsl2spirv(
    NAME sparse_voxel_brick_draw_micro_distance_diagnostics
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-draw-micro-distance-diagnostics.spirv
    DEFINES
        SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF
        SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_brick_draw_micro_distance_diagnostics)
# R1 Phase0 split. Residual variants read MicroPrepare occupancy by visible
# list slot; prepared variants consume one compact record per extracted parent.
# Keep diagnostic and distance-cutoff permutations separate so the default
# binaries retain the same compile-time counter/distance elimination contract.
function(sparse_voxel_brick_r1_shader NAME OUTPUT)
    wgsl2spirv(
        NAME ${NAME}
        SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl
        DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/${OUTPUT}.spirv
        DEFINES ${ARGN}
        INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
    )
    add_dependencies(SparseVoxelRasterShaders ${NAME})
endfunction()
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_residual
    sparse-voxel-brick-draw-residual
    SPARSE_VOXEL_BRICK_RESIDUAL_RASTER)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_residual_diagnostics
    sparse-voxel-brick-draw-residual-diagnostics
    SPARSE_VOXEL_BRICK_RESIDUAL_RASTER
    SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_residual_micro_distance
    sparse-voxel-brick-draw-residual-micro-distance
    SPARSE_VOXEL_BRICK_RESIDUAL_RASTER
    SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_residual_micro_distance_diagnostics
    sparse-voxel-brick-draw-residual-micro-distance-diagnostics
    SPARSE_VOXEL_BRICK_RESIDUAL_RASTER
    SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF
    SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_prepared
    sparse-voxel-brick-draw-prepared
    SPARSE_VOXEL_BRICK_PREPARED_RASTER)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_prepared_diagnostics
    sparse-voxel-brick-draw-prepared-diagnostics
    SPARSE_VOXEL_BRICK_PREPARED_RASTER
    SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_prepared_micro_distance
    sparse-voxel-brick-draw-prepared-micro-distance
    SPARSE_VOXEL_BRICK_PREPARED_RASTER
    SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_prepared_micro_distance_diagnostics
    sparse-voxel-brick-draw-prepared-micro-distance-diagnostics
    SPARSE_VOXEL_BRICK_PREPARED_RASTER
    SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF
    SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS)
# R2 direct-parent raster discards only ambiguous parent-boundary and exterior
# side-entry samples. These replay variants run the incumbent owner walk for
# that sparse set, keeping the hot prepared variants free of occupancy and
# prefix access.
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_prepared_tie_replay
    sparse-voxel-brick-draw-prepared-tie-replay
    SPARSE_VOXEL_BRICK_PREPARED_RASTER
    SPARSE_VOXEL_BRICK_PREPARED_TIE_REPLAY)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_prepared_tie_replay_diagnostics
    sparse-voxel-brick-draw-prepared-tie-replay-diagnostics
    SPARSE_VOXEL_BRICK_PREPARED_RASTER
    SPARSE_VOXEL_BRICK_PREPARED_TIE_REPLAY
    SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_prepared_tie_replay_micro_distance
    sparse-voxel-brick-draw-prepared-tie-replay-micro-distance
    SPARSE_VOXEL_BRICK_PREPARED_RASTER
    SPARSE_VOXEL_BRICK_PREPARED_TIE_REPLAY
    SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF)
sparse_voxel_brick_r1_shader(
    sparse_voxel_brick_draw_prepared_tie_replay_micro_distance_diagnostics
    sparse-voxel-brick-draw-prepared-tie-replay-micro-distance-diagnostics
    SPARSE_VOXEL_BRICK_PREPARED_RASTER
    SPARSE_VOXEL_BRICK_PREPARED_TIE_REPLAY
    SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF
    SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS)
wgsl2spirv(
    NAME sparse_voxel_micro_prepare
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_micro_prepare.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-micro-prepare.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_micro_prepare)
wgsl2spirv(
    NAME sparse_voxel_micro_prepare_diagnostics
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_micro_prepare.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-micro-prepare-diagnostics.spirv
    DEFINES SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_micro_prepare_diagnostics)
wgsl2spirv(
    NAME sparse_voxel_brick_resolve
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_resolve.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-resolve.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_brick_resolve)
wgsl2spirv(
    NAME sparse_voxel_vg_gbuffer_adapter
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_vg_gbuffer_adapter.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-vg-gbuffer-adapter.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_vg_gbuffer_adapter)
# Overdraw drain/reset is compiled into a separate resolver. The shipping
# target has neither the counter bindings nor the per-pixel atomic exchange.
wgsl2spirv(
    NAME sparse_voxel_brick_resolve_diagnostics
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_resolve.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-brick-resolve-diagnostics.spirv
    DEFINES SPARSE_VOXEL_BRICK_RESOLVE_DIAGNOSTICS
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_brick_resolve_diagnostics)
# T0 instrument for the terrain mesh-proxy plan: rasterizes the resident
# terrain height field as a mesh under the scene camera and counts fragments
# per pixel. Diagnostic only; nothing downstream reads its attachments.
wgsl2spirv(
    NAME sparse_voxel_terrain_mesh_probe
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_terrain_mesh_probe.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-terrain-mesh-probe.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_terrain_mesh_probe)
# T2: the same mesh, publishing the brick resolver's contract instead of a
# fragment counter. Keep it on the same late-depth contract as the brick path:
# close/grazing terrain proxies are correctness-sensitive, and this diagnostic
# should not hide that with a stricter conservative-depth assumption.
wgsl2spirv(
    NAME sparse_voxel_terrain_mesh_contract
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_terrain_mesh_probe.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-terrain-mesh-contract.spirv
    DEFINES SPARSE_VOXEL_TERRAIN_MESH_CONTRACT
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(
    SparseVoxelRasterShaders
    sparse_voxel_terrain_mesh_contract)
# World-anchored irradiance cache. The tracing is the existing Tree64 pass;
# these two kernels only produce its rays and fold its results into the cache.
wgsl2spirv(
    NAME worldgi_ddgi_raygen
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi_ddgi_raygen.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/worldgi/worldgi-ddgi-raygen.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/
)
add_dependencies(SparseVoxelRasterShaders worldgi_ddgi_raygen)
wgsl2spirv(
    NAME worldgi_ddgi_update
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi_ddgi_update.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/worldgi/worldgi-ddgi-update.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/
)
add_dependencies(SparseVoxelRasterShaders worldgi_ddgi_update)
wgsl2spirv(
    NAME sparse_voxel_gbuffer
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_gbuffer.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-gbuffer.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_depth
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_depth.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-depth.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_depth_readback
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_depth_readback.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-depth-readback.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_depth_present
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_depth_present.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-depth-present.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_material_gbuffer
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_material_gbuffer.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-material-gbuffer.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME wgsl_virtualgeometry_id_readback_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/virtualgeometry-id-readback-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/virtualgeometry-id-readback-cs.spirv
)

wgsl2spirv(
    NAME sparse_voxel_gbuffer_readback
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_gbuffer_readback.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-gbuffer-readback.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_exact_output_readback
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_exact_output_readback.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-exact-output-readback.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_color_readback
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_color_readback.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-color-readback.spirv
)
wgsl2spirv(
    NAME sparse_voxel_hzb_readback
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_hzb_readback.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-hzb-readback.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_present
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_present.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-present.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_compose
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_compose.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-compose.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_atmosphere_composite
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_atmosphere_composite.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-atmosphere-composite.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_planet_background
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_background.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-planet-background.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_planet_cbt_allocator
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_allocator.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-planet-cbt-allocator.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_planet_cbt_update
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_cbt_update.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-planet-cbt-update.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_planet_merge
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_merge.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-planet-merge.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_planet_overview
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_overview.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-planet-overview.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_planet_overview_atmosphere
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_overview_atmosphere.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-planet-overview-atmosphere.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_atmosphere_terrain_shadow
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_atmosphere_terrain_shadow.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-atmosphere-terrain-shadow.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_planet_overview_coverage
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_overview_coverage.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-planet-overview-coverage.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_planet_navigation_merge
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_planet_navigation_merge.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-planet-navigation-merge.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_light_field
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_light_field.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-light-field.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_sky_guide
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_sky_guide.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-sky-guide.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_probe_place
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_probe_place.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-probe-place.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_probe_filter
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_probe_filter.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-probe-filter.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_probe_share
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_probe_share.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-probe-share.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_probe_rays
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_probe_rays.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-probe-rays.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_probe_resolve
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_probe_resolve.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-probe-resolve.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME sparse_voxel_probe_interpolate
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_probe_interpolate.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-probe-interpolate.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/ ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_blit
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_blit.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-blit.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_binning)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_radix)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_resolve)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_tree64_primary)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_gbuffer)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_depth)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_depth_readback)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_depth_present)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_material_gbuffer)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_gbuffer_readback)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_exact_output_readback)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_color_readback)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_hzb_readback)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_present)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_compose)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_atmosphere_composite)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_planet_background)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_planet_cbt_allocator)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_planet_cbt_update)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_planet_merge)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_planet_overview)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_planet_overview_atmosphere)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_atmosphere_terrain_shadow)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_planet_overview_coverage)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_planet_navigation_merge)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_light_field)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_sky_guide)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_probe_place)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_probe_filter)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_probe_share)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_probe_rays)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_probe_resolve)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_probe_interpolate)
wgsl2spirv(
    NAME sparse_voxel_light_gizmo
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_light_gizmo.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-light-gizmo.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_light_gizmo)
wgsl2spirv(
    NAME sparse_voxel_gtao
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_gtao.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-gtao.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/ ${CMAKE_SOURCE_DIR}/assets/shaders/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_gtao)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_blit)
wgsl2spirv(
    NAME sparse_voxel_crt_signal
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_crt_signal.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-crt-signal.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_crt_beam
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_crt_beam.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-crt-beam.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_crt_phosphor
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_crt_phosphor.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-crt-phosphor.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_crt_glow
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_crt_glow.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-crt-glow.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
wgsl2spirv(
    NAME sparse_voxel_crt_glass
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_crt_glass.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-crt-glass.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/
)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_crt_signal)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_crt_beam)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_crt_phosphor)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_crt_glow)
add_dependencies(SparseVoxelRasterShaders sparse_voxel_crt_glass)

add_custom_target(RenderingGpgpuShaders)
set(RENDERING_GPGPU_SHADER_SOURCES
    fsr2-frame-reset-cs
    fsr2-exposure-cs
    fsr2-coarse-luma-cs
    fsr2-reconstruct-dilate-cs
    fsr2-depth-reactive-cs
    fsr2-lock-cs
    fsr2-accumulate-cs
    fsr31-accumulate-cs
    fsr2-history-copy-cs
    fsr2-rcas-cs
    frame-generation-luma-pyramid-cs
    frame-generation-motion-pyramid-cs
    frame-generation-optical-flow-cs
    frame-generation-interpolate-cs
    frame-generation-inpaint-pyramid-cs
    frame-generation-inpaint-cs
    temporal-metrics-cs
    lighting-temporal-accumulation-cs
)
foreach(RENDERING_GPGPU_SHADER IN LISTS RENDERING_GPGPU_SHADER_SOURCES)
  string(REPLACE "-" "_" RENDERING_GPGPU_SHADER_TARGET ${RENDERING_GPGPU_SHADER})
  wgsl2spirv(
      NAME ${RENDERING_GPGPU_SHADER_TARGET}
      SRC ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/${RENDERING_GPGPU_SHADER}.wgsl
      DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/${RENDERING_GPGPU_SHADER}.spirv
      INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/utils/wgsl/
  )
  add_dependencies(RenderingGpgpuShaders ${RENDERING_GPGPU_SHADER_TARGET})
endforeach()

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
    DEFINES
        SWRT_TRIANGLE_PRIMITIVES
)
wgsl2spirv(
    NAME softwarebvh_raytrace_triangle_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-triangle-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_TRIANGLE_PRIMITIVES
        SWRT_DISABLE_OVERLAY
        SWRT_DISABLE_HEATMAP
)
wgsl2spirv(
    NAME softwarebvh_raytrace_triangle_debug
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-triangle-debug.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_TRIANGLE_PRIMITIVES
        SWRT_ENABLE_TRACE_STATS
)
wgsl2spirv(
    NAME softwarebvh_raytrace_triangle_bvh4_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-direct-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-triangle-bvh4-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_TRIANGLE_PRIMITIVES
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
        SWRT_TRIANGLE_PRIMITIVES
        SWRT_TLAS_BVH4
        SWRT_ENABLE_TRACE_STATS
)
wgsl2spirv(
    NAME softwarebvh_raytrace_world_cwbvh_triangle_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-world-cwbvh-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-triangle-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_TRIANGLE_PRIMITIVES
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
        SWRT_TRIANGLE_PRIMITIVES
        SWRT_WORLD_CWBVH
        SWRT_ENABLE_TRACE_STATS
)
wgsl2spirv(
    NAME softwarebvh_raytrace_world_cwbvh_subgroup_triangle_release
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/software-raytrace-world-cwbvh-triangle.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/softwarebvh-raytrace-world-cwbvh-subgroup-triangle-release.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/softwarebvh/wgsl/
    DEFINES
        SWRT_TRIANGLE_PRIMITIVES
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
        SWRT_TRIANGLE_PRIMITIVES
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
add_dependencies(SparseVoxelRasterShaders softwarebvh_radixsort2)

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
    NAME worldgi_radiance_cache_touch_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-radiance-cache-touch-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-radiance-cache-touch-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_radiance_cache_shade_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-radiance-cache-shade-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-radiance-cache-shade-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_radiance_cache_debug_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-radiance-cache-debug-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-radiance-cache-debug-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_shadow_probe_spawn_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-shadow-probe-spawn-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-shadow-probe-spawn-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_shadow_probe_trace_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-shadow-probe-trace-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-shadow-probe-trace-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME screen_raytrace_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/raytracing/wgsl/screen-raytrace-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/raytracing/screen-raytrace-cs.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
wgsl2spirv(
    NAME worldgi_shadow_probe_fill_cs
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/globalIllumination/worldgi/wgsl/worldgi-shadow-probe-fill-cs.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/globalIllumination/worldgi/worldgi-shadow-probe-fill-cs.spirv
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
add_dependencies(GlobalIlluminationShaders worldgi_radiance_cache_touch_cs)
add_dependencies(GlobalIlluminationShaders worldgi_radiance_cache_shade_cs)
add_dependencies(GlobalIlluminationShaders worldgi_radiance_cache_debug_cs)
add_dependencies(GlobalIlluminationShaders worldgi_shadow_probe_spawn_cs)
add_dependencies(GlobalIlluminationShaders worldgi_shadow_probe_trace_cs)
add_dependencies(GlobalIlluminationShaders screen_raytrace_cs)
add_dependencies(GlobalIlluminationShaders worldgi_shadow_probe_fill_cs)
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

add_custom_target(ProceduralShaders)
wgsl2spirv(
    NAME map_export_pages_test
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/procedural/wgsl/map_export_pages_test.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/procedural/map-export-pages-test.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/procedural/wgsl/
)
wgsl2spirv(
    NAME planet_stream_power
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/procedural/wgsl/planet_stream_power.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/procedural/planet-stream-power.spirv
    INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/
)
add_dependencies(ProceduralShaders planet_stream_power)
add_dependencies(ProceduralShaders map_export_pages_test)

add_dependencies(runtime GPUSignedDistanceFieldsShaders)
add_dependencies(runtime ProceduralShaders)
add_dependencies(runtime RenderingGpgpuShaders)
add_dependencies(runtime SoftwareBVHShaders)
add_dependencies(runtime GlobalIlluminationShaders)
add_dependencies(runtime AtmosphereShaders)
