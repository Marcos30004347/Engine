if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "SAMPLE, WORLD_MAP, and OUTPUT are required")
endif()

execute_process(
  COMMAND "${SAMPLE}"
    --world-map "${WORLD_MAP}"
    --world-map-center 0.5,0.5
    --terrain-render-distance-km 1
    --terrain-builder gpu-full
    --gbuffer-resolver bricks
    --terrain-v2-phase0-report "${OUTPUT}"
    --test-camera-travel-m 16
    --frames 24
    --resolution 640x360
    --disable-lighting
    --no-taa
    --disable-metalfx-upscaler
    --disable-frame-generation
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE STDOUT
  ERROR_VARIABLE STDERR)
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 0 runtime baseline failed (${RESULT})\n"
    "${STDOUT}\n${STDERR}")
endif()
if(NOT EXISTS "${OUTPUT}")
  message(FATAL_ERROR "Phase 0 runtime did not create ${OUTPUT}")
endif()

file(READ "${OUTPUT}" REPORT)
foreach(REQUIRED
    "terrain-gpu-v2-phase0-v1"
    "cpu_frame_ms"
    "gpu_generation_ms"
    "forced_drain_events"
    "forced_drained_frames"
    "maximum_pending_frames"
    "runtime_wait_idle_calls"
    "build_scratch_bytes"
    "hidden_output_bytes"
    "peak_resident_page_bytes"
    "peak_tree64_bytes"
    "peak_total_accounted_bytes"
    "resident_page_budget_bytes"
    "status_readback_bytes"
    "submitted_batches"
    "maximum_active_lanes"
    "generation_time_budget_us"
    "p95_page_estimate_us"
    "transient_budget_bytes"
    "covered_pixels"
    "zero_coverage"
    "lod0_resident"
    "camera_clearance_metres"
    "output_hash")
  if(NOT REPORT MATCHES "${REQUIRED}")
    message(FATAL_ERROR "Phase 0 runtime report is missing ${REQUIRED}")
  endif()
endforeach()
if(NOT REPORT MATCHES "\"ground\": \\{\"found\": true")
  message(FATAL_ERROR "Phase 0 camera was not resolved against terrain")
endif()
if(NOT REPORT MATCHES "\"maximum_active_lanes\": 4")
  message(FATAL_ERROR
    "Phase 0 did not admit a complete four-page ownership group: ${REPORT}")
endif()
