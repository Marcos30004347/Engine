if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

set(BASE_ARGS
  --world-map "${WORLD_MAP}"
  --terrain-render-distance-km 16
  --gbuffer-resolver bricks
  --frames 180
  --resolution 640x360
  --render-scale 1
  --test-world-terrain-raster
  --no-taa
  --disable-metalfx-upscaler
  --disable-frame-generation
  --disable-vsync
)
set(COMMON_ARGS
  ${BASE_ARGS}
  --world-map-center 0.5,0.5
)

function(assert_gpu_root_ownership PREFIX LOG)
  string(REGEX MATCHALL
    "Terrain runtime page table:[^\r\n]*"
    ROOT_LINES "${LOG}")
  if(NOT ROOT_LINES)
    message(FATAL_ERROR "${PREFIX} emitted no terrain root ownership contract")
  endif()
  list(GET ROOT_LINES -1 ROOT_LINE)
  string(REGEX MATCH
    "resident=([0-9]+)[^\r\n]*runtimeVisibleRoots=([0-9]+)"
    ROOT_MATCH "${ROOT_LINE}")
  if(ROOT_MATCH STREQUAL "")
    message(FATAL_ERROR
      "Could not parse ${PREFIX} root ownership: ${ROOT_LINE}")
  endif()
  if(NOT CMAKE_MATCH_1 EQUAL CMAKE_MATCH_2)
    message(FATAL_ERROR
      "${PREFIX} exposed reserved GPU root capacity as geometry: "
      "resident=${CMAKE_MATCH_1}, visibleRoots=${CMAKE_MATCH_2}; ${ROOT_LINE}")
  endif()
endfunction()

function(run_terrain PREFIX)
  execute_process(
    COMMAND "${SAMPLE}" ${COMMON_ARGS} ${ARGN}
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE STDOUT
    ERROR_VARIABLE STDERR
  )
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR
      "${PREFIX} terrain regression failed:\n${STDOUT}\n${STDERR}")
  endif()
  set(LOG "${STDOUT}\n${STDERR}")
  assert_gpu_root_ownership("${PREFIX}" "${LOG}")
  string(REGEX MATCHALL
    "Sparse voxel terrain image:[^\r\n]*"
    IMAGE_LINES "${LOG}")
  if(NOT IMAGE_LINES)
    message(FATAL_ERROR "${PREFIX} emitted no terrain image contract")
  endif()
  list(GET IMAGE_LINES -1 IMAGE_LINE)
  string(REGEX MATCH "terrainPixels=([0-9]+)" COVERAGE "${IMAGE_LINE}")
  if(COVERAGE STREQUAL "")
    message(FATAL_ERROR "Could not parse ${PREFIX} coverage: ${IMAGE_LINE}")
  endif()
  set("${PREFIX}_COVERAGE" "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set("${PREFIX}_LINE" "${IMAGE_LINE}" PARENT_SCOPE)
endfunction()

function(run_terrain_auto_spawn PREFIX)
  execute_process(
    COMMAND "${SAMPLE}" ${BASE_ARGS} ${ARGN}
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE STDOUT
    ERROR_VARIABLE STDERR
  )
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR
      "${PREFIX} auto-spawn terrain regression failed:\n${STDOUT}\n${STDERR}")
  endif()
  set(LOG "${STDOUT}\n${STDERR}")
  assert_gpu_root_ownership("${PREFIX} auto-spawn" "${LOG}")
  string(REGEX MATCHALL
    "Sparse voxel terrain image:[^\r\n]*"
    IMAGE_LINES "${LOG}")
  if(NOT IMAGE_LINES)
    message(FATAL_ERROR
      "${PREFIX} auto-spawn emitted no terrain image contract")
  endif()
  list(GET IMAGE_LINES -1 IMAGE_LINE)
  string(REGEX MATCH "terrainPixels=([0-9]+)" COVERAGE "${IMAGE_LINE}")
  if(COVERAGE STREQUAL "")
    message(FATAL_ERROR
      "Could not parse ${PREFIX} auto-spawn coverage: ${IMAGE_LINE}")
  endif()
  set("${PREFIX}_COVERAGE" "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set("${PREFIX}_LINE" "${IMAGE_LINE}" PARENT_SCOPE)
endfunction()

run_terrain(HORIZON)
run_terrain_auto_spawn(AUTO_HORIZON)
run_terrain(BRICKS --test-world-terrain-slope)
# Raw BaseColor while turning used to expose holes that a static final frame
# missed: proxy faces flipped at brick-centre planes and ordinary triangle
# edge rules opened single-pixel seams. Keep temporal reconstruction and
# lighting out of this fixture so any failure belongs to raster/depth itself.
run_terrain(ROTATING_ALBEDO
  --test-world-terrain-slope
  --test-camera-ground-forward
  --test-camera-travel-m 0
  --test-camera-yaw-rate 0.5
  --debug-mode 0
)
run_terrain(MESH --test-world-terrain-slope --terrain-mesh-only)
run_terrain(TEN_CM --test-world-terrain-10cm)

math(EXPR COVERAGE_DELTA "${BRICKS_COVERAGE} - ${MESH_COVERAGE}")
if(COVERAGE_DELTA LESS 0)
  math(EXPR COVERAGE_DELTA "-${COVERAGE_DELTA}")
endif()
math(EXPR MAXIMUM_DELTA "${MESH_COVERAGE} / 200 + 32")
if(COVERAGE_DELTA GREATER MAXIMUM_DELTA)
  message(FATAL_ERROR
    "Brick raster coverage differs from the smooth heightfield mesh: "
    "brick=${BRICKS_COVERAGE}, mesh=${MESH_COVERAGE}, delta=${COVERAGE_DELTA}; "
    "brickContract=${BRICKS_LINE}; meshContract=${MESH_LINE}")
endif()

message(STATUS
  "World terrain raster regression passed: centered=${HORIZON_COVERAGE}, "
  "autoSpawn=${AUTO_HORIZON_COVERAGE}, brick=${BRICKS_COVERAGE}, "
  "rotatingAlbedo=${ROTATING_ALBEDO_COVERAGE}, "
  "mesh=${MESH_COVERAGE}, delta=${COVERAGE_DELTA}")
