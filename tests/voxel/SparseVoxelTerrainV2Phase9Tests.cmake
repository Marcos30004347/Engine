cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase9Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase9Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase9Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase9GroundStability
  COMMAND SparseVoxelTerrainV2Phase9Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase9GroundStability
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.v2.phase9;svraster.terrain.stability;cpu")

# Phase 4 byte-compares every compact occupancy word and material with its CPU
# oracle; Phase 5 does the same for Tree64 masks, links, and roots. Re-run both
# binaries as Phase 9 qualification gates. This is stronger than comparing only
# the cells around a reported screen-space discontinuity.
add_test(
  NAME SparseVoxelTerrainV2Phase9GPUOccupancyQualification
  COMMAND SparseVoxelTerrainV2Phase4GPUTests ${CMAKE_SOURCE_DIR}/world-data)
set_tests_properties(
  SparseVoxelTerrainV2Phase9GPUOccupancyQualification
  PROPERTIES
    TIMEOUT 240
    LABELS "svraster.terrain.v2.phase9;svraster.terrain.stability;gpu")

add_test(
  NAME SparseVoxelTerrainV2Phase9GPUTree64Qualification
  COMMAND SparseVoxelTerrainV2Phase5GPUTests)
set_tests_properties(
  SparseVoxelTerrainV2Phase9GPUTree64Qualification
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.v2.phase9;svraster.terrain.stability;gpu")

file(READ
  "${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_ray_create.wgsl"
  TERRAIN_V2_SCENE_RAY_CREATE)
if(NOT TERRAIN_V2_SCENE_RAY_CREATE MATCHES "let rayOrigin = surfacePosition;")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 9 requires direct shadow rays to start at the exact receiver")
endif()

file(READ
  "${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_scene_ray_compact.wgsl"
  TERRAIN_V2_SCENE_RAY_COMPACT)
if(NOT TERRAIN_V2_SCENE_RAY_COMPACT MATCHES
    "let sortedOrigin = sortedRay.originMinimumDistance.xyz;")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 9 requires compaction to preserve the exact receiver origin")
endif()
if(NOT TERRAIN_V2_SCENE_RAY_COMPACT MATCHES
    "var startDistance = max\\(sortedRay.originMinimumDistance.w, 0.0\\);")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 9 permits only an explicit caller start distance")
endif()
