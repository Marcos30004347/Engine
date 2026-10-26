if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

# Captured at ground level in the generated world. This view deliberately
# combines three previous blind spots: planet-scale coordinates, LOD0 walls at
# grazing angles, and enough depth complexity for HZB brick rejection.
set(CAPTURE_ARGS
  --world-map "${WORLD_MAP}"
  --world-map-center 0.56901041666666663,0.35546875
  --terrain-render-distance-km 16
  --gbuffer-resolver bricks
  --resolution 1220x720
  --render-scale 0.5
  --camera-position 124363.422,884.386169,-260148.656
  --camera-rotation "-0.183444023,-0.722854972,-0.209702089,0.632340193"
  --camera-near 0.00100000005
  --camera-far 25000
  --test-frame-signatures
  --test-capture-start-frame 400
  --test-terrain-freeze-frame 360
  --no-taa
  --disable-metalfx-upscaler
  --disable-frame-generation
  --disable-vsync)

function(run_capture NAME)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env SVR_TRACE_TERRAIN=1
      "${SAMPLE}" ${CAPTURE_ARGS} ${ARGN}
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR)
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR
      "${NAME} captured terrain run failed (${RESULT})\n${OUTPUT}\n${ERROR}")
  endif()
  set(${NAME}_LOG "${OUTPUT}\n${ERROR}" PARENT_SCOPE)
endfunction()

function(validate_no_streaming_publication_failure LOG PREFIX)
  if(LOG MATCHES
      "Terrain GPU publication rolled back|GPU terrain runtime cut references visible pages without matching committed raster/Tree64 descriptors|full-GPU terrain ownership transaction would publish a runtime cut with unbacked pages|a full-GPU terrain ownership transaction is already staged")
    message(FATAL_ERROR
      "${PREFIX} hit a terrain hierarchy publication failure:\n${LOG}")
  endif()
endfunction()

function(last_signature LOG PREFIX)
  string(REGEX MATCHALL
    "Sparse voxel frame signature: frame=[0-9]+ coveredPixels=[0-9]+ outputHash=[0-9a-fA-F]+"
    SIGNATURES "${LOG}")
  if(NOT SIGNATURES)
    message(FATAL_ERROR "${PREFIX} emitted no frame signatures")
  endif()
  list(GET SIGNATURES -1 LAST)
  if(NOT LAST MATCHES
      "frame=([0-9]+) coveredPixels=([0-9]+) outputHash=([0-9a-fA-F]+)")
    message(FATAL_ERROR "Could not parse ${PREFIX} signature: ${LAST}")
  endif()
  set(${PREFIX}_FRAME "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${PREFIX}_COVERAGE "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${PREFIX}_HASH "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()

function(last_final_output_hash LOG PREFIX)
  string(REGEX MATCHALL
    "Sparse voxel exact output: frame=[0-9]+[^\r\n]*finalOutputHash=[0-9a-fA-F]+"
    OUTPUTS "${LOG}")
  if(NOT OUTPUTS)
    message(FATAL_ERROR "${PREFIX} emitted no exact final-output signatures")
  endif()
  list(GET OUTPUTS -1 LAST)
  if(NOT LAST MATCHES "frame=([0-9]+).*finalOutputHash=([0-9a-fA-F]+)")
    message(FATAL_ERROR "Could not parse ${PREFIX} final output: ${LAST}")
  endif()
  set(${PREFIX}_FINAL_FRAME "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${PREFIX}_FINAL_HASH "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# HZB is allowed to reject work, but not a single final pixel. Keep lighting
# out of this pair so stochastic visibility cannot hide a geometry mismatch.
run_capture(CULL
  --frames 440 --disable-lighting --debug-mode 0 --brick-measure-area)
run_capture(REFERENCE
  --frames 440 --disable-lighting --debug-mode 0
  --test-disable-culling-frame 360 --brick-measure-area)
validate_no_streaming_publication_failure("${CULL_LOG}" CULL)
validate_no_streaming_publication_failure("${REFERENCE_LOG}" REFERENCE)
last_signature("${CULL_LOG}" CULL)
last_signature("${REFERENCE_LOG}" REFERENCE)
if(NOT CULL_FRAME EQUAL 439 OR NOT REFERENCE_FRAME EQUAL 439)
  message(FATAL_ERROR
    "Captured culling pair did not reach the settled frame: "
    "${CULL_FRAME}/${REFERENCE_FRAME}")
endif()
if(NOT CULL_COVERAGE STREQUAL REFERENCE_COVERAGE OR
   NOT CULL_HASH STREQUAL REFERENCE_HASH)
  message(FATAL_ERROR
    "Captured HZB/frustum culling removed visible terrain: "
    "enabled=${CULL_COVERAGE}/${CULL_HASH}, "
    "disabled=${REFERENCE_COVERAGE}/${REFERENCE_HASH}")
endif()
function(validate_settled_lod LOG PREFIX)
  string(REGEX MATCHALL "SVR terrain trace: frame=[^\r\n]*" TRACE_LINES "${LOG}")
  set(SETTLED_LINES 0)
  set(EXPECTED_GENERATION "")
  set(EXPECTED_CUT "")
  foreach(LINE IN LISTS TRACE_LINES)
    if(LINE MATCHES
        "frame=([0-9]+) terrainGen=([0-9]+).*lodCut\\(count=([0-9]+) xor=([0-9]+) sum=([0-9]+) transitions=([0-9]+)\\).*cameraLOD=([0-9]+).*pendingChunks=([0-9]+) queuedRequests=([0-9]+).*jobPending=([0-9]+) uploadPending=([0-9]+)")
      set(FRAME "${CMAKE_MATCH_1}")
      if(FRAME GREATER_EQUAL 360)
        set(GENERATION "${CMAKE_MATCH_2}")
        set(CUT "${CMAKE_MATCH_3}/${CMAKE_MATCH_4}/${CMAKE_MATCH_5}")
      if(NOT CMAKE_MATCH_6 EQUAL 0 OR NOT CMAKE_MATCH_7 EQUAL 0 OR
         NOT LINE MATCHES
           "pendingChunks=0 queuedRequests=0.*jobPending=0 uploadPending=0")
          message(FATAL_ERROR
            "${PREFIX} collected before the camera LOD0 cut settled: ${LINE}")
        endif()
        if(EXPECTED_GENERATION STREQUAL "")
          set(EXPECTED_GENERATION "${GENERATION}")
          set(EXPECTED_CUT "${CUT}")
        elseif(NOT GENERATION STREQUAL EXPECTED_GENERATION OR
               NOT CUT STREQUAL EXPECTED_CUT)
          message(FATAL_ERROR
            "${PREFIX} terrain cut changed during capture: ${LINE}")
        endif()
        math(EXPR SETTLED_LINES "${SETTLED_LINES} + 1")
      endif()
    endif()
  endforeach()
  if(SETTLED_LINES LESS 2)
    message(FATAL_ERROR
      "${PREFIX} emitted only ${SETTLED_LINES} settled LOD0 trace samples")
  endif()
endfunction()

validate_settled_lod("${CULL_LOG}" CULL)
validate_settled_lod("${REFERENCE_LOG}" REFERENCE)
string(REGEX MATCHALL "Sparse voxel brick cull:[^\r\n]*" CULL_LINES "${CULL_LOG}")
if(NOT CULL_LINES)
  message(FATAL_ERROR "Captured culling run emitted no brick statistics")
endif()
list(GET CULL_LINES -1 CULL_LINE)
if(NOT CULL_LINE MATCHES "visible=([0-9]+) occluded=([0-9]+)")
  message(FATAL_ERROR "Could not parse captured brick cull: ${CULL_LINE}")
endif()
set(CULL_VISIBLE "${CMAKE_MATCH_1}")
set(CULL_OCCLUDED "${CMAKE_MATCH_2}")
if(CULL_OCCLUDED LESS 1)
  message(FATAL_ERROR "Captured fixture did not exercise HZB brick rejection")
endif()

# Exercise the light leak with a deliberately long history and no spatial
# denoiser. Screen hits are confirmed by Tree64 in the same run; a large count
# of screen-hit/world-miss rays is the exact signature of the tiny bright
# windows that used to shine through solid LOD0 walls.
run_capture(SCREEN
  --frames 460 --debug-mode 14 --disable-svgf
  --lighting-accumulation-frames 128 --screen-trace-statistics)
run_capture(WORLD
  --frames 460 --debug-mode 14 --disable-svgf
  --lighting-accumulation-frames 128 --disable-screen-traces)
validate_no_streaming_publication_failure("${SCREEN_LOG}" SCREEN)
validate_no_streaming_publication_failure("${WORLD_LOG}" WORLD)
validate_settled_lod("${SCREEN_LOG}" SCREEN)
validate_settled_lod("${WORLD_LOG}" WORLD)
last_signature("${SCREEN_LOG}" SCREEN)
last_signature("${WORLD_LOG}" WORLD)
last_final_output_hash("${SCREEN_LOG}" SCREEN)
last_final_output_hash("${WORLD_LOG}" WORLD)
if(NOT SCREEN_COVERAGE STREQUAL WORLD_COVERAGE)
  message(FATAL_ERROR
    "Captured screen/world runs disagree on settled lighting coverage: "
    "screen=${SCREEN_COVERAGE}, world=${WORLD_COVERAGE}")
endif()
if(NOT SCREEN_FINAL_HASH STREQUAL WORLD_FINAL_HASH)
  message(FATAL_ERROR
    "Captured screen/world runs disagree on final lighting: "
    "screen=${SCREEN_FINAL_HASH}, world=${WORLD_FINAL_HASH}")
endif()

function(validate_lighting LOG PREFIX SCREEN_ENABLED)
  string(REGEX MATCHALL "Sparse voxel scene ray stats: frame=[^\r\n]*"
    LINES "${LOG}")
  set(SETTLED 0)
  set(MAX_FALSE_SCREEN 0)
  foreach(LINE IN LISTS LINES)
    if(LINE MATCHES
        "frame=([0-9]+) rays=([0-9]+).*screenHits=([0-9]+).*worldHits=([0-9]+).*worldMisses=([0-9]+).*unresolved=([0-9]+).*queryLimit=([0-9]+).*screenHitWorldMiss=([0-9]+).*")
      set(FRAME "${CMAKE_MATCH_1}")
      set(RAYS "${CMAKE_MATCH_2}")
      set(SCREEN_HITS "${CMAKE_MATCH_3}")
      set(WORLD_HITS "${CMAKE_MATCH_4}")
      set(QUERY_LIMIT "${CMAKE_MATCH_7}")
      set(FALSE_SCREEN "${CMAKE_MATCH_8}")
      if(FRAME GREATER_EQUAL 420)
        if(WORLD_HITS LESS 1 OR NOT QUERY_LIMIT EQUAL 0)
          message(FATAL_ERROR
            "${PREFIX} has invalid settled Tree64 visibility at frame ${FRAME}: ${LINE}")
        endif()
        if(SCREEN_ENABLED AND SCREEN_HITS GREATER 0)
          # World confirmation owns visibility. Bound the remaining geometric
          # plane disagreement against all traced rays; the old synthetic
          # edge slab is prohibited separately below.
          math(EXPR FALSE_PERCENT_BOUND "${RAYS}")
          math(EXPR FALSE_PERCENT_SCALED "${FALSE_SCREEN} * 100")
          if(FALSE_PERCENT_SCALED GREATER FALSE_PERCENT_BOUND)
            message(FATAL_ERROR
              "${PREFIX} reopened sub-voxel light windows at frame ${FRAME}: "
              "screenHits=${SCREEN_HITS}, screenHitWorldMiss=${FALSE_SCREEN}")
          endif()
        endif()
        set(MAX_FALSE_SCREEN "${FALSE_SCREEN}")
        math(EXPR SETTLED "${SETTLED} + 1")
      endif()
    endif()
  endforeach()
  if(SCREEN_ENABLED)
    string(REGEX MATCHALL
      "Sparse voxel screen trace stats: frame=[^\r\n]*" SCREEN_LINES "${LOG}")
    set(SCREEN_SETTLED 0)
    foreach(SCREEN_LINE IN LISTS SCREEN_LINES)
      if(SCREEN_LINE MATCHES
          "frame=([0-9]+).*hits=([0-9]+).*planeHits=([0-9]+).*slabHits=([0-9]+)")
        if(CMAKE_MATCH_1 GREATER_EQUAL 420)
          if(NOT CMAKE_MATCH_2 EQUAL CMAKE_MATCH_3 OR
             NOT CMAKE_MATCH_4 EQUAL 0)
            message(FATAL_ERROR
              "${PREFIX} emitted a non-geometric screen slab hit: ${SCREEN_LINE}")
          endif()
          math(EXPR SCREEN_SETTLED "${SCREEN_SETTLED} + 1")
        endif()
      endif()
    endforeach()
    if(SCREEN_SETTLED LESS 20)
      message(FATAL_ERROR
        "${PREFIX} emitted only ${SCREEN_SETTLED} settled screen-trace frames")
    endif()
  endif()
  if(SETTLED LESS 20)
    message(FATAL_ERROR
      "${PREFIX} emitted only ${SETTLED} settled lighting frames")
  endif()
  set(${PREFIX}_FALSE_SCREEN "${MAX_FALSE_SCREEN}" PARENT_SCOPE)
endfunction()

validate_lighting("${SCREEN_LOG}" SCREEN TRUE)
validate_lighting("${WORLD_LOG}" WORLD FALSE)

message(STATUS
  "Captured LOD0 regression passed: coverage=${CULL_COVERAGE}, "
  "visible/occluded=${CULL_VISIBLE}/${CULL_OCCLUDED}, "
  "last rejected screen-only hits=${SCREEN_FALSE_SCREEN}")
