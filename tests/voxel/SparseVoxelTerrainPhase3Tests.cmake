cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase3Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase3Tests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainPhase3Tests PRIVATE Engine)

add_executable(
  SparseVoxelTerrainConvergencePhase3Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase3Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase3Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainPhase3RasterContract
  COMMAND SparseVoxelTerrainPhase3Tests)
set_tests_properties(
  SparseVoxelTerrainPhase3RasterContract
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase3;svraster.terrain.raster;cpu")

add_test(
  NAME SparseVoxelTerrainConvergencePhase3
  COMMAND SparseVoxelTerrainConvergencePhase3Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase3
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase3;svraster.terrain.convergence;svraster.streaming;cpu")

add_executable(
  SparseVoxelTerrainPhase3GPUTests
  ${TEST_DIR}/SparseVoxelTerrainPhase3GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainPhase3GPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTerrainPhase3GPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainPhase3RasterGPU
  COMMAND SparseVoxelTerrainPhase3GPUTests)
set_tests_properties(
  SparseVoxelTerrainPhase3RasterGPU
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase3;svraster.terrain.raster;gpu")
