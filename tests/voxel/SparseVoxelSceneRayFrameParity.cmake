if(NOT DEFINED SAMPLE)
    message(FATAL_ERROR "SAMPLE must point at SparseVoxelRendererSample")
endif()

set(COMMON_ARGS
    --gbuffer-resolver svraster
    --frames 4
    --test-camera-interior
    --test-multiple-lights
    --shadow-backend tree64
    --test-frame-signatures
    --lighting-scale 4
    --rays-per-pixel 1
    --disable-vsync
    --resolution 320x180
)

function(run_scene_ray_case CASE_NAME HASH_VAR WORLD_VAR WORLD_HITS_VAR SCREEN_HITS_VAR SCREEN_MISSES_VAR)
    execute_process(
        COMMAND "${SAMPLE}" ${COMMON_ARGS} ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    set(output "${stdout}\n${stderr}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${CASE_NAME} failed with exit code ${result}\n${output}")
    endif()
    if(NOT output MATCHES "Sparse voxel smoke test passed:.*outputHash=([0-9a-fA-F]+)")
        message(FATAL_ERROR "${CASE_NAME} did not report a final output hash\n${output}")
    endif()
    set(hash "${CMAKE_MATCH_1}")
    if(NOT output MATCHES "Sparse voxel tree64 dispatch: frame=3[^\\n]*dispatchRayCount=([1-9][0-9]*)")
        message(FATAL_ERROR "${CASE_NAME} did not dispatch Tree64 rays on the final frame\n${output}")
    endif()
    if(NOT output MATCHES "Sparse voxel scene ray stats: frame=3 rays=([1-9][0-9]*) screenHits=([0-9]+) screenMisses=([0-9]+) worldHits=([0-9]+) worldMisses=([0-9]+) escapedScene=([0-9]+) unresolved=([0-9]+) escapedTreeVolume=([0-9]+) unknownResidency=([0-9]+) queryLimit=([0-9]+) reservoirM=([0-9]+) temporalReservoirs=([0-9]+) spatialReservoirs=([0-9]+) invalidRays=([0-9]+)")
        message(FATAL_ERROR "${CASE_NAME} did not report scene-ray statistics\n${output}")
    endif()
    set(ray_count "${CMAKE_MATCH_1}")
    set(screen_hits "${CMAKE_MATCH_2}")
    set(screen_misses "${CMAKE_MATCH_3}")
    set(world_hits "${CMAKE_MATCH_4}")
    set(reservoir_m "${CMAKE_MATCH_11}")
    set(invalid_rays "${CMAKE_MATCH_14}")
    math(EXPR world_results "${CMAKE_MATCH_4} + ${CMAKE_MATCH_5}")
    if(world_results EQUAL 0)
        message(FATAL_ERROR "${CASE_NAME} produced no merged Tree64 scene-ray results\n${output}")
    endif()
    if(reservoir_m EQUAL 0)
        message(FATAL_ERROR "${CASE_NAME} produced no selected DI reservoirs\n${output}")
    endif()
    if(invalid_rays GREATER_EQUAL ray_count)
        message(FATAL_ERROR "${CASE_NAME} only scheduled invalid scene rays\n${output}")
    endif()
    if(world_hits EQUAL 0)
        message(FATAL_ERROR "${CASE_NAME} produced no Tree64 occluder hits\n${output}")
    endif()
    set(${HASH_VAR} "${hash}" PARENT_SCOPE)
    set(${WORLD_VAR} "${world_results}" PARENT_SCOPE)
    set(${WORLD_HITS_VAR} "${world_hits}" PARENT_SCOPE)
    set(${SCREEN_HITS_VAR} "${screen_hits}" PARENT_SCOPE)
    set(${SCREEN_MISSES_VAR} "${screen_misses}" PARENT_SCOPE)
endfunction()

run_scene_ray_case(
    "screen trace scene-ray frame"
    screen_hash
    screen_world_results
    screen_world_hits
    screen_hits
    screen_misses
    --screen-trace-statistics
)
run_scene_ray_case(
    "world-only scene-ray frame"
    world_hash
    world_results
    world_hits
    world_screen_hits
    world_screen_misses
    --disable-screen-traces
)

if(NOT screen_hash STREQUAL world_hash)
    message(FATAL_ERROR
        "Screen-space and world-only scene-ray frames diverged: "
        "screen=${screen_hash} world=${world_hash}")
endif()
if(screen_misses EQUAL 0)
    message(FATAL_ERROR "Screen-trace run never exercised the world fallback")
endif()
if(NOT world_screen_hits EQUAL 0 OR NOT world_screen_misses EQUAL 0)
    message(FATAL_ERROR "World-only run reported screen-space results")
endif()

message(
    STATUS
    "Scene-ray screen/world frame parity passed: hash=${screen_hash} "
    "screenWorldResults=${screen_world_results} "
    "screenWorldHits=${screen_world_hits} "
    "worldResults=${world_results}")
