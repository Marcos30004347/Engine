cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase2Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase2Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase2Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase2Infrastructure
  COMMAND SparseVoxelTerrainV2Phase2Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase2Infrastructure
  PROPERTIES
    TIMEOUT 60
    LABELS "svraster.terrain.v2.phase2;svraster.terrain.cpu")

add_executable(
  SparseVoxelTerrainV2Phase2GPUTests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase2GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase2GPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTerrainV2Phase2GPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainV2Phase2GPUScan
  COMMAND SparseVoxelTerrainV2Phase2GPUTests)
set_tests_properties(
  SparseVoxelTerrainV2Phase2GPUScan
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.v2.phase2;svraster.terrain.gpu")
