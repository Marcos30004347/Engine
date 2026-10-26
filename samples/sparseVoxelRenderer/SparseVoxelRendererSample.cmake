get_filename_component(SPARSE_VOXEL_SAMPLE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
    SparseVoxelRendererSample
    ${SPARSE_VOXEL_SAMPLE_DIR}/SparseVoxelRendererSample.cpp
    ${SPARSE_VOXEL_SAMPLE_DIR}/SparseVoxelShadowBenchmark.cpp
)
target_link_libraries(SparseVoxelRendererSample PRIVATE Engine)

execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE SPARSE_VOXEL_BENCHMARK_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT SPARSE_VOXEL_BENCHMARK_GIT_COMMIT)
    set(SPARSE_VOXEL_BENCHMARK_GIT_COMMIT "unknown")
endif()
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE SPARSE_VOXEL_BENCHMARK_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(SPARSE_VOXEL_BENCHMARK_GIT_STATUS)
    set(SPARSE_VOXEL_BENCHMARK_GIT_DIRTY 1)
else()
    set(SPARSE_VOXEL_BENCHMARK_GIT_DIRTY 0)
endif()
target_compile_definitions(
    SparseVoxelRendererSample
    PRIVATE
        SPARSE_VOXEL_BENCHMARK_GIT_COMMIT="${SPARSE_VOXEL_BENCHMARK_GIT_COMMIT}"
        SPARSE_VOXEL_BENCHMARK_GIT_DIRTY=${SPARSE_VOXEL_BENCHMARK_GIT_DIRTY}
        # $<CONFIG>, not CMAKE_BUILD_TYPE: multi-config generators leave the
        # latter empty, so the Xcode tree reported its build type as "" - in the
        # one place where telling Debug from Release matters most.
        SPARSE_VOXEL_BENCHMARK_BUILD_TYPE="$<CONFIG>"
)
target_sources(
    SparseVoxelRendererSample
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_draw.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_tables.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/imgui_widgets.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_sdl3.cpp
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(
    SparseVoxelRendererSample
    PRIVATE
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui
        ${CMAKE_SOURCE_DIR}/thirdparty/imgui/backends
)
# imgui_impl_vulkan.h includes <vulkan/vulkan.h> directly, and Engine exposes
# the SDK headers only PRIVATE so they do not propagate here. The Makefile
# build happened to work because the SDK also installs into /usr/local/include,
# which Command Line Tools clang searches by default and Xcode's clang does not.
if(HAS_VULKAN)
    target_include_directories(SparseVoxelRendererSample PRIVATE ${Vulkan_INCLUDE_DIR})
endif()
# Shaders and assets are resolved relative to the executable, so the binary has
# to sit next to the generated assets/ tree. Multi-config generators such as
# Xcode append a per-config subdirectory to RUNTIME_OUTPUT_DIRECTORY, which
# would put it one level too deep; the per-config properties pin all of them to
# the same place. Single-config generators resolve these to the same directory.
set_target_properties(
    SparseVoxelRendererSample
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/tests
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/tests
)
# Bake the run configuration into the generated Xcode scheme so opening the
# project does not require re-entering launch arguments and re-enabling Metal
# capture by hand every time. SPARSE_VOXEL_SCHEME_ARGUMENTS is a CMake list, so
# each element becomes one argument row in the scheme.
if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(
        SPARSE_VOXEL_DEFAULT_SCHEME_ARGUMENTS
        "--terrain-package;${CMAKE_SOURCE_DIR}/build-release/tests/polygon-terrain-game-scale/planet.planetpkg;--terrain-chunk-radius;1;--terrain-lod-levels;1;--disable-vsync;--skip-output-signature"
    )
    if(NOT SPARSE_VOXEL_SCHEME_ARGUMENTS)
        set(
            SPARSE_VOXEL_SCHEME_ARGUMENTS
            "${SPARSE_VOXEL_DEFAULT_SCHEME_ARGUMENTS}"
            CACHE STRING "Launch arguments baked into the Xcode scheme" FORCE
        )
    endif()
    set_target_properties(
        SparseVoxelRendererSample
        PROPERTIES
            XCODE_GENERATE_SCHEME ON
            XCODE_SCHEME_ARGUMENTS "${SPARSE_VOXEL_SCHEME_ARGUMENTS}"
            XCODE_SCHEME_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
            # MoltenVK reaches Metal indirectly, so Xcode's automatic detection
            # does not enable the capture button on its own.
            XCODE_SCHEME_ENABLE_GPU_FRAME_CAPTURE_MODE "Metal"
            # Run in Release, not Xcode's default of Debug.
            #
            # Building Release from the command line does not change what the
            # Run button does - the scheme owns that, independently - so a
            # `cmake --build ... --config Release` followed by Cmd-R silently
            # rebuilds and launches Debug. Terrain startup and its Tree64 build
            # are also host-heavy enough that this can look like a hang.
            #
            # Release is also the right configuration to capture: a GPU trace of
            # an unoptimised host build measures a frame nobody ships. Switch
            # this to Debug in Edit Scheme when the host code needs stepping.
            XCODE_SCHEME_LAUNCH_CONFIGURATION "Release"
    )
endif()

add_dependencies(SparseVoxelRendererSample SparseVoxelRasterShaders)
add_dependencies(SparseVoxelRendererSample wgsl_virtualgeometry_depth_reduce_to_pot)
add_dependencies(SparseVoxelRendererSample wgsl_virtualgeometry_depth_pyramid_spd)
add_dependencies(SparseVoxelRendererSample wgsl_virtualgeometry_depth_pyramid_spd_source_depth)
add_dependencies(SparseVoxelRendererSample wgsl_virtualgeometry_cull_prepass_multipass_draw_indirect_count_disabled)
add_dependencies(SparseVoxelRendererSample wgsl_virtualgeometry_vertex_shader_draw_indirect_count_disabled)
add_dependencies(SparseVoxelRendererSample wgsl_virtualgeometry_vertex_shader_motion_draw_indirect_count_disabled)

# Deliberately unpinned: this is the only test that exercises whatever the
# sample's default G-buffer resolver is. Every other sample test names one.
add_test(NAME SparseVoxelRendererSampleSmoke COMMAND SparseVoxelRendererSample --frames 3 --no-terrain)
set_tests_properties(SparseVoxelRendererSampleSmoke PROPERTIES LABELS "svraster.g9;svraster.g12;svraster.g17b")

add_test(
    NAME SparseVoxelWorldTerrainRasterRegression
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelWorldTerrainRasterRegression.cmake
)
set_tests_properties(
    SparseVoxelWorldTerrainRasterRegression
    PROPERTIES
        LABELS "svraster.terrain;svraster.bricks;svraster.tree64;world-map;gpu"
        TIMEOUT 240
)

add_test(
    NAME SparseVoxelTerrainVirtualGeometryGBufferSmoke
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainVirtualGeometryGBufferSmoke.cmake
)
set_tests_properties(
    SparseVoxelTerrainVirtualGeometryGBufferSmoke
    PROPERTIES
        LABELS "svraster.terrain;virtualgeometry;virtualgeometry.gbuffer;world-map;gpu"
        TIMEOUT 240
)

add_test(
    NAME SparseVoxelTerrainVGCutErrorContract
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainVGCutErrorContract.cmake
)
set_tests_properties(
    SparseVoxelTerrainVGCutErrorContract
    PROPERTIES
        LABELS "svraster.terrain;virtualgeometry;virtualgeometry.lod;world-map;gpu;slow"
        TIMEOUT 1800
        # The contract asserts a convergence rate: chunk rebuilds per frame at
        # the generation budget. Sharing the GPU with five other tests slows
        # generation far below that rate and fails the run on timing alone.
        RUN_SERIAL TRUE
)

add_test(
    NAME SparseVoxelTerrainVGCapacityRefinement
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainVGCapacityRefinement.cmake
)
set_tests_properties(
    SparseVoxelTerrainVGCapacityRefinement
    PROPERTIES
        LABELS "svraster.terrain;virtualgeometry;virtualgeometry.lod;world-map;gpu;slow"
        TIMEOUT 900
        RUN_SERIAL TRUE
)

add_test(
    NAME SparseVoxelTerrainVGMovementContract
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainVGMovementContract.cmake
)
set_tests_properties(
    SparseVoxelTerrainVGMovementContract
    PROPERTIES
        LABELS "svraster.terrain;virtualgeometry;virtualgeometry.lod;world-map;gpu;slow"
        TIMEOUT 180
        RUN_SERIAL TRUE
)

add_test(
    NAME SparseVoxelTerrainVGSettleContract
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainVGSettleContract.cmake
)
set_tests_properties(
    SparseVoxelTerrainVGSettleContract
    PROPERTIES
        LABELS "svraster.terrain;virtualgeometry;virtualgeometry.lod;world-map;gpu;slow"
        TIMEOUT 900
        RUN_SERIAL TRUE
)

add_test(
    NAME SparseVoxelTerrainVGLodProfileContract
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainVGLodProfileContract.cmake
)
set_tests_properties(
    SparseVoxelTerrainVGLodProfileContract
    PROPERTIES
        LABELS "svraster.terrain;virtualgeometry;virtualgeometry.lod;world-map;gpu;slow"
        TIMEOUT 900
        RUN_SERIAL TRUE
)

add_test(
    NAME SparseVoxelTerrainGPUVGContract
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainGPUVGContract.cmake
)
set_tests_properties(
    SparseVoxelTerrainGPUVGContract
    PROPERTIES
        LABELS "svraster.terrain;virtualgeometry;virtualgeometry.lod;world-map;gpu;slow"
        # 700 frames of the GPU generation route take about 210 s on an idle
        # machine and four times that on one that has been running GPU tests
        # back to back for hours, which is how this timeout was first hit.
        TIMEOUT 1800
        RUN_SERIAL TRUE
)

add_test(
    NAME SparseVoxelWorldTerrainShadowParity
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelWorldTerrainShadowParity.cmake
)
set_tests_properties(
    SparseVoxelWorldTerrainShadowParity
    PROPERTIES
        LABELS "svraster.terrain;svraster.tree64;svraster.scene-rays;svraster.screen-trace;world-map;gpu"
        TIMEOUT 240
)

add_test(
    NAME SparseVoxelCapturedTerrainRegression
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelCapturedTerrainRegression.cmake
)
set_tests_properties(
    SparseVoxelCapturedTerrainRegression
    PROPERTIES
        LABELS "svraster.terrain;svraster.culling;svraster.tree64;svraster.scene-rays;svraster.screen-trace;world-map;gpu"
        TIMEOUT 3600
)

add_test(
    NAME SparseVoxelCapturedTerrainAngleCoverageRegression
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelCapturedTerrainAngleCoverageRegression.cmake
)
set_tests_properties(
    SparseVoxelCapturedTerrainAngleCoverageRegression
    PROPERTIES
        LABELS "svraster.terrain;svraster.culling;svraster.bricks;world-map;gpu"
        TIMEOUT 600
)

add_test(
    NAME SparseVoxelCapturedTerrainMotionRegression
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelCapturedTerrainMotionRegression.cmake
)
set_tests_properties(
    SparseVoxelCapturedTerrainMotionRegression
    PROPERTIES
        LABELS "svraster.terrain;svraster.culling;svraster.temporal;svraster.tree64;svraster.scene-rays;svraster.screen-trace;world-map;gpu"
        TIMEOUT 3600
)

add_test(
    NAME SparseVoxelCapturedTerrainNoAccumulationMotionRegression
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelCapturedTerrainNoAccumulationMotionRegression.cmake
)
set_tests_properties(
    SparseVoxelCapturedTerrainNoAccumulationMotionRegression
    PROPERTIES
        LABELS "svraster.terrain;svraster.temporal;svraster.tree64;svraster.scene-rays;world-map;gpu"
        TIMEOUT 3600
)

add_test(
    NAME SparseVoxelCapturedTerrainStreamingMotionRegression
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelCapturedTerrainStreamingMotionRegression.cmake
)
set_tests_properties(
    SparseVoxelCapturedTerrainStreamingMotionRegression
    PROPERTIES
        LABELS "svraster.terrain;svraster.streaming;svraster.temporal;svraster.tree64;svraster.scene-rays;svraster.screen-trace;world-map;gpu"
        TIMEOUT 3600
)

add_test(
    NAME SparseVoxelWorldTerrainLightingResidencyRegression
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelWorldTerrainLightingResidencyRegression.cmake
)
set_tests_properties(
    SparseVoxelWorldTerrainLightingResidencyRegression
    PROPERTIES
        LABELS "svraster.terrain;svraster.streaming;svraster.tree64;svraster.scene-rays;world-map;gpu"
        TIMEOUT 900
)

add_test(
    NAME SparseVoxelTerrainGBufferContract
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainGBufferContract.cmake
)
set_tests_properties(
    SparseVoxelTerrainGBufferContract
    PROPERTIES
        LABELS "svraster.gbuffer;svraster.terrain;svraster.bricks;world-map;gpu"
        TIMEOUT 180
)

# Walk at eye height across several streamed ownership changes.  This catches
# the GPU-v2 failure where a parent was evicted one transaction before its
# child payloads were generated, producing large black page-shaped holes.
add_test(
    NAME SparseVoxelWorldTerrainMovingCoverage
    COMMAND SparseVoxelRendererSample
        --world-map ${CMAKE_SOURCE_DIR}/world-data
        --world-map-center 0.5,0.5
        --terrain-render-distance-km 16
        --test-world-terrain-raster
        --test-camera-ground-forward
        --test-camera-height 1.8
        --test-camera-travel-m 60
        --frames 180
        --resolution 320x180
        --render-scale 1
        --no-taa
        --jitter-scale 0
        --disable-metalfx-upscaler
        --disable-frame-generation
        --disable-vsync
)
set_tests_properties(
    SparseVoxelWorldTerrainMovingCoverage
    PROPERTIES
        LABELS "svraster.terrain;svraster.streaming;svraster.bricks;world-map;gpu"
        TIMEOUT 120
)

# Rebuild the RenderGraph after terrain has published. The replacement graph
# owns fresh GPU arenas, so its CPU terrain residency must also start empty and
# regenerate every chunk instead of emitting only a streaming delta.
add_test(
    NAME SparseVoxelWorldTerrainGraphRecompile
    COMMAND SparseVoxelRendererSample
        --world-map ${CMAKE_SOURCE_DIR}/world-data
        --world-map-center 0.5,0.5
        --terrain-render-distance-km 1
        --test-world-terrain-raster
        --test-toggle-taa
        --frames 10
        --resolution 320x180
        --render-scale 1
        --jitter-scale 0
        --disable-metalfx-upscaler
        --disable-frame-generation
        --disable-vsync
)
set_tests_properties(
    SparseVoxelWorldTerrainGraphRecompile
    PROPERTIES
        LABELS "svraster.terrain;svraster.streaming;svraster.bricks;world-map;gpu"
        TIMEOUT 180
)

add_test(
    NAME SparseVoxelWorldTerrainLODStability
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelWorldTerrainLODStability.cmake
)
set_tests_properties(
    SparseVoxelWorldTerrainLODStability
    PROPERTIES
        LABELS "svraster.terrain;svraster.streaming;world-map;gpu"
        TIMEOUT 120
)

add_test(
    NAME SparseVoxelWorldTerrainStreamingRoundTrip
    COMMAND SparseVoxelRendererSample
        --world-map ${CMAKE_SOURCE_DIR}/world-data
        --world-map-center 0.5,0.5
        --terrain-render-distance-km 16
        --test-terrain-streaming
        --frames 120
        --resolution 960x540
)

add_test(
    NAME TerrainGPUV2Phase0GroundBaseline
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -DOUTPUT=${CMAKE_BINARY_DIR}/tests/terrain-gpu-v2-phase0-smoke.json
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/TerrainGPUV2Phase0Baseline.cmake
)
set_tests_properties(
    TerrainGPUV2Phase0GroundBaseline
    PROPERTIES
        LABELS "svraster.terrain.v2.phase0;svraster.terrain;world-map;gpu"
        TIMEOUT 240
)
set_tests_properties(
    SparseVoxelWorldTerrainStreamingRoundTrip
    PROPERTIES
        LABELS "svraster.terrain;svraster.streaming;svraster.tree64;world-map;gpu"
        TIMEOUT 120
)

add_test(
    NAME SparseVoxelLightFieldPageChurnInstrumentation
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelLightFieldPageChurnInstrumentation.cmake
)
set_tests_properties(
    SparseVoxelLightFieldPageChurnInstrumentation
    PROPERTIES
        LABELS "svraster.light-field;svraster.streaming;world-map;gpu"
        TIMEOUT 180
)

add_test(
    NAME SparseVoxelLightFieldStabilityVerification
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DWORLD_MAP=${CMAKE_SOURCE_DIR}/world-data
        -DWORLD_GENERATOR=$<TARGET_FILE:WorldGeneratorSample>
        -DGAME_WORLD_MAP=${CMAKE_BINARY_DIR}/tests/part4-world-game
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelLightFieldStabilityVerification.cmake
)
set_tests_properties(
    SparseVoxelLightFieldStabilityVerification
    PROPERTIES
        LABELS "svraster.gbuffer;svraster.light-field;svraster.terrain;world-map;gpu;slow"
        TIMEOUT 240
)

add_test(
    NAME SparseVoxelRasterR0ExactEquivalence
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelRasterR0Equivalence.cmake
)
set_tests_properties(
    SparseVoxelRasterR0ExactEquivalence
    PROPERTIES
        LABELS "svraster.r0;svraster.bricks;gpu"
        TIMEOUT 120
)

add_test(
    NAME SparseVoxelMicroRasterR1ExactEquivalence
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelMicroRasterR1Equivalence.cmake
)
set_tests_properties(
    SparseVoxelMicroRasterR1ExactEquivalence
    PROPERTIES
        LABELS "svraster.r1;svraster.bricks;gpu"
        TIMEOUT 120
)

# R2's optimization contract is structural: the compiled prepared fragment
# must not retain the legacy fine-occupancy walk or its dependent material
# lookup. Inspect every shipping/diagnostic and distance-cutoff permutation so
# an infrequently selected binary cannot silently regress to the old path.
set(SPARSE_VOXEL_R2_PREPARED_SHADER_DIR
    ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster)
add_test(
    NAME SparseVoxelMicroRasterR2ShaderContract
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelMicroRasterR2ShaderContract.py
        ${SPARSE_VOXEL_R2_PREPARED_SHADER_DIR}/sparse-voxel-brick-draw-prepared.spirv
        ${SPARSE_VOXEL_R2_PREPARED_SHADER_DIR}/sparse-voxel-brick-draw-prepared-diagnostics.spirv
        ${SPARSE_VOXEL_R2_PREPARED_SHADER_DIR}/sparse-voxel-brick-draw-prepared-micro-distance.spirv
        ${SPARSE_VOXEL_R2_PREPARED_SHADER_DIR}/sparse-voxel-brick-draw-prepared-micro-distance-diagnostics.spirv
)
set_tests_properties(
    SparseVoxelMicroRasterR2ShaderContract
    PROPERTIES LABELS "svraster.r2;svraster.bricks;shader"
)

add_test(
    NAME SparseVoxelMicroRasterR2DirectEquivalence
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelMicroRasterR2Equivalence.cmake
)
set_tests_properties(
    SparseVoxelMicroRasterR2DirectEquivalence
    PROPERTIES
        LABELS "svraster.r2;svraster.bricks;gpu"
        TIMEOUT 180
)

add_test(
    NAME SparseVoxelFrameGenerationSmoke
    COMMAND SparseVoxelRendererSample
        --frames 4
        --test-static-scene
        --frame-generation
        --disable-vsync
        --skip-output-signature
)
set_tests_properties(
    SparseVoxelFrameGenerationSmoke
    PROPERTIES LABELS "svraster.frame-generation;gpu"
)

add_test(
    NAME SparseVoxelSceneRayScreenWorldFrameParity
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelSceneRayFrameParity.cmake
)
if(APPLE AND TARGET vg_hardware_raytrace)
    add_test(NAME SparseVoxelHardwareReSTIRSmoke
        COMMAND ${CMAKE_COMMAND}
            -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
            -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelHardwareReSTIRSmoke.cmake)
    set_tests_properties(SparseVoxelHardwareReSTIRSmoke PROPERTIES
        LABELS "svraster.scene-rays;restir;metal;raytracing;gpu"
        SKIP_REGULAR_EXPRESSION "SKIP: Metal hardware ray tracing unavailable"
        TIMEOUT 240)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    if(APPLE)
        add_test(NAME SparseVoxelTerrainReSTIRSmoke
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainReSTIRSmoke.py
                $<TARGET_FILE:SparseVoxelRendererSample> ${CMAKE_SOURCE_DIR}/world-data)
        set_tests_properties(SparseVoxelTerrainReSTIRSmoke PROPERTIES
            LABELS "restir;terrain;metalfx;raytracing;gpu" SKIP_RETURN_CODE 77 TIMEOUT 320)
        add_test(NAME SparseVoxelTerrainReSTIRMovement
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainReSTIRMovement.py
                $<TARGET_FILE:SparseVoxelRendererSample> ${CMAKE_SOURCE_DIR}/world-data)
        set_tests_properties(SparseVoxelTerrainReSTIRMovement PROPERTIES
            LABELS "voxel;raytracing;metal;gpu;terrain;slow" SKIP_RETURN_CODE 77 TIMEOUT 260 RUN_SERIAL TRUE)
        add_test(NAME SparseVoxelTerrainCapacityRecovery
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelTerrainCapacityRecovery.py
                $<TARGET_FILE:SparseVoxelRendererSample> ${CMAKE_SOURCE_DIR}/world-data)
        set_tests_properties(SparseVoxelTerrainCapacityRecovery PROPERTIES
            LABELS "voxel;raytracing;metal;gpu;terrain;slow" TIMEOUT 260 RUN_SERIAL TRUE)
        add_test(NAME SparseVoxelMetalFXDenoiserSmoke
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelMetalFXDenoiserSmoke.py
                $<TARGET_FILE:SparseVoxelRendererSample>)
        set_tests_properties(SparseVoxelMetalFXDenoiserSmoke PROPERTIES
            LABELS "restir;metalfx;gpu" SKIP_RETURN_CODE 77 TIMEOUT 300)
    endif()
endif()
set_tests_properties(
    SparseVoxelSceneRayScreenWorldFrameParity
    PROPERTIES
        LABELS "svraster.scene-rays;svraster.tree64;gpu"
        TIMEOUT 180
)

add_test(
    NAME SparseVoxelOccupancyCameraTraceG29
    COMMAND SparseVoxelRendererSample
        --gbuffer-resolver svraster
        --frames 2
        --test-static-scene
        --test-occupancy-hit-cell
)
set_tests_properties(
    SparseVoxelOccupancyCameraTraceG29
    PROPERTIES
        LABELS "svraster.g29;gpu"
        PASS_REGULAR_EXPRESSION "hit=[1-9][0-9]* queryLimit=0"
)

add_test(
    NAME SparseVoxelOccupancyLargerCellRebuildG29
    COMMAND SparseVoxelRendererSample
        --gbuffer-resolver svraster
        --frames 2
        --test-static-scene
        --test-occupancy-hit-cell
        --occupancy-voxel-size 0.03125
        --occupancy-extent 256
)
set_tests_properties(
    SparseVoxelOccupancyLargerCellRebuildG29
    PROPERTIES
        LABELS "svraster.g24;svraster.g29;gpu"
        PASS_REGULAR_EXPRESSION "hit=[1-9][0-9]* queryLimit=0"
)

add_test(
    NAME SparseVoxelOccupancyMovingCameraG29
    COMMAND SparseVoxelRendererSample
        --gbuffer-resolver svraster
        --frames 8
        --test-static-scene
        --test-camera-yaw
        --test-occupancy-hit-cell
)
set_tests_properties(
    SparseVoxelOccupancyMovingCameraG29
    PROPERTIES
        LABELS "svraster.g24;svraster.g29;gpu"
        PASS_REGULAR_EXPRESSION "frame=7 invalid=0 miss=[0-9]+ hit=[1-9][0-9]* queryLimit=0"
)

add_test(
    NAME SparseVoxelSuzannePreviewRendererG17B
    COMMAND SparseVoxelRendererSample
        --gbuffer-resolver svraster
        --frames 3
        --test-obj-preview ${CMAKE_SOURCE_DIR}/assets/meshes/obj/suzanne.obj
        --voxel-size-meters 0.2
)
set_tests_properties(SparseVoxelSuzannePreviewRendererG17B PROPERTIES LABELS "svraster.g17b;svraster.m2;gpu")

add_test(
    NAME SparseVoxelStanfordBunnyPreviewRendererG17B
    COMMAND SparseVoxelRendererSample
        --gbuffer-resolver svraster
        --frames 3
        --test-obj-preview ${CMAKE_SOURCE_DIR}/assets/meshes/obj/stanford-bunny.obj
        --voxel-size-meters 0.01
)
set_tests_properties(SparseVoxelStanfordBunnyPreviewRendererG17B PROPERTIES LABELS "svraster.g17b;svraster.m2;gpu")

add_test(
    NAME SparseVoxelStanfordBunnyFrustumEquivalenceG22
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DOBJ=${CMAKE_SOURCE_DIR}/assets/meshes/obj/stanford-bunny.obj
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelFrustumEquivalence.cmake
)
set_tests_properties(SparseVoxelStanfordBunnyFrustumEquivalenceG22 PROPERTIES LABELS "svraster.g11;svraster.g22;svraster.m3;gpu")

add_test(
    NAME SparseVoxelVSVOFileRendererG18
    COMMAND SparseVoxelRendererSample
        --gbuffer-resolver svraster
        --frames 3
        --test-vsvo-roundtrip ${CMAKE_SOURCE_DIR}/assets/meshes/obj/suzanne.obj
        --voxel-size-meters 0.2
        --load-all-pages
)
set_tests_properties(SparseVoxelVSVOFileRendererG18 PROPERTIES LABELS "svraster.g18;svraster.m2;gpu")

add_test(
    NAME SparseVoxelVSVOFileDepthRendererG19
    COMMAND SparseVoxelRendererSample
        --gbuffer-resolver svraster
        --frames 3
        --depth-only
        --test-vsvo-roundtrip ${CMAKE_SOURCE_DIR}/assets/meshes/obj/suzanne.obj
        --voxel-size-meters 0.2
        --load-all-pages
)
set_tests_properties(SparseVoxelVSVOFileDepthRendererG19 PROPERTIES LABELS "svraster.g19;svraster.m3;gpu")

add_test(
    NAME SparseVoxelOcclusionGBufferEquivalenceG22
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelOcclusionEquivalence.cmake
)
set_tests_properties(SparseVoxelOcclusionGBufferEquivalenceG22 PROPERTIES LABELS "svraster.g20;svraster.g22;svraster.m3;gpu")

add_test(
    NAME SparseVoxelOcclusionDepthEquivalenceG22
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DDEPTH_ONLY=ON
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelOcclusionEquivalence.cmake
)
set_tests_properties(SparseVoxelOcclusionDepthEquivalenceG22 PROPERTIES LABELS "svraster.g20;svraster.g22;svraster.m3;gpu")

# The brick hierarchy's own occlusion path, on the one camera with enough depth
# complexity to exercise it. Asserts both halves: the image is unchanged, and
# culling actually removed bricks.
add_test(
    NAME SparseVoxelBrickOcclusionEquivalenceG22
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DRESOLVER=bricks
        -DSCENE_OBJ=${CMAKE_SOURCE_DIR}/assets/meshes/obj/sponza.obj
        -DBENCHMARK_TIMINGS=ON
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelOcclusionEquivalence.cmake
)
set_tests_properties(
    SparseVoxelBrickOcclusionEquivalenceG22
    PROPERTIES
        LABELS "svraster.g20;svraster.g22;svraster.bricks;gpu;benchmark"
        TIMEOUT 180
)

add_test(
    NAME SparseVoxelOcclusionDisocclusionEquivalenceG22
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DDISOCCLUSION=ON
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelOcclusionEquivalence.cmake
)
set_tests_properties(SparseVoxelOcclusionDisocclusionEquivalenceG22 PROPERTIES LABELS "svraster.g22;svraster.m3;gpu")

add_test(
    NAME SparseVoxelOcclusionFrameStabilityG22
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DFRAME_STABILITY=ON
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelOcclusionEquivalence.cmake
)
set_tests_properties(SparseVoxelOcclusionFrameStabilityG22 PROPERTIES LABELS "svraster.g22;svraster.m3;gpu")

add_test(
    NAME SparseVoxelOcclusionUIToggleG22
    COMMAND SparseVoxelRendererSample --gbuffer-resolver svraster --frames 3 --test-toggle-occlusion
)
set_tests_properties(SparseVoxelOcclusionUIToggleG22 PROPERTIES LABELS "svraster.g22;svraster.m3;gpu")

add_test(
    NAME SparseVoxelPassTimelineG22
    COMMAND SparseVoxelRendererSample --gbuffer-resolver svraster --frames 5 --enable-occlusion --gpu-timings --no-terrain
)
set_tests_properties(SparseVoxelPassTimelineG22 PROPERTIES LABELS "svraster.g22;svraster.m3;gpu")

add_test(
    NAME SparseVoxelShadowBenchmarkT0
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/SparseVoxelShadowBenchmarkT0.json
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelShadowBenchmarkSmoke.cmake
)
set_tests_properties(
    SparseVoxelShadowBenchmarkT0
    PROPERTIES LABELS "svraster.tree64.t0;gpu"
)

add_test(
    NAME SparseVoxelProbeBenchmarkMetadataT0
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/SparseVoxelProbeBenchmarkMetadataT0.json
        -DWORKLOAD=lighting
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelShadowBenchmarkSmoke.cmake
)
set_tests_properties(
    SparseVoxelProbeBenchmarkMetadataT0
    PROPERTIES LABELS "svraster.probe.t0;gpu"
)

add_test(
    NAME SparseVoxelTree64CutoverT9
    COMMAND SparseVoxelRendererSample
        --gbuffer-resolver svraster
        --frames 10
        --test-static-scene
        --test-occupancy-hit-cell
)
set_tests_properties(
    SparseVoxelTree64CutoverT9
    PROPERTIES
        LABELS "svraster.tree64.t9;gpu"
        # Guards "the Tree64 representation is built and ready". It used to
        # assert occupied>0 on the static chunk directory, which an instanced
        # scene leaves empty by design once the dynamic layer landed - so it
        # asserted on the wrong layer and could never pass. Assert readiness
        # plus a populated dynamic layer, which is where the geometry now is.
        PASS_REGULAR_EXPRESSION "Tree64 state: ready=1 pending=0 unknown=0 occupied=[0-9]+ liveNodes=[0-9]+ dynamic=1 dynModels=[1-9][0-9]* dynInstances=[1-9][0-9]* dynNodes=[1-9][0-9]*"
)

add_test(
    NAME SparseVoxelTree64BenchmarkT9
    COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:SparseVoxelRendererSample>
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/SparseVoxelTree64BenchmarkT9.json
        -DBACKEND=tree64
        -DWARMUP=9
        -DOCCUPANCY_EXTENT=512
        -P ${CMAKE_SOURCE_DIR}/tests/voxel/SparseVoxelShadowBenchmarkSmoke.cmake
)
set_tests_properties(
    SparseVoxelTree64BenchmarkT9
    PROPERTIES LABELS "svraster.tree64.t9;gpu"
)

add_test(
    NAME SparseVoxelVSVOSelectiveRendererG18
    COMMAND SparseVoxelRendererSample
        --gbuffer-resolver svraster
        --frames 8
        --test-vsvo-selective ${CMAKE_SOURCE_DIR}/assets/meshes/obj/suzanne.obj
        --voxel-size-meters 0.2
)
set_tests_properties(SparseVoxelVSVOSelectiveRendererG18 PROPERTIES LABELS "svraster.g18;svraster.m2;gpu")
