if(NOT DEFINED BENCHMARK OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "BENCHMARK and OUTPUT are required")
endif()

execute_process(
  COMMAND "${BENCHMARK}" --quick --output "${OUTPUT}"
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE STDOUT
  ERROR_VARIABLE STDERR)
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR
    "Phase 0 terrain benchmark failed (${RESULT})\n${STDOUT}\n${STDERR}")
endif()
if(NOT EXISTS "${OUTPUT}")
  message(FATAL_ERROR "Phase 0 terrain benchmark did not create ${OUTPUT}")
endif()
file(READ "${OUTPUT}" JSON)
foreach(REQUIRED
    "gpu-terrain-generation-phase0-v1"
    "map_sampling_ms"
    "shell_extraction_ms"
    "brick_hierarchy_ms"
    "tree64_ms"
    "emitted_runs"
    "horizontal_voxels"
    "vertical_voxel_capacity"
    "request_to_visible_ms"
    "upload_bytes"
    "semantic_hash")
  if(NOT JSON MATCHES "${REQUIRED}")
    message(FATAL_ERROR "Phase 0 terrain benchmark JSON is missing ${REQUIRED}")
  endif()
endforeach()
