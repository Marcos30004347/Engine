cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainConvergencePhase10Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase10Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase10Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase10
  COMMAND SparseVoxelTerrainConvergencePhase10Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase10
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase10;svraster.terrain.convergence;svraster.tree64.streaming;cpu")
