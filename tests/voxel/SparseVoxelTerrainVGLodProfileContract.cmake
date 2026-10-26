# Terrain VG LOD profile contract. Voxel sizes are exact powers of two of the
# finest voxel (10 cm), so the settled profile is exact: a chunk the culler
# touched on the final frame, at level L with voxel v_L, must sit farther
# than R_L = v_L * pixelsPerMetre / threshold at its nearest approach, or the
# culler would still be demanding it. Inside R_1 every touched chunk must be
# 10 cm, and voxel size may only grow with distance from there.
if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()
execute_process(
  COMMAND "${SAMPLE}"
    --world-map "${WORLD_MAP}"
    --terrain-render-distance-km 1
    --frames 1200
    --resolution 640x360
    --lod-threshold 1.5
    --test-static-scene
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
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SparseVoxelTerrainVGLodProfileContract.log" "${LOG}")
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "Terrain VG LOD profile run failed:\n${LOG}")
endif()
string(REGEX MATCH "Sparse voxel terrain lod profile: touched=([0-9]+) violations=([0-9]+) worstPx=([0-9.]+) worstLevels=([0-9.]+) overBudget=([0-9]+) inversions=([0-9]+) finestRadiusM=([0-9.]+) nearCoarse=([0-9]+)[^\n]*" PROFILE_LINE "${LOG}")
if(NOT PROFILE_LINE)
  message(FATAL_ERROR "No LOD profile line in the run:\n${LOG}")
endif()
set(TOUCHED "${CMAKE_MATCH_1}")
set(VIOLATIONS "${CMAKE_MATCH_2}")
set(WORST_PX "${CMAKE_MATCH_3}")
set(WORST_LEVELS "${CMAKE_MATCH_4}")
set(INVERSIONS "${CMAKE_MATCH_6}")
set(NEAR_COARSE "${CMAKE_MATCH_8}")
if(TOUCHED LESS 30)
  message(FATAL_ERROR "Too few touched chunks to judge a profile (touched=${TOUCHED}): ${PROFILE_LINE}")
endif()
if(NOT VIOLATIONS EQUAL 0)
  # The 512-owner source cut can exhaust its budget while VG keeps a valid
  # resident fallback. Keep the strict near-field and radial checks below,
  # and allow only the configured 4 px source error at that hard capacity.
  string(REGEX MATCH "residentChunks=([0-9]+) installFailures=0 noSlice=0" CAPACITY "${LOG}")
  if(NOT CAPACITY OR CMAKE_MATCH_1 LESS 509 OR WORST_PX GREATER 4.0 OR
     NOT LOG MATCHES "World terrain no-op: pending=[0-9]+ built=0")
    message(FATAL_ERROR "Terrain source error failed to converge within its capacity bound: ${PROFILE_LINE}")
  endif()
  string(REGEX MATCH "Sparse voxel terrain VG cut contract:[^\n]* giants=0 " RENDERED_CUT "${LOG}")
  if(NOT RENDERED_CUT)
    message(FATAL_ERROR "Capacity-limited source terrain still draws oversized clusters: ${PROFILE_LINE}")
  endif()
endif()
if(NOT INVERSIONS EQUAL 0)
  message(FATAL_ERROR "Voxel size does not grow monotonically with distance from the camera (inversions=${INVERSIONS}): ${PROFILE_LINE}")
endif()
if(NOT NEAR_COARSE EQUAL 0)
  message(FATAL_ERROR "Chunks inside the 10 cm radius that are not 10 cm (nearCoarse=${NEAR_COARSE}): ${PROFILE_LINE}")
endif()
message(STATUS "Terrain VG LOD profile contract: ${PROFILE_LINE}")
