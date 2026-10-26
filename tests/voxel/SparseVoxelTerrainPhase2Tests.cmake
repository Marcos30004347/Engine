cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase2Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase2Tests.cpp)
target_link_libraries(SparseVoxelTerrainPhase2Tests PRIVATE Engine)

add_executable(
  SparseVoxelTerrainConvergencePhase2Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase2Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase2Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainPhase2Scheduler
  COMMAND SparseVoxelTerrainPhase2Tests)
set_tests_properties(
  SparseVoxelTerrainPhase2Scheduler
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase2;svraster.terrain.scheduler;cpu")

add_test(
  NAME SparseVoxelTerrainConvergencePhase2
  COMMAND SparseVoxelTerrainConvergencePhase2Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase2
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase2;svraster.terrain.convergence;svraster.streaming;cpu")

add_executable(
  SparseVoxelTerrainPhase2GPUTests
  ${TEST_DIR}/SparseVoxelTerrainPhase2GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainPhase2GPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTerrainPhase2GPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainPhase2SyntheticGPU
  COMMAND SparseVoxelTerrainPhase2GPUTests)
set_tests_properties(
  SparseVoxelTerrainPhase2SyntheticGPU
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase2;svraster.terrain.synthetic;gpu")
