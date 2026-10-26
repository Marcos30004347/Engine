if(NOT DEFINED SAMPLE OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "SAMPLE and OUTPUT are required")
endif()
if(NOT DEFINED BACKEND)
  set(BACKEND packed-dda)
endif()
if(NOT DEFINED WARMUP)
  set(WARMUP 1)
endif()
if(NOT DEFINED OCCUPANCY_EXTENT)
  set(OCCUPANCY_EXTENT 128)
endif()
if(NOT DEFINED WORKLOAD)
  set(WORKLOAD shadow)
endif()
if(NOT WORKLOAD STREQUAL "shadow" AND NOT WORKLOAD STREQUAL "lighting")
  message(FATAL_ERROR "WORKLOAD must be shadow or lighting")
endif()

set(BENCHMARK_OPTION "--benchmark-shadow")
set(EXPECTED_RASTER_LOD "1[.]5")
set(RASTER_LOD_OPTIONS)
if(WORKLOAD STREQUAL "lighting")
  set(BENCHMARK_OPTION "--benchmark-lighting")
  # Supply only the raster override. The serialized Tree64 threshold must stay
  # at the scene's 1.5-pixel default, proving the two controls are independent
  # in benchmark mode.
  set(EXPECTED_RASTER_LOD "7")
  list(APPEND RASTER_LOD_OPTIONS --lod-threshold 7)
endif()

execute_process(
  COMMAND
    "${SAMPLE}"
    # Keep the backend smoke test on the deterministic diagnostic shell. The
    # interactive sample now defaults to streamed planet terrain, which both
    # requires Tree64 and depends on an external exported package.
    --no-terrain
    # Pinned to SVRaster so the benchmark keeps measuring the same chain it
    # was calibrated against, independent of the sample's default resolver.
    --gbuffer-resolver svraster
    --shadow-backend "${BACKEND}"
    "${BENCHMARK_OPTION}" "${OUTPUT}"
    --benchmark-sequence static
    --benchmark-warmup "${WARMUP}"
    --benchmark-frames 2
    --benchmark-resolution 320x180
    ${RASTER_LOD_OPTIONS}
    --occupancy-extent "${OCCUPANCY_EXTENT}"
    --probe-density-half 123
    --probe-density-quarter 234
    --probe-local-light-cull-start 345
    --probe-analytical-distance 456
    --probe-tile 10
    --probe-adaptive-percent 17
    --probe-awake-rays 23
    --planet-background-stride 4
    # Generated terrain is permanently parent-voxel-only at 10 cm. There is
    # no terrain micro-material switch left to serialize into this benchmark.
    # Named explicitly, like every other asserted field: the compact/residual
    # split is opt-in now that it measured slower than the incumbent draw, so
    # the serialized "true" below has to come from a flag rather than a default.
    --brick-micro-raster
    --brick-exact-micro-distance 789
    --svgf-atrous-iterations 1
  RESULT_VARIABLE BENCHMARK_RESULT
  OUTPUT_VARIABLE BENCHMARK_STDOUT
  ERROR_VARIABLE BENCHMARK_STDERR
)

if(NOT BENCHMARK_RESULT EQUAL 0)
  message(
    FATAL_ERROR
    "${WORKLOAD} benchmark failed (${BENCHMARK_RESULT})\n"
    "stdout:\n${BENCHMARK_STDOUT}\n"
    "stderr:\n${BENCHMARK_STDERR}"
  )
endif()

if(NOT EXISTS "${OUTPUT}")
  message(FATAL_ERROR "${WORKLOAD} benchmark did not create ${OUTPUT}")
endif()

file(READ "${OUTPUT}" BENCHMARK_JSON)
foreach(REQUIRED_PATTERN
    "\"schema_version\"[ \t\r\n]*:[ \t\r\n]*19"
    "\"backend\"[ \t\r\n]*:[ \t\r\n]*\"${BACKEND}\""
    "\"terrain_streaming_mode\"[ \t\r\n]*:[ \t\r\n]*\"none\""
    "\"measured_frames\"[ \t\r\n]*:[ \t\r\n]*2"
    "\"raster_lod_pixel_threshold\"[ \t\r\n]*:[ \t\r\n]*${EXPECTED_RASTER_LOD}"
    "\"tree64_lod_pixel_threshold\"[ \t\r\n]*:[ \t\r\n]*1[.]5"
    "\"frames\"[ \t\r\n]*:"
    "\"aggregates\"[ \t\r\n]*:"
    "\"brick_phase0_total_ms\"[ \t\r\n]*:"
    "\"tree64_total_persistent_bytes\"[ \t\r\n]*:"
    "\"probe_density_half_distance_metres\"[ \t\r\n]*:[ \t\r\n]*123"
    "\"probe_density_quarter_distance_metres\"[ \t\r\n]*:[ \t\r\n]*234"
    "\"probe_trace_fade_start_distance_metres\"[ \t\r\n]*:[ \t\r\n]*345"
    "\"probe_analytical_distance_metres\"[ \t\r\n]*:[ \t\r\n]*456"
    "\"probe_tile_size\"[ \t\r\n]*:[ \t\r\n]*10"
    "\"probe_adaptive_percent\"[ \t\r\n]*:[ \t\r\n]*17"
    "\"probe_awake_rays\"[ \t\r\n]*:[ \t\r\n]*23"
    "\"planet_background_stride\"[ \t\r\n]*:[ \t\r\n]*4"
    "\"terrain_micro_materials_enabled\"[ \t\r\n]*:[ \t\r\n]*false"
    "\"brick_micro_raster_enabled\"[ \t\r\n]*:[ \t\r\n]*true"
    "\"brick_micro_raster_mode\"[ \t\r\n]*:[ \t\r\n]*\"r2-heightfield-parent\""
    "\"brick_micro_record_capacity\"[ \t\r\n]*:[ \t\r\n]*[1-9][0-9]*"
    "\"brick_micro_record_buffer_bytes\"[ \t\r\n]*:[ \t\r\n]*[1-9][0-9]*"
    "\"brick_micro_residual_buffer_bytes\"[ \t\r\n]*:[ \t\r\n]*[1-9][0-9]*"
    "\"brick_micro_dispatch_buffer_bytes\"[ \t\r\n]*:[ \t\r\n]*12"
    "\"brick_micro_draw_args_buffer_bytes\"[ \t\r\n]*:[ \t\r\n]*16"
    "\"brick_micro_total_buffer_bytes\"[ \t\r\n]*:[ \t\r\n]*[1-9][0-9]*"
    "\"brick_exact_micro_distance_cutoff_metres\"[ \t\r\n]*:[ \t\r\n]*789"
    "\"svgf_atrous_iterations\"[ \t\r\n]*:[ \t\r\n]*1"
    "\"ray_hash\"[ \t\r\n]*:[ \t\r\n]*\"[0-9a-f][0-9a-f]*\""
    "\"result_hash\"[ \t\r\n]*:[ \t\r\n]*\"[0-9a-f][0-9a-f]*\"")
  if(NOT BENCHMARK_JSON MATCHES "${REQUIRED_PATTERN}")
    message(FATAL_ERROR "Shadow benchmark JSON is missing ${REQUIRED_PATTERN}")
  endif()
endforeach()

foreach(REQUIRED_GUIDE_PATTERN
    "\"sky_guiding_strength\"[ \t\r\n]*:"
    "\"sky_guiding_learning_rate\"[ \t\r\n]*:"
    "\"ray_refresh_interval\"[ \t\r\n]*:"
    "\"minimum_converged_samples\"[ \t\r\n]*:"
    "\"convergence_relative_error\"[ \t\r\n]*:"
    "\"stable_observation_count\"[ \t\r\n]*:"
    "\"region_stability_tile_size\"[ \t\r\n]*:"
    "\"region_wake_frames\"[ \t\r\n]*:"
    "\"region_soft_visibility_threshold\"[ \t\r\n]*:"
    "\"region_soft_visibility_streak\"[ \t\r\n]*:"
    "\"region_soft_prefill_stride\"[ \t\r\n]*:"
    "\"ray_tile_budget_fraction\"[ \t\r\n]*:"
    "\"ray_tile_budget_boost\"[ \t\r\n]*:"
    "\"ray_tile_budget_heartbeat\"[ \t\r\n]*:"
    "\"occluder_invalidation\"[ \t\r\n]*:"
    "\"sky_guide_total_bytes\"[ \t\r\n]*:[ \t\r\n]*[1-9][0-9]*")
  if(NOT BENCHMARK_JSON MATCHES "${REQUIRED_GUIDE_PATTERN}")
    message(FATAL_ERROR "Shadow benchmark JSON is missing ${REQUIRED_GUIDE_PATTERN}")
  endif()
endforeach()

if(BACKEND STREQUAL "tree64")
  foreach(REQUIRED_TREE64_PATTERN
      "\"tree64_chunk_side\"[ \t\r\n]*:[ \t\r\n]*64"
      "\"tree64_shadow_lod_distance_slope\"[ \t\r\n]*:"
      "\"tree64_shadow_lod_start_distance\"[ \t\r\n]*:"
      "\"tree64_shadow_lod_maximum_world_size\"[ \t\r\n]*:"
      "\"tree64_total_persistent_bytes\"[ \t\r\n]*:[ \t\r\n]*[1-9][0-9]*")
    if(NOT BENCHMARK_JSON MATCHES "${REQUIRED_TREE64_PATTERN}")
      message(FATAL_ERROR "Tree64 benchmark JSON is missing ${REQUIRED_TREE64_PATTERN}")
    endif()
  endforeach()
endif()

message(STATUS "${WORKLOAD} benchmark smoke report validated: ${OUTPUT}")
