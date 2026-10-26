cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainConvergencePhase12Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase12Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase12Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase12
  COMMAND SparseVoxelTerrainConvergencePhase12Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase12
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase12;svraster.terrain.convergence;cpu")

add_test(
  NAME SparseVoxelVirtualTraceBenchmarkSmoke
  COMMAND SparseVoxelTerrainConvergencePhase12Tests)
set_tests_properties(
  SparseVoxelVirtualTraceBenchmarkSmoke
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.virtual-trace;svraster.terrain.benchmark;cpu")
