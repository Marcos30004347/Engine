# Moving through a full source cut must leave room for the temporary VG
# ancestors as their children's meshes arrive. A 512-slice pool previously
# exhausted itself against 511 source chunks and never installed the remainder.
if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()
# Keep the return trip, and also leave the original fine region permanently.
# Checking only the starting location hides capacity stalls on new ground.
foreach(TRAVEL_KIND IN ITEMS roundtrip forward_moving forward)
  if(TRAVEL_KIND STREQUAL "roundtrip")
    set(FRAMES 600)
    set(CAPTURE 590)
    set(TRAVEL --test-camera-travel-m 24 --test-camera-out-and-back)
  else()
    set(FRAMES 1400)
    set(CAPTURE 1390)
    if(TRAVEL_KIND STREQUAL "forward_moving")
      # Inspect the destination while still moving, before any settling time.
      set(FRAMES 1000)
      set(CAPTURE 990)
    endif()
    set(TRAVEL --test-camera-travel-m 256
      --test-camera-travel-start-frame 400 --test-camera-travel-frames 600)
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env SVR_TRACE_TERRAIN=1 "${SAMPLE}"
      --world-map "${WORLD_MAP}" --terrain-render-distance-km 1
      --frames ${FRAMES} --resolution 640x360 --memory-budget-mb 7168
      --lod-threshold 1.5 --test-world-terrain-10cm
      --test-camera-height 1.7 ${TRAVEL} --test-capture-start-frame ${CAPTURE}
      --no-taa --jitter-scale 0 --disable-metalfx-upscaler
      --disable-frame-generation --disable-vsync
    TIMEOUT 170
    RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR
  )
  set(LOG "${STDOUT}\n${STDERR}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SparseVoxelTerrainVGMovementContract-${TRAVEL_KIND}.log" "${LOG}")
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Terrain VG movement failed: ${LOG}")
  endif()
  if(NOT LOG MATCHES "Terrain VG uses G-buffer Lambert shading; light tracing disabled" OR
     LOG MATCHES "name=svr.sample.(SceneTree64|Tree64Trace|Tree64DynamicTrace|SceneRayCreate|SceneRaySchedule)")
    message(FATAL_ERROR "The terrain geometry fixture enabled light tracing")
  endif()
  string(REGEX MATCH "Sparse voxel terrain settle:[^\n]*" SETTLE "${LOG}")
  if(NOT SETTLE MATCHES "installFailures=0 noSlice=0 bareAfterRetire=0")
    message(FATAL_ERROR "Moving terrain lost an install or hierarchy slot: ${SETTLE}")
  endif()
  string(REGEX MATCHALL "Sparse voxel terrain lod profile:[^\n]*" PROFILES "${LOG}")
  if(NOT PROFILES)
    message(FATAL_ERROR "No visible terrain LOD profile")
  endif()
  list(GET PROFILES -1 PROFILE)
  if(NOT PROFILE MATCHES "nearCoarse=0 " OR NOT PROFILE MATCHES "v=0.10")
    message(FATAL_ERROR "The close moving camera did not reach 10 cm terrain: ${PROFILE}")
  endif()
  string(REGEX MATCHALL "Sparse voxel frame signature:[^\n]*" SIGNATURES "${LOG}")
  if(NOT SIGNATURES)
    message(FATAL_ERROR "No final-frame coverage readback")
  endif()
  list(GET SIGNATURES -1 SIGNATURE)
  if(NOT SIGNATURE MATCHES "coveredPixels=([1-9][0-9]+) holes=0 enclosed=0 ")
    message(FATAL_ERROR "The moving cut left a geometry gap: ${SIGNATURE}")
  endif()
  message(STATUS "Terrain VG movement passed: ${SETTLE}; ${PROFILE}; ${SIGNATURE}")
endforeach()
