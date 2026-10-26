if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

# Captured at the 5 km world-data origin fixture, close enough to LOD0 terrain
# that the camera can see bricks straddling the eye plane. At this orientation
# the raster proxy must remain conservative: the fragment DDA is the only
# per-pixel terrain visibility decision after frustum/HZB select a brick. The
# regression looked like page/brick culling, with triangular/rectangular missing
# chunks and fine line gaps that appeared only at particular camera angles.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    SVR_TRACE_TERRAIN=1
    SVR_TRACE_TERRAIN_HOLES=1
    "${SAMPLE}"
      --world-map "${WORLD_MAP}"
      --world-map-center 0.5,0.5
      --terrain-render-distance-km 5
      --gbuffer-resolver bricks
      --resolution 1220x720
      --render-scale 0.5
      --camera-position -10.1782389,766.122864,-2.71868563
      --camera-rotation "-0.224900275,0.765143335,0.350374192,0.491135091"
      --camera-near 0.00100000005
      --camera-far 5000
      --debug-mode 0
      --frames 404
      --test-frame-signatures
      --test-capture-start-frame 400
      --test-terrain-freeze-frame 360
      --disable-lighting
      --no-taa
      --disable-metalfx-upscaler
      --disable-frame-generation
      --disable-vsync
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE OUTPUT
  ERROR_VARIABLE ERROR)
set(LOG "${OUTPUT}\n${ERROR}")
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR
    "Captured angle-coverage run failed (${RESULT})\n${LOG}")
endif()

# Terrain rendering owns a 5 km radius in this fixture. The replay should keep
# the depth range aligned with the terrain radius instead of spending precision
# on space where no terrain can be resident.
if(NOT LOG MATCHES
    "Camera replay setup:[^\r\n]*near=0\\.00100000005 far=5000")
  message(FATAL_ERROR
    "Captured replay did not clamp camera far to terrain radius\n${LOG}")
endif()

string(REGEX MATCHALL
  "Sparse voxel frame signature: frame=403[^\r\n]*" SIGNATURES "${LOG}")
if(NOT SIGNATURES)
  message(FATAL_ERROR "Captured replay emitted no settled frame signature")
endif()
list(GET SIGNATURES -1 SIGNATURE)
if(NOT SIGNATURE MATCHES "coveredPixels=([0-9]+)")
  message(FATAL_ERROR "Could not parse captured coverage: ${SIGNATURE}")
endif()
set(COVERED "${CMAKE_MATCH_1}")
# The broken proxy path covered only 827761 pixels in this fixture, while the
# conservative proxy + fragment DDA covers the full 1220x720 frame. Keep a tiny
# margin for harmless raster/backend changes while prohibiting that regression.
if(COVERED LESS 878000)
  message(FATAL_ERROR
    "Angle-dependent raster rejection reopened terrain: ${SIGNATURE}")
endif()

string(REGEX MATCHALL "SVR terrain holes: frame=403[^\r\n]*" HOLE_LINES
  "${LOG}")
if(NOT HOLE_LINES)
  message(FATAL_ERROR "Captured replay emitted no terrain-hole diagnostics")
endif()
list(GET HOLE_LINES -1 HOLES)
if(NOT HOLES MATCHES
    "interiorGapPixels=0[^\r\n]*verticalGap=0[^\r\n]*maxSpan=0")
  message(FATAL_ERROR
    "Captured camera contains an enclosed terrain coverage gap: ${HOLES}")
endif()

string(REGEX MATCHALL "SVR terrain trace: frame=[0-9]+[^\r\n]*" TRACE_LINES
  "${LOG}")
if(NOT TRACE_LINES)
  message(FATAL_ERROR "Captured replay emitted no terrain trace")
endif()
list(GET TRACE_LINES -1 TRACE)
if(NOT TRACE MATCHES "cameraLOD=0" OR
   NOT TRACE MATCHES "pendingChunks=0" OR
   NOT TRACE MATCHES "jobPending=0 uploadPending=0")
  message(FATAL_ERROR
    "Captured coverage was measured before LOD0 settled: ${TRACE}")
endif()

message(STATUS
  "Captured angle coverage passed: covered=${COVERED}, ${HOLES}")
