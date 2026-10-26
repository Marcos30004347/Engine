if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

execute_process(
  COMMAND "${SAMPLE}"
    --world-map "${WORLD_MAP}"
    --world-map-center 0.5,0.5
    --terrain-render-distance-km 16
    --gbuffer-resolver bricks
    --test-terrain-streaming
    --test-frame-signatures
    --frames 120
    --resolution 320x180
    --render-scale 1
    --no-taa
    --disable-metalfx-upscaler
    --disable-frame-generation
    --disable-vsync
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE STDOUT
  ERROR_VARIABLE STDERR
)
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR
    "Light-field page-churn instrumentation run failed:\n${STDOUT}\n${STDERR}")
endif()

set(LOG "${STDOUT}\n${STDERR}")
string(REGEX MATCHALL
  "Sparse voxel light field churn:[^\r\n]*"
  CHURN_LINES "${LOG}")
list(LENGTH CHURN_LINES CHURN_LINE_COUNT)
if(CHURN_LINE_COUNT LESS 100)
  message(FATAL_ERROR
    "Expected per-frame light-field churn telemetry, got ${CHURN_LINE_COUNT} lines")
endif()

function(read_churn_field LINE FIELD OUTPUT)
  string(REGEX MATCH "${FIELD}=([0-9]+)" FIELD_MATCH "${LINE}")
  if(FIELD_MATCH STREQUAL "")
    message(FATAL_ERROR "Churn line is missing '${FIELD}': ${LINE}")
  endif()
  set(${OUTPUT} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

set(COMMIT_FRAME_COUNT 0)
set(NON_COMMIT_FRAME_COUNT 0)
set(COMMIT_WITH_CHURN_COUNT 0)
set(VARIANCE_MEASURED_COUNT 0)
foreach(LINE IN LISTS CHURN_LINES)
  foreach(FIELD IN ITEMS
      commit generation built refined coarsened evicted attempted lost
      halfStopPopped varianceSettled variancePopped commitSettled commitPopped)
    read_churn_field("${LINE}" "${FIELD}" "${FIELD}")
  endforeach()

  if(lost GREATER attempted)
    message(FATAL_ERROR "History loss exceeds attempts: ${LINE}")
  endif()
  if(variancePopped GREATER varianceSettled)
    message(FATAL_ERROR "Variance pops exceed considered pixels: ${LINE}")
  endif()
  if(commitPopped GREATER commitSettled)
    message(FATAL_ERROR "Commit pops exceed considered pixels: ${LINE}")
  endif()
  if(varianceSettled GREATER 0)
    math(EXPR VARIANCE_MEASURED_COUNT "${VARIANCE_MEASURED_COUNT} + 1")
  endif()

  if(commit EQUAL 1)
    math(EXPR COMMIT_FRAME_COUNT "${COMMIT_FRAME_COUNT} + 1")
    if(NOT commitSettled EQUAL varianceSettled OR
       NOT commitPopped EQUAL variancePopped)
      message(FATAL_ERROR
        "CPU commit snapshot disagrees with GPU commit counters: ${LINE}")
    endif()
    math(EXPR PAGE_CHURN "${built} + ${refined} + ${coarsened} + ${evicted}")
    if(PAGE_CHURN GREATER 0)
      math(EXPR COMMIT_WITH_CHURN_COUNT "${COMMIT_WITH_CHURN_COUNT} + 1")
    endif()
  elseif(commit EQUAL 0)
    math(EXPR NON_COMMIT_FRAME_COUNT "${NON_COMMIT_FRAME_COUNT} + 1")
    if(NOT commitSettled EQUAL 0 OR NOT commitPopped EQUAL 0)
      message(FATAL_ERROR
        "Commit-only GPU counters were nonzero outside a commit: ${LINE}")
    endif()
    if(NOT built EQUAL 0 OR NOT refined EQUAL 0 OR
       NOT coarsened EQUAL 0 OR NOT evicted EQUAL 0)
      message(FATAL_ERROR
        "Page churn leaked outside its captured commit window: ${LINE}")
    endif()
  else()
    message(FATAL_ERROR "Commit flag is not binary: ${LINE}")
  endif()
endforeach()

if(COMMIT_FRAME_COUNT EQUAL 0 OR NON_COMMIT_FRAME_COUNT EQUAL 0)
  message(FATAL_ERROR
    "Instrumentation did not cover both commit and non-commit frames: "
    "commit=${COMMIT_FRAME_COUNT}, nonCommit=${NON_COMMIT_FRAME_COUNT}")
endif()
if(COMMIT_WITH_CHURN_COUNT EQUAL 0)
  message(FATAL_ERROR
    "No terrain commit carried built/refined/coarsened/evicted page counts")
endif()
if(VARIANCE_MEASURED_COUNT EQUAL 0)
  message(FATAL_ERROR
    "Variance-normalized light-field metric never observed settled geometry")
endif()

message(STATUS
  "Light-field/page-churn telemetry joined ${CHURN_LINE_COUNT} frames: "
  "commit=${COMMIT_FRAME_COUNT}, commitWithChurn=${COMMIT_WITH_CHURN_COUNT}, "
  "varianceMeasured=${VARIANCE_MEASURED_COUNT}")
