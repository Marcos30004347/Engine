if(NOT DEFINED SAMPLE OR NOT DEFINED OBJ)
  message(FATAL_ERROR "SAMPLE and OBJ are required")
endif()

foreach(FRAME_COUNT 1 2 3 4)
  # Pinned to SVRaster: the counters this compares are the BLAS/LOD
  # selection ones, which the brick resolver does not produce.
  set(COMMON_ARGS
      --gbuffer-resolver svraster
      --frames ${FRAME_COUNT}
      --test-camera-yaw
      --lod-threshold 8
      --test-obj-preview "${OBJ}"
      --voxel-size-meters 0.01)

  execute_process(
    COMMAND "${SAMPLE}" ${COMMON_ARGS}
    RESULT_VARIABLE CULLED_RESULT
    OUTPUT_VARIABLE CULLED_STDOUT
    ERROR_VARIABLE CULLED_STDERR)
  if(NOT CULLED_RESULT EQUAL 0)
    message(FATAL_ERROR "BLAS-frustum renderer failed at frame ${FRAME_COUNT}:\n${CULLED_STDOUT}\n${CULLED_STDERR}")
  endif()

  execute_process(
    COMMAND "${SAMPLE}" ${COMMON_ARGS} --disable-blas-frustum
    RESULT_VARIABLE UNCULLED_RESULT
    OUTPUT_VARIABLE UNCULLED_STDOUT
    ERROR_VARIABLE UNCULLED_STDERR)
  if(NOT UNCULLED_RESULT EQUAL 0)
    message(FATAL_ERROR "No-BLAS-frustum reference failed at frame ${FRAME_COUNT}:\n${UNCULLED_STDOUT}\n${UNCULLED_STDERR}")
  endif()

  set(CULLED_LOG "${CULLED_STDOUT}\n${CULLED_STDERR}")
  set(UNCULLED_LOG "${UNCULLED_STDOUT}\n${UNCULLED_STDERR}")
  foreach(FIELD coveredPixels outputHash)
    string(REGEX MATCH "${FIELD}=([0-9a-fA-F]+)" CULLED_MATCH "${CULLED_LOG}")
    set(CULLED_VALUE "${CMAKE_MATCH_1}")
    string(REGEX MATCH "${FIELD}=([0-9a-fA-F]+)" UNCULLED_MATCH "${UNCULLED_LOG}")
    set(UNCULLED_VALUE "${CMAKE_MATCH_1}")
    if(CULLED_VALUE STREQUAL "" OR UNCULLED_VALUE STREQUAL "")
      message(FATAL_ERROR "Could not parse ${FIELD} at frame ${FRAME_COUNT}")
    endif()
    if(NOT CULLED_VALUE STREQUAL UNCULLED_VALUE)
      message(FATAL_ERROR "BLAS frustum changed ${FIELD} at frame ${FRAME_COUNT}: ${CULLED_VALUE} vs ${UNCULLED_VALUE}")
    endif()
  endforeach()
endforeach()

message(STATUS "Stanford bunny BLAS-frustum equivalence passed at four camera angles")
