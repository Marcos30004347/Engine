if(NOT DEFINED SAMPLE)
    message(FATAL_ERROR "SAMPLE must point at SparseVoxelRendererSample")
endif()

# Exercise the default backend selection and the real renderer worklist,
# reservoir history, screen trace, native AS, and final lighting resolve.
set(total_hits 0)
set(total_misses 0)
foreach(mode IN ITEMS screen-first world-only interior)
    set(extra)
    if(mode STREQUAL "world-only")
        list(APPEND extra --disable-screen-traces)
    endif()
    if(mode STREQUAL "interior")
        list(APPEND extra --test-camera-interior --test-multiple-lights)
    endif()
    execute_process(
        COMMAND "${SAMPLE}" --no-terrain --frames 24 --test-static-scene
            --test-frame-signatures --test-capture-start-frame 22
            --resolution 320x180 --render-scale 1 --disable-vsync
            --disable-metalfx-upscaler --disable-frame-generation ${extra}
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
        TIMEOUT 120
    )
    set(output "${stdout}\n${stderr}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/restir-${mode}-smoke.log" "${output}")
    if(output MATCHES "ReSTIR hardware tracing unavailable for this scene/backend")
        message(STATUS "SKIP: Metal hardware ray tracing unavailable")
        return()
    endif()
    if(NOT result EQUAL 0 OR NOT output MATCHES "ReSTIR hardware ray tracing: Metal")
        message(FATAL_ERROR "${mode}: default hardware ReSTIR did not run\n${output}")
    endif()
    if(NOT output MATCHES "Sparse voxel scene ray stats: frame=23 rays=([1-9][0-9]*) screenHits=([0-9]+) screenMisses=([0-9]+) worldHits=([0-9]+) worldMisses=([0-9]+) escapedScene=0 unresolved=0")
        message(FATAL_ERROR "${mode}: expected resolved native shadow results\n${output}")
    endif()
    math(EXPR total_hits "${total_hits} + ${CMAKE_MATCH_4}")
    math(EXPR total_misses "${total_misses} + ${CMAKE_MATCH_5}")
    if(mode STREQUAL "world-only" AND NOT CMAKE_MATCH_2 EQUAL 0)
        message(FATAL_ERROR "World-only tracing accepted a screen-space hit")
    endif()
    if(NOT output MATCHES "Sparse voxel smoke test passed:.*outputHash=[0-9a-fA-F]+")
        message(FATAL_ERROR "${mode}: final lighting resolve was not validated\n${output}")
    endif()
endforeach()
if(total_hits EQUAL 0 OR total_misses EQUAL 0)
    message(FATAL_ERROR "Expected both visible exterior rays and occluded interior rays")
endif()
message(STATUS "Hardware ReSTIR shadows passed: exterior visibility and interior occlusion, screen-first and world-only")
