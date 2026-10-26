if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env SVR_TRACE_TERRAIN=1
    "${SAMPLE}"
    --world-map "${WORLD_MAP}"
    --world-map-center 0.5,0.5
    --terrain-render-distance-km 16
    --gbuffer-resolver bricks
    # The GPU streamer deliberately bounds payload generation per frame. Give
    # the moving 16 km cut enough time to exercise late camera-driven swaps;
    # the old 64-record publication deadlock appeared only after generation 28.
    --frames 720
    # The runtime default is 2 px. At half the previous raster dimensions this
    # preserves the regression's original projected-error cut and settle time.
    --resolution 320x180
    --render-scale 1
    --test-camera-walk
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
    "Moving-camera terrain LOD regression failed:\n${STDOUT}\n${STDERR}")
endif()

set(LOG "${STDOUT}\n${STDERR}")
string(FIND "${LOG}" "atomic publication capacity" CAPACITY_REJECTION)
if(NOT CAPACITY_REJECTION EQUAL -1)
  message(FATAL_ERROR
    "Moving-camera terrain hit the atomic publication ceiling:\n${LOG}")
endif()

string(REGEX MATCHALL
  "SVR terrain trace: frame=(4[89][0-9]|[56][0-9][0-9]|7[01][0-9]) terrainGen=[0-9]+[^\r\n]*"
  FINAL_TRACES "${LOG}")
list(LENGTH FINAL_TRACES FINAL_TRACE_COUNT)
if(FINAL_TRACE_COUNT LESS 4)
  message(FATAL_ERROR
    "Moving-camera terrain did not expose a late refinement window; "
    "found ${FINAL_TRACE_COUNT} final traces")
endif()

set(GENERATIONS "")
set(LOD0_TRACE_COUNT 0)
set(DRAINED_TRACE_COUNT 0)
foreach(TRACE IN LISTS FINAL_TRACES)
  string(REGEX MATCH
    "terrainGen=([0-9]+).*coveredPixels=([0-9]+).*pageStates=[0-9]+.*lod=\\[([0-9]+),([0-9]+)\\].*pendingChunks=([0-9]+)"
    TRACE_STATE "${TRACE}")
  if(TRACE_STATE STREQUAL "")
    message(FATAL_ERROR "Could not parse terrain trace: ${TRACE}")
  endif()
  set(GENERATION "${CMAKE_MATCH_1}")
  set(COVERED_PIXELS "${CMAKE_MATCH_2}")
  set(LOD_MIN "${CMAKE_MATCH_3}")
  set(PENDING "${CMAKE_MATCH_5}")
  list(APPEND GENERATIONS "${GENERATION}")
  if(COVERED_PIXELS LESS 10000)
    message(FATAL_ERROR "Terrain lost moving-camera coverage: ${TRACE}")
  endif()
  if(LOD_MIN EQUAL 0)
    math(EXPR LOD0_TRACE_COUNT "${LOD0_TRACE_COUNT} + 1")
  endif()
  if(PENDING EQUAL 0)
    math(EXPR DRAINED_TRACE_COUNT "${DRAINED_TRACE_COUNT} + 1")
  endif()
endforeach()

list(REMOVE_DUPLICATES GENERATIONS)
list(LENGTH GENERATIONS GENERATION_COUNT)
# Liveness, not churn. A converged cut publishes no further generations, and
# demanding four of them asserted that refinement never finishes - which was
# only true while a parent could be evicted before its replacement children
# existed, so the same ground was rebuilt over and over. With that fixed this
# camera reaches LOD 0 with the plan drained and correctly stops. Accept either
# continued progress or a converged, drained cut; a stalled one satisfies
# neither, because DRAINED_TRACE_COUNT is checked below and LOD 0 residency is
# checked above.
if(GENERATION_COUNT LESS 4 AND NOT DRAINED_TRACE_COUNT EQUAL FINAL_TRACE_COUNT)
  message(FATAL_ERROR
    "Moving-camera terrain refinement neither progressed nor converged in the "
    "late window; observed generations=${GENERATIONS}, drained "
    "${DRAINED_TRACE_COUNT}/${FINAL_TRACE_COUNT} traces")
endif()
if(NOT LOD0_TRACE_COUNT EQUAL FINAL_TRACE_COUNT)
  message(FATAL_ERROR
    "LOD 0 did not remain resident throughout the late moving-camera window: "
    "${LOD0_TRACE_COUNT}/${FINAL_TRACE_COUNT} traces")
endif()
if(DRAINED_TRACE_COUNT EQUAL 0)
  message(FATAL_ERROR
    "Moving-camera terrain never drained its pending chunk plan")
endif()

message(STATUS
  "Moving-camera terrain retained LOD 0 and continued refining: "
  "${GENERATION_COUNT} generations across ${FINAL_TRACE_COUNT} late traces, "
  "drained traces=${DRAINED_TRACE_COUNT}")
