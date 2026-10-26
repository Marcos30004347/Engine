# The GPU-generated terrain virtual-geometry route, end to end on the slope
# test: pages are counted, reserved exactly, emitted and installed on the GPU,
# the cut refines under the pixel budget, no level draws twice, and the
# terrain never opens a hole.
if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env SVR_TRACE_TERRAIN=1
    "${SAMPLE}"
    --world-map "${WORLD_MAP}"
    --terrain-render-distance-km 1
    --frames 700
    --resolution 640x360
    --test-world-terrain-slope
    --lod-threshold 1.5
    --no-taa
    --jitter-scale 0
    --disable-metalfx-upscaler
    --disable-frame-generation
    --disable-vsync
    --terrain-generation gpu-v2
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE STDOUT
  ERROR_VARIABLE STDERR
)
set(LOG "${STDOUT}\n${STDERR}")
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "Terrain GPU VG contract run failed:\n${LOG}")
endif()
string(REGEX MATCHALL "Terrain generated-VG page resident:" RESIDENT_LINES "${LOG}")
list(LENGTH RESIDENT_LINES RESIDENT_COUNT)
if(RESIDENT_COUNT LESS 40)
  message(FATAL_ERROR
    "Terrain GPU VG generated too few pages (${RESIDENT_COUNT} installs over 700 frames)")
endif()
foreach(BAD "Terrain generated-VG reservation failed" "Terrain generated-VG page rejected"
        "Terrain generated-VG page count rejected" "Terrain generated-VG payload enqueue failed"
        "Terrain GPU VG page set is full")
  string(FIND "${LOG}" "${BAD}" BAD_OFFSET)
  if(NOT BAD_OFFSET EQUAL -1)
    message(FATAL_ERROR "Terrain GPU VG contract hit '${BAD}':\n${LOG}")
  endif()
endforeach()
string(REGEX MATCHALL
  "Sparse voxel terrain VG cut contract: cutClusters=([0-9]+) maxTriPx=([0-9.]+|inf) giants=([0-9]+) giantStreamingLeaves=([0-9]+) giantTrueLeaves=([0-9]+) giantRefinable=(-?[0-9]+)"
  CONTRACT_LINES "${LOG}")
if(NOT CONTRACT_LINES)
  message(FATAL_ERROR "Terrain VG cut contract line was not emitted:\n${LOG}")
endif()
list(GET CONTRACT_LINES -1 CONTRACT_LINE)
string(REGEX MATCH
  "cutClusters=([0-9]+) maxTriPx=([0-9.]+|inf) giants=([0-9]+) giantStreamingLeaves=([0-9]+) giantTrueLeaves=([0-9]+) giantRefinable=(-?[0-9]+)"
  CONTRACT_MATCH "${CONTRACT_LINE}")
set(CUT_CLUSTERS "${CMAKE_MATCH_1}")
set(MAX_TRI_PX "${CMAKE_MATCH_2}")
set(GIANT_REFINABLE "${CMAKE_MATCH_6}")
if(NOT GIANT_REFINABLE EQUAL 0)
  message(FATAL_ERROR
    "Terrain GPU VG cut drew refinable clusters oversized (giantRefinable=${GIANT_REFINABLE}): ${CONTRACT_LINE}")
endif()
if(CUT_CLUSTERS LESS 2000)
  message(FATAL_ERROR
    "Terrain GPU VG cut did not refine under the pixel budget (cutClusters=${CUT_CLUSTERS}): ${CONTRACT_LINE}")
endif()
if(MAX_TRI_PX STREQUAL "inf" OR MAX_TRI_PX GREATER 64)
  message(FATAL_ERROR
    "Terrain GPU VG cut error is not converging (maxTriPx=${MAX_TRI_PX}): ${CONTRACT_LINE}")
endif()
# Coverage: no enclosed hole on any frame, seams stay at pinhole level.
string(REGEX MATCHALL "frame signature: frame=([0-9]+) coveredPixels=([0-9]+) holes=([0-9]+) enclosed=([0-9]+)" SIGNATURES "${LOG}")
set(MAX_ENCLOSED 0)
set(MAX_HOLES 0)
set(LAST_COVERED 0)
foreach(SIGNATURE ${SIGNATURES})
  string(REGEX MATCH "holes=([0-9]+) enclosed=([0-9]+)" S "${SIGNATURE}")
  if(CMAKE_MATCH_1 GREATER MAX_HOLES)
    set(MAX_HOLES ${CMAKE_MATCH_1})
  endif()
  if(CMAKE_MATCH_2 GREATER MAX_ENCLOSED)
    set(MAX_ENCLOSED ${CMAKE_MATCH_2})
  endif()
  string(REGEX MATCH "coveredPixels=([0-9]+)" C "${SIGNATURE}")
  set(LAST_COVERED ${CMAKE_MATCH_1})
endforeach()
if(MAX_ENCLOSED GREATER 0)
  message(FATAL_ERROR "Terrain GPU VG opened an enclosed hole (max ${MAX_ENCLOSED} pixels)")
endif()
if(MAX_HOLES GREATER 256)
  message(FATAL_ERROR "Terrain GPU VG seams exceed pinhole level (max ${MAX_HOLES} pixels)")
endif()
if(LAST_COVERED LESS 100000)
  message(FATAL_ERROR "Terrain GPU VG final coverage is too low (${LAST_COVERED} pixels)")
endif()
message(STATUS
  "Terrain GPU VG contract: installs=${RESIDENT_COUNT} cutClusters=${CUT_CLUSTERS} maxTriPx=${MAX_TRI_PX} "
  "maxHoles=${MAX_HOLES} maxEnclosed=${MAX_ENCLOSED} coverage=${LAST_COVERED}")
