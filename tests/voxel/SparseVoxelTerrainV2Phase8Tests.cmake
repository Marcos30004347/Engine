cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase8Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase8Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase8Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase8RuntimeBudget
  COMMAND SparseVoxelTerrainV2Phase8Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase8RuntimeBudget
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.v2.phase8;svraster.terrain.runtime")

file(READ
  "${CMAKE_SOURCE_DIR}/src/voxel/svraster/TerrainGPUV2Runtime.cpp"
  TERRAIN_V2_RUNTIME_SOURCE)
if(TERRAIN_V2_RUNTIME_SOURCE MATCHES "waitFrame\\(" OR
   TERRAIN_V2_RUNTIME_SOURCE MATCHES "waitIdle\\(" OR
   TERRAIN_V2_RUNTIME_SOURCE MATCHES "bufferRead\\(" OR
   TERRAIN_V2_RUNTIME_SOURCE MATCHES "readback")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 8 runtime introduced a drain or generated-payload readback")
endif()
