if(NOT DEFINED SAMPLE OR NOT DEFINED WORLD_MAP)
  message(FATAL_ERROR "SAMPLE and WORLD_MAP are required")
endif()

set(COMMON_ARGS
  --world-map "${WORLD_MAP}"
  --world-map-center 0.5,0.5
  --terrain-render-distance-km 4
  --gbuffer-resolver bricks
  --test-world-terrain-10cm
  --test-world-terrain-slope
  --test-camera-ground-forward
  --test-camera-height 0.01
  --test-camera-travel-m 0
  --test-camera-yaw-rate 1
  --test-camera-yaw-frames 60
  --frames 120
  --resolution 160x90
  --render-scale 1
  --test-frame-signatures
  --jitter-scale 0
  --no-taa
  --disable-metalfx-upscaler
  --disable-frame-generation
  --disable-vsync)

function(run_shadow_variant NAME)
  execute_process(
    COMMAND "${SAMPLE}" ${COMMON_ARGS} ${ARGN}
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR)
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR
      "${NAME} close LOD0 shadow run failed (${RESULT})\n${OUTPUT}\n${ERROR}")
  endif()
  set(${NAME}_LOG "${OUTPUT}\n${ERROR}" PARENT_SCOPE)
endfunction()

run_shadow_variant(SCREEN)
run_shadow_variant(WORLD --disable-screen-traces)

function(validate_authoritative_hits LOG PREFIX)
  string(REGEX MATCHALL "Sparse voxel scene ray stats: frame=[^\n]*" LINES "${LOG}")
  set(SETTLED_COUNT 0)
  set(HIT_FRAME_COUNT 0)
  set(FALSE_SCREEN 0)
  foreach(LINE IN LISTS LINES)
    if(LINE MATCHES "frame=([0-9]+).*screenHits=([0-9]+).*worldHits=([0-9]+).*screenWorldBothHit=([0-9]+).*screenHitWorldMiss=([0-9]+).*screenMissWorldHit=([0-9]+)")
      set(FRAME "${CMAKE_MATCH_1}")
      set(WORLD_HITS "${CMAKE_MATCH_3}")
      set(CONFIRMED_SCREEN_HITS "${CMAKE_MATCH_4}")
      set(SCREEN_MISS_WORLD_HITS "${CMAKE_MATCH_6}")
      math(EXPR MERGED_OCCLUDERS "${WORLD_HITS} + ${CONFIRMED_SCREEN_HITS}")
      math(EXPR RAW_TREE_OCCLUDERS "${SCREEN_MISS_WORLD_HITS} + ${CONFIRMED_SCREEN_HITS}")
      math(EXPR FALSE_SCREEN "${FALSE_SCREEN} + ${CMAKE_MATCH_5}")
      if(NOT LINE MATCHES "skyHits=([0-9]+)")
        message(FATAL_ERROR "${PREFIX} statistics omitted sky-lane counters")
      endif()
      # Residency commits are asynchronous and can land a few timeline frames
      # apart in separate processes. Frame 67 is the first frame after both
      # logs have reliably published the complete LOD0 cut under the current
      # Tree64 streaming cadence.
      if(FRAME GREATER_EQUAL 67)
        if(MERGED_OCCLUDERS GREATER 0)
          math(EXPR HIT_FRAME_COUNT "${HIT_FRAME_COUNT} + 1")
        endif()
        if(PREFIX STREQUAL "SCREEN" AND
           NOT MERGED_OCCLUDERS EQUAL RAW_TREE_OCCLUDERS)
          message(FATAL_ERROR
            "${PREFIX} merged visibility diverged from the same-ray Tree64 "
            "answer at frame ${FRAME}: merged=${MERGED_OCCLUDERS}, "
            "Tree64=${RAW_TREE_OCCLUDERS}")
        endif()
        math(EXPR SETTLED_COUNT "${SETTLED_COUNT} + 1")
      endif()
    endif()
  endforeach()
  if(SETTLED_COUNT LESS 20)
    message(FATAL_ERROR
      "${PREFIX} did not produce enough settled rotating-camera statistics")
  endif()
  if(HIT_FRAME_COUNT LESS 20)
    message(FATAL_ERROR
      "${PREFIX} produced close-slope shadow hits in only "
      "${HIT_FRAME_COUNT} settled frames")
  endif()
  set(${PREFIX}_FALSE_SCREEN "${FALSE_SCREEN}" PARENT_SCOPE)
endfunction()

validate_authoritative_hits("${SCREEN_LOG}" SCREEN)
validate_authoritative_hits("${WORLD_LOG}" WORLD)

function(validate_presented_shadow_contrast LOG PREFIX)
  string(REGEX MATCHALL
    "Sparse voxel temporal shading: frame=[^\n]*"
    LINES "${LOG}")
  set(SETTLED_FRAMES 0)
  set(CONTRAST_FRAMES 0)
  foreach(LINE IN LISTS LINES)
    if(LINE MATCHES "frame=([0-9]+).*litFraction=\\[([^]]*)\\]")
      set(FRAME "${CMAKE_MATCH_1}")
      if(FRAME GREATER_EQUAL 67)
        math(EXPR SETTLED_FRAMES "${SETTLED_FRAMES} + 1")
        # A Tree64 hit counter alone proves only that visibility was traced.
        # The shipped image must also keep a visible dark-side population; the
        # old terrain default could produce hits and then smooth them back into
        # a fully-lit presentation. Values are emitted with three decimals, so
        # this catches any region below 0.990 lit fraction.
        set(FRACTIONS "${CMAKE_MATCH_2}")
        if(FRACTIONS MATCHES "(^|,)(0\\.[0-8][0-9][0-9]|0\\.9[0-8][0-9])")
          math(EXPR CONTRAST_FRAMES "${CONTRAST_FRAMES} + 1")
        endif()
      endif()
    endif()
  endforeach()
  if(SETTLED_FRAMES LESS 20)
    message(FATAL_ERROR
      "${PREFIX} did not emit enough settled presented-shadow statistics")
  endif()
  if(CONTRAST_FRAMES LESS 10)
    message(FATAL_ERROR
      "${PREFIX} traced terrain occluders but the presented image kept no "
      "persistent hard-shadow contrast (contrastFrames=${CONTRAST_FRAMES}, "
      "settledFrames=${SETTLED_FRAMES})")
  endif()
endfunction()

validate_presented_shadow_contrast("${SCREEN_LOG}" SCREEN)
validate_presented_shadow_contrast("${WORLD_LOG}" WORLD)

# Do not compare stochastic ReSTIR hit counts between the two processes. A
# screen-space result feeds the preceding frames' reservoirs, so separate runs
# can legitimately choose slightly different later rays even with identical
# camera motion. The per-frame assertion above is stronger for path parity: it
# compares the merged screen-enabled answer with Tree64 for the exact same ray.
# Some generated-world fixtures no longer produce a coplanar screen-only hit in
# this camera path; the focused scene-ray GPU screen-trace tests keep that
# rejection case covered.

foreach(PREFIX IN ITEMS SCREEN WORLD)
  set(LOG "${${PREFIX}_LOG}")
  if(NOT LOG MATCHES
      "Terrain lighting:[^\n]*probeShadow=\\(fade 3000m\\.\\.analytic 4400m\\)")
    message(FATAL_ERROR
      "${PREFIX} terrain lighting reverted to a near analytic shadow cutoff; "
      "horizon terrain must remain traceable for the 4 km test view")
  endif()
  if(NOT LOG MATCHES
      "Sparse voxel probe distance LOD:[^\n]*traceFade=3000\\.0m[^\n]*analytical=4400\\.0m")
    message(FATAL_ERROR
      "${PREFIX} effective probe shadow cutoff does not preserve the terrain "
      "horizon band")
  endif()
  if(NOT LOG MATCHES
      "Terrain 10 cm ground contract: found=1 lod=0 voxel=0\\.100000m[^\n]*clearance=0\\.0[0-2][0-9][0-9][0-9][0-9]m")
    message(FATAL_ERROR "${PREFIX} did not keep the slope close at LOD0")
  endif()
  if(NOT LOG MATCHES
      "Sparse voxel terrain image: terrainPixels=[0-9]+[^\n]*interiorHoles=0[^\n]*interiorGapPixels=0[^\n]*verticalGapPixels=0")
    message(FATAL_ERROR
      "${PREFIX} close LOD0 terrain contains raster/Tree64 coverage holes")
  endif()
endforeach()

message(STATUS
  "close LOD0 slope same-ray shadows match for screen/world paths; "
  "rejected screen-only hits=${SCREEN_FALSE_SCREEN}")
