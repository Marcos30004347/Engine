if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

# Build a long, denoiser-independent lighting history in the captured LOD0
# terrain view, then apply a small continuous yaw.  This reproduces the case
# where camera motion used to clear or orphan nearly all temporal evidence.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    SVR_TRACE_TERRAIN=1
    SVR_TRACE_TERRAIN_HOLES=1
    "${SAMPLE}"
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
    --frames 500
    --debug-mode 14
    --disable-svgf
    --lighting-accumulation-frames 128
    --test-frame-signatures
    --test-capture-start-frame 400
    --test-terrain-freeze-frame 360
    --screen-trace-statistics
    --no-taa
    --disable-metalfx-upscaler
    --disable-frame-generation
    --disable-vsync
    --test-camera-ground-forward
    --test-camera-travel-m 0
    --test-camera-yaw-rate 0.05
    --test-camera-yaw-start-frame 419
    --test-camera-yaw-frames 60
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE OUTPUT
  ERROR_VARIABLE ERROR)
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR
    "Captured temporal-motion run failed (${RESULT})\n${OUTPUT}\n${ERROR}")
endif()
set(LOG "${OUTPUT}\n${ERROR}")

# Do not start the yaw while feedback is still replacing coarse terrain. The
# captured camera owner must be LOD0 and the complete published cut must remain
# unchanged for the two periodic samples immediately before motion.
string(REGEX MATCHALL "SVR terrain trace: frame=[^\r\n]*" TERRAIN_LINES "${LOG}")
set(SETTLED_TERRAIN_LINES 0)
set(SETTLED_GENERATION "")
set(SETTLED_CUT "")
foreach(LINE IN LISTS TERRAIN_LINES)
  if(LINE MATCHES
      "frame=([0-9]+) terrainGen=([0-9]+).*lodCut\\(count=([0-9]+) xor=([0-9]+) sum=([0-9]+) transitions=([0-9]+)\\).*cameraLOD=([0-9]+).*pendingChunks=([0-9]+) queuedRequests=([0-9]+).*jobPending=([0-9]+) uploadPending=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 360 AND FRAME LESS 400)
      set(GENERATION "${CMAKE_MATCH_2}")
      set(CUT "${CMAKE_MATCH_3}/${CMAKE_MATCH_4}/${CMAKE_MATCH_5}")
      if(NOT CMAKE_MATCH_6 EQUAL 0 OR NOT CMAKE_MATCH_7 EQUAL 0 OR
         NOT LINE MATCHES
           "pendingChunks=0 queuedRequests=0.*jobPending=0 uploadPending=0")
        message(FATAL_ERROR
          "Captured yaw began before the LOD0 terrain cut settled: ${LINE}")
      endif()
      if(SETTLED_GENERATION STREQUAL "")
        set(SETTLED_GENERATION "${GENERATION}")
        set(SETTLED_CUT "${CUT}")
      elseif(NOT GENERATION STREQUAL SETTLED_GENERATION OR
             NOT CUT STREQUAL SETTLED_CUT)
        message(FATAL_ERROR
          "Captured terrain cut changed before yaw: ${LINE}")
      endif()
      math(EXPR SETTLED_TERRAIN_LINES "${SETTLED_TERRAIN_LINES} + 1")
    endif()
  endif()
endforeach()
if(SETTLED_TERRAIN_LINES LESS 2)
  message(FATAL_ERROR
    "Captured yaw emitted only ${SETTLED_TERRAIN_LINES} settled terrain samples")
endif()

string(REGEX MATCHALL
  "Sparse voxel frame signature: frame=[0-9]+ coveredPixels=[0-9]+ outputHash=[0-9a-fA-F]+"
  SIGNATURES "${LOG}")
if(NOT SIGNATURES)
  message(FATAL_ERROR "Captured temporal-motion run emitted no signatures")
endif()
list(GET SIGNATURES -1 LAST_SIGNATURE)
if(NOT LAST_SIGNATURE MATCHES
    "frame=499 coveredPixels=([0-9]+) outputHash=([0-9a-fA-F]+)")
  message(FATAL_ERROR
    "Captured temporal-motion run did not finish at frame 499: ${LAST_SIGNATURE}")
endif()
set(FINAL_COVERAGE "${CMAKE_MATCH_1}")
if(FINAL_COVERAGE LESS 750000)
  message(FATAL_ERROR
    "Captured yaw lost too much visible terrain: ${FINAL_COVERAGE} pixels")
endif()

string(REGEX MATCHALL "SVR terrain holes: frame=[^\r\n]*" HOLE_LINES "${LOG}")
set(MOTION_HOLE_FRAMES 0)
foreach(LINE IN LISTS HOLE_LINES)
  if(LINE MATCHES
      "frame=([0-9]+) terrainPixels=([0-9]+) nearBlack=([0-9]+).*interiorGapPixels=([0-9]+).*verticalGap=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 420)
      set(TERRAIN_PIXELS "${CMAKE_MATCH_2}")
      set(NEAR_BLACK "${CMAKE_MATCH_3}")
      if(NOT CMAKE_MATCH_4 EQUAL 0 OR NOT CMAKE_MATCH_5 EQUAL 0)
        message(FATAL_ERROR
          "Captured yaw opened terrain coverage holes at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR BLACKOUT_LIMIT "${TERRAIN_PIXELS} * 9 / 10")
      if(NEAR_BLACK GREATER BLACKOUT_LIMIT)
        message(FATAL_ERROR
          "Captured yaw turned covered terrain mostly black at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR MOTION_HOLE_FRAMES "${MOTION_HOLE_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(MOTION_HOLE_FRAMES LESS 60)
  message(FATAL_ERROR
    "Captured yaw emitted only ${MOTION_HOLE_FRAMES} per-frame terrain image checks")
endif()

# The world confirmation remains authoritative during motion. Screen tracing
# may conservatively over-report at a coplanar edge, but it must stay bounded,
# and neither backend may run out of queries or lose every temporal reservoir.
string(REGEX MATCHALL "Sparse voxel scene ray stats: frame=[^\r\n]*"
  RAY_LINES "${LOG}")
set(MOTION_RAY_FRAMES 0)
set(MIN_TEMPORAL_RESERVOIRS 2147483647)
set(MAX_FALSE_SCREEN_PERCENT 0)
foreach(LINE IN LISTS RAY_LINES)
  if(LINE MATCHES
      "frame=([0-9]+) rays=([0-9]+).*screenHits=([0-9]+).*worldHits=([0-9]+).*queryLimit=([0-9]+).*temporalReservoirs=([0-9]+).*invalidRays=([0-9]+).*screenHitWorldMiss=([0-9]+).*")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 420)
      set(RAYS "${CMAKE_MATCH_2}")
      set(SCREEN_HITS "${CMAKE_MATCH_3}")
      set(WORLD_HITS "${CMAKE_MATCH_4}")
      set(QUERY_LIMIT "${CMAKE_MATCH_5}")
      set(TEMPORAL_RESERVOIRS "${CMAKE_MATCH_6}")
      set(INVALID_RAYS "${CMAKE_MATCH_7}")
      set(FALSE_SCREEN "${CMAKE_MATCH_8}")
      if(WORLD_HITS LESS 1 OR NOT QUERY_LIMIT EQUAL 0 OR
         NOT INVALID_RAYS EQUAL 0 OR TEMPORAL_RESERVOIRS LESS 1)
        message(FATAL_ERROR
          "Captured yaw lost valid world/temporal tracing at frame ${FRAME}: ${LINE}")
      endif()
      if(SCREEN_HITS GREATER 0)
        math(EXPR FALSE_SCREEN_SCALED "${FALSE_SCREEN} * 10000 / ${RAYS}")
        if(FALSE_SCREEN_SCALED GREATER MAX_FALSE_SCREEN_PERCENT)
          set(MAX_FALSE_SCREEN_PERCENT "${FALSE_SCREEN_SCALED}")
        endif()
        # Screen hits are always world-confirmed. Once synthetic edge slabs
        # are forbidden, a small number of single-layer plane candidates may
        # still be rejected at silhouettes; bound those against all traced
        # rays rather than dividing by the deliberately small plane-hit set.
        if(FALSE_SCREEN_SCALED GREATER 200)
          message(FATAL_ERROR
            "Captured yaw screen/world edge disagreement exceeded 2% of rays at "
            "frame ${FRAME}: ${LINE}")
        endif()
      endif()
      if(TEMPORAL_RESERVOIRS LESS MIN_TEMPORAL_RESERVOIRS)
        set(MIN_TEMPORAL_RESERVOIRS "${TEMPORAL_RESERVOIRS}")
      endif()
      math(EXPR MOTION_RAY_FRAMES "${MOTION_RAY_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(MOTION_RAY_FRAMES LESS 60)
  message(FATAL_ERROR
    "Captured yaw emitted only ${MOTION_RAY_FRAMES} motion ray frames")
endif()

string(REGEX MATCHALL "Sparse voxel screen trace stats: frame=[^\r\n]*"
  SCREEN_TRACE_LINES "${LOG}")
set(MOTION_SCREEN_TRACE_FRAMES 0)
foreach(LINE IN LISTS SCREEN_TRACE_LINES)
  if(LINE MATCHES "frame=([0-9]+).*hits=([0-9]+).*planeHits=([0-9]+).*slabHits=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 420)
      if(NOT CMAKE_MATCH_2 EQUAL CMAKE_MATCH_3 OR NOT CMAKE_MATCH_4 EQUAL 0)
        message(FATAL_ERROR
          "Captured yaw emitted a non-geometric screen slab hit at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR MOTION_SCREEN_TRACE_FRAMES "${MOTION_SCREEN_TRACE_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(MOTION_SCREEN_TRACE_FRAMES LESS 60)
  message(FATAL_ERROR
    "Captured yaw emitted only ${MOTION_SCREEN_TRACE_FRAMES} screen-trace frames")
endif()

# A camera transform is not a content epoch. The history/generation reset
# counters must stay zero, while both successful reuse and reprojection work
# prove that history was transported rather than silently restarted.
string(REGEX MATCHALL "Sparse voxel hit reuse: frame=[^\r\n]*"
  REUSE_LINES "${LOG}")
set(MOTION_REUSE_FRAMES 0)
set(MIN_REUSED 2147483647)
set(MAX_REPROJECTION_REJECTS 0)
foreach(LINE IN LISTS REUSE_LINES)
  if(LINE MATCHES
      "frame=([0-9]+) reused=([0-9]+) resets=([0-9]+) history=([0-9]+).*generation=([0-9]+).*reprojection=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 420)
      set(REUSED "${CMAKE_MATCH_2}")
      set(HISTORY_RESETS "${CMAKE_MATCH_4}")
      set(GENERATION_RESETS "${CMAKE_MATCH_5}")
      set(REPROJECTION_REJECTS "${CMAKE_MATCH_6}")
      if(NOT HISTORY_RESETS EQUAL 0 OR NOT GENERATION_RESETS EQUAL 0 OR
         REUSED LESS 300000 OR REPROJECTION_REJECTS GREATER 150000)
        message(FATAL_ERROR
          "Captured yaw invalidated temporal history at frame ${FRAME}: ${LINE}")
      endif()
      if(REUSED LESS MIN_REUSED)
        set(MIN_REUSED "${REUSED}")
      endif()
      if(REPROJECTION_REJECTS GREATER MAX_REPROJECTION_REJECTS)
        set(MAX_REPROJECTION_REJECTS "${REPROJECTION_REJECTS}")
      endif()
      math(EXPR MOTION_REUSE_FRAMES "${MOTION_REUSE_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(MOTION_REUSE_FRAMES LESS 60 OR MAX_REPROJECTION_REJECTS LESS 1)
  message(FATAL_ERROR
    "Captured yaw did not exercise temporal reprojection: "
    "frames=${MOTION_REUSE_FRAMES}, rejects=${MAX_REPROJECTION_REJECTS}")
endif()

message(STATUS
  "Captured temporal-motion regression passed: coverage=${FINAL_COVERAGE}, "
  "minTemporalReservoirs=${MIN_TEMPORAL_RESERVOIRS}, minReused=${MIN_REUSED}, "
  "maxScreenWorldDisagreementPermyriad=${MAX_FALSE_SCREEN_PERCENT}")
