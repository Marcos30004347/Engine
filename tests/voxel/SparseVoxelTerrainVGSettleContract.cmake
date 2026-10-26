# Terrain VG settle contract: at the real pixel threshold (1.5 px, never the
# 64 px bounded-run default), a 1 km world-map cut must stop asking for
# geometry well before the run ends, reach the finest level under the camera,
# install every page it asks for, give every resident chunk a hierarchy slice,
# and show no interior hole on the final frame.
if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env SVR_TRACE_TERRAIN_HOLES=1
    "${SAMPLE}"
    --world-map "${WORLD_MAP}"
    --terrain-render-distance-km 1
    --frames 600
    --resolution 640x360
    --lod-threshold 1.5
    --test-static-scene
    --test-frame-signatures
    --test-capture-start-frame 590
    --no-taa
    --jitter-scale 0
    --disable-metalfx-upscaler
    --disable-frame-generation
    --disable-vsync
  RESULT_VARIABLE RESULT
  OUTPUT_VARIABLE STDOUT
  ERROR_VARIABLE STDERR
)
set(LOG "${STDOUT}\n${STDERR}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SparseVoxelTerrainVGSettleContract.log" "${LOG}")
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "Terrain VG settle contract run failed:\n${LOG}")
endif()
string(REGEX MATCH "Sparse voxel terrain settle: lastActivityFrame=([0-9]+) lod0Chunks=([0-9]+) residentChunks=([0-9]+) installFailures=([0-9]+) noSlice=([0-9]+)" SETTLE_LINE "${LOG}")
if(NOT SETTLE_LINE)
  message(FATAL_ERROR "No settle summary line in the run:\n${LOG}")
endif()
set(LAST_ACTIVITY "${CMAKE_MATCH_1}")
set(LOD0 "${CMAKE_MATCH_2}")
set(RESIDENT "${CMAKE_MATCH_3}")
set(INSTALL_FAILURES "${CMAKE_MATCH_4}")
set(NO_SLICE "${CMAKE_MATCH_5}")
if(LAST_ACTIVITY GREATER 540)
  message(FATAL_ERROR "Terrain VG cut did not settle for the final 60 frames (lastActivityFrame=${LAST_ACTIVITY} of 600): ${SETTLE_LINE}")
endif()
if(LOD0 LESS 10)
  message(FATAL_ERROR "The ground under the camera did not reach the finest level (lod0Chunks=${LOD0}): ${SETTLE_LINE}")
endif()
if(NOT INSTALL_FAILURES EQUAL 0)
  message(FATAL_ERROR "Page or chunk installs failed for space (installFailures=${INSTALL_FAILURES}); size the VG pools for the real working set: ${SETTLE_LINE}")
endif()
if(NOT NO_SLICE EQUAL 0)
  message(FATAL_ERROR "Resident chunks without a hierarchy slice (noSlice=${NO_SLICE}): ${SETTLE_LINE}")
endif()
string(REGEX MATCHALL "SVR terrain holes: frame=[0-9]+ terrainPixels=[0-9]+ nearBlack=[0-9]+ isolatedNearBlack=[0-9]+ interiorHoles=[0-9]+" HOLE_LINES "${LOG}")
if(NOT HOLE_LINES)
  message(FATAL_ERROR "The run emitted no terrain-hole diagnostics on its captured frames:\n${LOG}")
endif()
list(GET HOLE_LINES -1 LAST_HOLES)
string(REGEX MATCH "interiorHoles=([0-9]+)" HOLE_MATCH "${LAST_HOLES}")
if(NOT CMAKE_MATCH_1 EQUAL 0)
  message(FATAL_ERROR "Interior terrain holes on the final frame: ${LAST_HOLES}")
endif()

# `interiorHoles` alone is not enough to say the terrain is whole. It counts
# only pixels enclosed by terrain above *and* below in the same column, so
# terrain replaced by sky is not enclosed and does not register: a
# configuration measured at 12.98 interiorGapPixels was in fact drawing
# 98,824 terrain pixels against 219,600, more than half the terrain missing.
# So assert the coverage itself. Virtual geometry is the only terrain
# rasterizer on this route - the brick resolver is registered feedbackOnly and
# has no draw pass - so this number is exactly what VG put on screen.
string(REGEX MATCHALL "terrainPixels=([0-9]+)" TERRAIN_PIXEL_MATCHES "${LOG}")
set(MAX_TERRAIN_PIXELS 0)
foreach(MATCH ${TERRAIN_PIXEL_MATCHES})
  string(REGEX MATCH "([0-9]+)" PIXELS "${MATCH}")
  if(PIXELS GREATER MAX_TERRAIN_PIXELS)
    set(MAX_TERRAIN_PIXELS "${PIXELS}")
  endif()
endforeach()
string(REGEX MATCH "terrainPixels=([0-9]+)" FINAL_PIXEL_MATCH "${LAST_HOLES}")
set(FINAL_TERRAIN_PIXELS "${CMAKE_MATCH_1}")
if(MAX_TERRAIN_PIXELS GREATER 0)
  math(EXPR COVERAGE_FLOOR "${MAX_TERRAIN_PIXELS} * 80 / 100")
  if(FINAL_TERRAIN_PIXELS LESS COVERAGE_FLOOR)
    message(FATAL_ERROR
      "Terrain coverage collapsed on the final frame "
      "(terrainPixels=${FINAL_TERRAIN_PIXELS} against a run peak of "
      "${MAX_TERRAIN_PIXELS}); interiorHoles cannot see terrain that became "
      "sky: ${LAST_HOLES}")
  endif()
endif()

# Keep triangle-size diagnostics for detecting gross fallback artifacts.
# Closed voxel sides and greedy top faces have different lengths even in a
# correct error-selected cut; the tiny fraction is not an LOD acceptance test.
string(REGEX MATCH
  "Sparse voxel terrain VG cut contract: [^\n]*giantRefinable=(-?[0-9]+)[^\n]*drawn=([0-9]+) tiny=([0-9]+)"
  CUT_LINE "${LOG}")
if(CUT_LINE)
  set(GIANT_REFINABLE "${CMAKE_MATCH_1}")
  set(DRAWN "${CMAKE_MATCH_2}")
  set(TINY "${CMAKE_MATCH_3}")
  # A giant that *can* refine means the stored errors understate the on-screen
  # deviation - an error-selection defect. A giant that is a streaming leaf is
  # the additive frontier, which is a coverage policy and not this contract's.
  #
  # Not zero-tolerance: this is the last frame's count over ~200,000 drawn
  # clusters, and a handful appear and disappear between runs at a refinement
  # boundary. Measured 0 and 3 on consecutive runs of this camera. What the
  # contract is for is a systematic understatement, so scale the bar with the
  # cut and keep a floor for small cuts.
  math(EXPR REFINABLE_BAR "${DRAWN} / 1000")
  if(REFINABLE_BAR LESS 32)
    set(REFINABLE_BAR 32)
  endif()
  if(GIANT_REFINABLE GREATER REFINABLE_BAR)
    message(FATAL_ERROR
      "Drawn clusters are coarser than the budget and could have refined "
      "(giantRefinable=${GIANT_REFINABLE} of drawn=${DRAWN}, bar "
      "${REFINABLE_BAR}): the stored cluster errors understate on-screen "
      "deviation")
  endif()
  if(DRAWN GREATER 0)
    math(EXPR TINY_PERCENT "${TINY} * 100 / ${DRAWN}")
    # Triangle size is diagnostic, not VG geometric error. Closed blocks add
    # short side faces, especially at grazing angles, and greedy top faces can
    # be large while still lying exactly on the represented surface. A fixed
    # tiny-triangle fraction incorrectly rejects a valid cluster error cut.
    # Near-field LOD, coverage and refinable-cluster error are checked above
    # and by the cut-error contract.
    message(STATUS
      "Terrain VG cut bands: drawn=${DRAWN} tiny=${TINY} (${TINY_PERCENT}%) "
      "giantRefinable=${GIANT_REFINABLE}")
  endif()
endif()

message(STATUS
  "Terrain VG settle contract: ${SETTLE_LINE} | ${LAST_HOLES} | "
  "terrainPixels final=${FINAL_TERRAIN_PIXELS} peak=${MAX_TERRAIN_PIXELS}")
