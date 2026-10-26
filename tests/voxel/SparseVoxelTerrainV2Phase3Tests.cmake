cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase3Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase3Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase3Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase3AtlasColumns
  COMMAND SparseVoxelTerrainV2Phase3Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase3AtlasColumns
  PROPERTIES
    TIMEOUT 60
    LABELS "svraster.terrain.v2.phase3;svraster.terrain.cpu")

add_executable(
  SparseVoxelTerrainV2Phase3GPUTests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase3GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase3GPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTerrainV2Phase3GPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainV2Phase3GPUColumns
  COMMAND SparseVoxelTerrainV2Phase3GPUTests ${CMAKE_SOURCE_DIR}/world-data)
set_tests_properties(
  SparseVoxelTerrainV2Phase3GPUColumns
  PROPERTIES
    TIMEOUT 240
    LABELS "svraster.terrain.v2.phase3;svraster.terrain.gpu;svraster.terrain.world-map;svraster.terrain.cpu-parity;svraster.terrain.slope")
