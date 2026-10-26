if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

# A one-frame lighting window is a diagnostic mode for the current sparse
# lightfield write. It must not silently override the requested 2x2 election
# into full-rate tracing; temporal accumulation and spatial reconstruction are
# separate stages that own filling missing phases in normal rendering.
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
    --frames 445
    --debug-mode 14
	    --disable-svgf
	    --lighting-accumulation-frames 1
	    --ray-resolution-divisor 2
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
    "Captured no-accumulation motion run failed (${RESULT})\n${OUTPUT}\n${ERROR}")
endif()
set(LOG "${OUTPUT}\n${ERROR}")

string(REGEX MATCH
  "SVR terrain trace: frame=360[^\r\n]*cameraLOD=0[^\r\n]*pendingChunks=0 queuedRequests=0[^\r\n]*jobPending=0 uploadPending=0"
  SETTLED_LOD0 "${LOG}")
if(NOT SETTLED_LOD0)
  message(FATAL_ERROR
    "Captured no-accumulation run did not settle and freeze at LOD0")
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
          "One-frame lighting opened terrain coverage holes at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR BLACKOUT_LIMIT "${TERRAIN_PIXELS} * 9 / 10")
      if(NEAR_BLACK GREATER BLACKOUT_LIMIT)
        message(FATAL_ERROR
          "One-frame lighting turned covered terrain mostly black at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR MOTION_HOLE_FRAMES "${MOTION_HOLE_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(MOTION_HOLE_FRAMES LESS 25)
  message(FATAL_ERROR
    "Captured no-accumulation run emitted only ${MOTION_HOLE_FRAMES} per-frame terrain image checks")
endif()

string(REGEX MATCHALL "Sparse voxel scene ray stats: frame=[^\r\n]*"
  RAY_LINES "${LOG}")
set(CHECKED_FRAMES 0)
set(MIN_RAYS 2147483647)
set(MAX_RAY_PERCENT 0)
set(MIN_RAY_PERCENT 2147483647)
foreach(LINE IN LISTS RAY_LINES)
  if(LINE MATCHES
      "frame=([0-9]+) rays=([0-9]+).*invalidRays=([0-9]+).*scheduled=([0-9]+) coveredPixels=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 420)
      set(RAYS "${CMAKE_MATCH_2}")
      set(INVALID_RAYS "${CMAKE_MATCH_3}")
      set(SCHEDULED "${CMAKE_MATCH_4}")
      set(COVERED "${CMAKE_MATCH_5}")
	      if(COVERED LESS 750000 OR NOT SCHEDULED EQUAL RAYS OR
	         NOT INVALID_RAYS EQUAL 0)
	        message(FATAL_ERROR
	          "One-frame lighting violated sparse-window scheduling at frame ${FRAME}: ${LINE}")
	      endif()
	      math(EXPR RAY_PERCENT "${RAYS} * 1000 / ${COVERED}")
	      if(RAY_PERCENT LESS 180 OR RAY_PERCENT GREATER 340)
	        message(FATAL_ERROR
	          "One-frame 2x2 lighting did not stay near quarter-rate at frame "
	          "${FRAME}: percentPermille=${RAY_PERCENT}, ${LINE}")
	      endif()
	      if(RAYS LESS MIN_RAYS)
	        set(MIN_RAYS "${RAYS}")
	      endif()
	      if(RAY_PERCENT LESS MIN_RAY_PERCENT)
	        set(MIN_RAY_PERCENT "${RAY_PERCENT}")
	      endif()
	      if(RAY_PERCENT GREATER MAX_RAY_PERCENT)
	        set(MAX_RAY_PERCENT "${RAY_PERCENT}")
	      endif()
      math(EXPR CHECKED_FRAMES "${CHECKED_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(CHECKED_FRAMES LESS 25)
  message(FATAL_ERROR
    "Captured no-accumulation run checked only ${CHECKED_FRAMES} motion frames")
endif()

message(STATUS
  "Captured no-accumulation motion regression passed: "
  "frames=${CHECKED_FRAMES}, minSparseRays=${MIN_RAYS}, "
  "rayPermilleRange=${MIN_RAY_PERCENT}..${MAX_RAY_PERCENT}")
