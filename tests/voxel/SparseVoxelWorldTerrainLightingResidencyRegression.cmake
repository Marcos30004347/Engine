if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

# Repro for close-to-terrain movement at 5 km render distance:
#   SparseVoxelRendererSample --world-map world-data --terrain-render-distance-km 5 --resolution 1220x720
#
# The historical failure was not a raster coverage hole. Terrain remained in
# the G-buffer, but a small streaming publication left old Tree64 terrain
# instances visible to lighting after their page-state generation changed.
# Dynamic any-hit rays then became UNKNOWN_RESIDENCY for almost every pixel,
# which presented as huge black regions / whole-screen blackouts while moving.
set(MIN_COVERED_PIXELS 360000)
set(MAX_TINY_VERTICAL_GAP_PIXELS 16)
set(MAX_TINY_VERTICAL_GAP_SPAN_ROWS 2)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    SVR_TRACE_TERRAIN=1
    SVR_TRACE_TERRAIN_HOLES=1
    "${SAMPLE}"
      --world-map "${WORLD_MAP}"
      --terrain-render-distance-km 5
      --gbuffer-resolver bricks
      --resolution 1220x720
      --frames 150
      --debug-mode 14
      --disable-svgf
      --lighting-accumulation-frames 128
      --test-frame-signatures
      --test-capture-start-frame 60
      --screen-trace-statistics
      --no-taa
      --disable-metalfx-upscaler
      --disable-frame-generation
      --disable-vsync
      --test-camera-walk
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE OUTPUT
  ERROR_VARIABLE ERROR)
set(LOG "${OUTPUT}\n${ERROR}")
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR
    "World terrain lighting-residency run failed (${RESULT})\n${LOG}")
endif()

string(REGEX MATCHALL "Sparse voxel scene ray stats: frame=[^\r\n]*"
  RAY_LINES "${LOG}")
set(CHECKED_RAY_FRAMES 0)
set(MAX_UNKNOWN 0)
set(MAX_QUERY_LIMIT 0)
foreach(LINE IN LISTS RAY_LINES)
  if(LINE MATCHES
      "frame=([0-9]+) rays=([0-9]+).*unresolved=([0-9]+).*escapedTreeVolume=([0-9]+) unknownResidency=([0-9]+) queryLimit=([0-9]+).*invalidRays=([0-9]+).*scheduled=([0-9]+) coveredPixels=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 116)
      set(RAYS "${CMAKE_MATCH_2}")
      set(UNKNOWN "${CMAKE_MATCH_5}")
      set(QUERY_LIMIT "${CMAKE_MATCH_6}")
      set(INVALID_RAYS "${CMAKE_MATCH_7}")
      set(SCHEDULED_RAYS "${CMAKE_MATCH_8}")
      set(COVERED_PIXELS "${CMAKE_MATCH_9}")
      if(UNKNOWN GREATER MAX_UNKNOWN)
        set(MAX_UNKNOWN "${UNKNOWN}")
      endif()
      if(QUERY_LIMIT GREATER MAX_QUERY_LIMIT)
        set(MAX_QUERY_LIMIT "${QUERY_LIMIT}")
      endif()
      if(NOT LINE MATCHES
          "tree64InvalidReasons\\(flags=0,dynGen=0,model=0,terrainState=0,descBounds=0,descId=0,nodePage=0,materialPage=0\\)")
        message(FATAL_ERROR
          "World terrain lighting residency reported Tree64 trace invalids at frame ${FRAME}: ${LINE}")
      endif()
      if(NOT UNKNOWN EQUAL 0 OR NOT QUERY_LIMIT EQUAL 0 OR
         INVALID_RAYS GREATER 1 OR NOT SCHEDULED_RAYS EQUAL RAYS OR
         COVERED_PIXELS LESS MIN_COVERED_PIXELS)
        message(FATAL_ERROR
          "World terrain lighting residency regressed at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR CHECKED_RAY_FRAMES "${CHECKED_RAY_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(CHECKED_RAY_FRAMES LESS 30)
  message(FATAL_ERROR
    "World terrain lighting-residency checked only ${CHECKED_RAY_FRAMES} ray-stat frames")
endif()

string(REGEX MATCHALL "SVR terrain holes: frame=[^\r\n]*" HOLE_LINES "${LOG}")
set(CHECKED_IMAGE_FRAMES 0)
set(MAX_NEAR_BLACK_PERMILLE 0)
set(MAX_VERTICAL_GAP_PIXELS 0)
set(MAX_VERTICAL_GAP_SPAN 0)
foreach(LINE IN LISTS HOLE_LINES)
  if(LINE MATCHES
      "frame=([0-9]+) terrainPixels=([0-9]+) nearBlack=([0-9]+).*interiorHoles=([0-9]+).*interiorGapPixels=([0-9]+).*verticalGap=([0-9]+).*maxSpan=([0-9]+)")
    set(FRAME "${CMAKE_MATCH_1}")
    if(FRAME GREATER_EQUAL 116)
      set(TERRAIN_PIXELS "${CMAKE_MATCH_2}")
      set(NEAR_BLACK "${CMAKE_MATCH_3}")
      set(INTERIOR_HOLES "${CMAKE_MATCH_4}")
      set(INTERIOR_GAPS "${CMAKE_MATCH_5}")
      set(VERTICAL_GAPS "${CMAKE_MATCH_6}")
      set(MAX_SPAN "${CMAKE_MATCH_7}")
      if(VERTICAL_GAPS GREATER MAX_VERTICAL_GAP_PIXELS)
        set(MAX_VERTICAL_GAP_PIXELS "${VERTICAL_GAPS}")
      endif()
      if(MAX_SPAN GREATER MAX_VERTICAL_GAP_SPAN)
        set(MAX_VERTICAL_GAP_SPAN "${MAX_SPAN}")
      endif()
      # The moving, lit 1220x720 fixture can expose a handful of one/two-row
      # raster-edge seams while the camera walks over relief. The regression
      # this test guards is a streaming/page publication hole: it opens a
      # broad missing region in the terrain body and is accompanied by broken
      # Tree64 residency/ray counters.
      if(NOT INTERIOR_HOLES EQUAL 0 OR
         MAX_SPAN GREATER MAX_TINY_VERTICAL_GAP_SPAN_ROWS OR
         VERTICAL_GAPS GREATER MAX_TINY_VERTICAL_GAP_PIXELS OR
         NOT INTERIOR_GAPS EQUAL VERTICAL_GAPS)
        message(FATAL_ERROR
          "World terrain lighting residency opened terrain coverage holes at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR NEAR_BLACK_PERMILLE "${NEAR_BLACK} * 1000 / ${TERRAIN_PIXELS}")
      if(NEAR_BLACK_PERMILLE GREATER MAX_NEAR_BLACK_PERMILLE)
        set(MAX_NEAR_BLACK_PERMILLE "${NEAR_BLACK_PERMILLE}")
      endif()
      if(NEAR_BLACK_PERMILLE GREATER 100)
        message(FATAL_ERROR
          "World terrain lighting residency produced excessive near-black terrain at frame ${FRAME}: ${LINE}")
      endif()
      math(EXPR CHECKED_IMAGE_FRAMES "${CHECKED_IMAGE_FRAMES} + 1")
    endif()
  endif()
endforeach()
if(CHECKED_IMAGE_FRAMES LESS 30)
  message(FATAL_ERROR
    "World terrain lighting-residency emitted only ${CHECKED_IMAGE_FRAMES} terrain image checks")
endif()

message(STATUS
  "World terrain lighting-residency regression passed: rayFrames=${CHECKED_RAY_FRAMES}, "
  "imageFrames=${CHECKED_IMAGE_FRAMES}, maxUnknown=${MAX_UNKNOWN}, "
  "maxQueryLimit=${MAX_QUERY_LIMIT}, maxNearBlackPermille=${MAX_NEAR_BLACK_PERMILLE}, "
  "maxVerticalGapPixels=${MAX_VERTICAL_GAP_PIXELS}, maxVerticalGapSpan=${MAX_VERTICAL_GAP_SPAN}")
