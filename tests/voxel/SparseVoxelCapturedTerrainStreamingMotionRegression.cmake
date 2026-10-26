if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

# Small camera yaw while terrain feedback remains live. This catches the
# intermittent "covered world goes black while moving" class that a frozen LOD0
# reprojection test cannot see.
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
      --frames 720
      --debug-mode 14
      --disable-svgf
      --lighting-accumulation-frames 128
      --test-frame-signatures
      --test-capture-start-frame 240
      --screen-trace-statistics
      --no-taa
      --disable-metalfx-upscaler
      --disable-frame-generation
      --disable-vsync
      --test-camera-ground-forward
      --test-camera-travel-m 0
      --test-camera-yaw-rate 0.05
      --test-camera-yaw-start-frame 300
      --test-camera-yaw-frames 360
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE OUTPUT
  ERROR_VARIABLE ERROR)
set(LOG "${OUTPUT}\n${ERROR}")
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR
    "Captured streaming-motion run failed (${RESULT})\n${LOG}")
endif()

if(LOG MATCHES
    "Terrain GPU publication rolled back|GPU terrain runtime cut references visible pages without matching committed raster/Tree64 descriptors|full-GPU terrain ownership transaction would publish a runtime cut with unbacked pages|a full-GPU terrain ownership transaction is already staged")
  message(FATAL_ERROR
    "Captured streaming motion hit a terrain hierarchy publication failure:\n${LOG}")
endif()

string(REGEX MATCHALL
  "Sparse voxel frame signature: frame=[0-9]+ coveredPixels=[0-9]+ outputHash=[0-9a-fA-F]+"
  SIGNATURES "${LOG}")
if(NOT SIGNATURES)
  message(FATAL_ERROR "Captured streaming-motion run emitted no signatures")
endif()

set(CHECKED_SIGNATURES 0)
foreach(LINE IN LISTS SIGNATURES)
  if(LINE MATCHES "frame=([0-9]+) coveredPixels=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 420)
      if(CMAKE_MATCH_2 LESS 850000)
        message(FATAL_ERROR
          "Captured streaming motion lost visible terrain at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR CHECKED_SIGNATURES "${CHECKED_SIGNATURES} + 1")
    endif()
  endif()
endforeach()
if(CHECKED_SIGNATURES LESS 120)
  message(FATAL_ERROR
    "Captured streaming motion checked only ${CHECKED_SIGNATURES} settled signatures")
endif()

string(REGEX MATCHALL "SVR terrain holes: frame=[^\r\n]*" HOLE_LINES "${LOG}")
set(CHECKED_IMAGE_FRAMES 0)
set(MAX_NEAR_BLACK_PERMILLE 0)
foreach(LINE IN LISTS HOLE_LINES)
  if(LINE MATCHES
      "frame=([0-9]+) terrainPixels=([0-9]+) nearBlack=([0-9]+).*interiorGapPixels=([0-9]+).*verticalGap=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 420)
      set(TERRAIN_PIXELS "${CMAKE_MATCH_2}")
      set(NEAR_BLACK "${CMAKE_MATCH_3}")
      if(NOT CMAKE_MATCH_4 EQUAL 0 OR NOT CMAKE_MATCH_5 EQUAL 0)
        message(FATAL_ERROR
          "Captured streaming motion opened terrain coverage holes at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR NEAR_BLACK_PERMILLE "${NEAR_BLACK} * 1000 / ${TERRAIN_PIXELS}")
      if(NEAR_BLACK_PERMILLE GREATER MAX_NEAR_BLACK_PERMILLE)
        set(MAX_NEAR_BLACK_PERMILLE "${NEAR_BLACK_PERMILLE}")
      endif()
      if(NEAR_BLACK_PERMILLE GREATER 900)
        message(FATAL_ERROR
          "Captured streaming motion turned covered terrain mostly black at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR CHECKED_IMAGE_FRAMES "${CHECKED_IMAGE_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(CHECKED_IMAGE_FRAMES LESS 120)
  message(FATAL_ERROR
    "Captured streaming motion emitted only ${CHECKED_IMAGE_FRAMES} terrain image checks")
endif()

string(REGEX MATCHALL "Sparse voxel scene ray stats: frame=[^\r\n]*"
  RAY_LINES "${LOG}")
set(CHECKED_RAY_FRAMES 0)
foreach(LINE IN LISTS RAY_LINES)
  if(LINE MATCHES
      "frame=([0-9]+) rays=([0-9]+).*worldHits=([0-9]+).*unknownResidency=([0-9]+) queryLimit=([0-9]+).*temporalReservoirs=([0-9]+).*invalidRays=([0-9]+).*scheduled=([0-9]+) coveredPixels=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 420)
      if(CMAKE_MATCH_3 LESS 1 OR NOT CMAKE_MATCH_4 EQUAL 0 OR
         NOT CMAKE_MATCH_5 EQUAL 0 OR CMAKE_MATCH_6 LESS 1 OR
         NOT CMAKE_MATCH_7 EQUAL 0 OR NOT CMAKE_MATCH_8 EQUAL CMAKE_MATCH_2 OR
         CMAKE_MATCH_9 LESS 850000)
        message(FATAL_ERROR
          "Captured streaming motion lost valid lighting work at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR CHECKED_RAY_FRAMES "${CHECKED_RAY_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(CHECKED_RAY_FRAMES LESS 120)
  message(FATAL_ERROR
    "Captured streaming motion checked only ${CHECKED_RAY_FRAMES} ray-stat frames")
endif()

message(STATUS
  "Captured streaming motion regression passed: frames=${CHECKED_IMAGE_FRAMES}, "
  "maxNearBlackPermille=${MAX_NEAR_BLACK_PERMILLE}")
