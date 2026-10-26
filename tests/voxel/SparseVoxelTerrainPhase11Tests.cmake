cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainConvergencePhase11Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase11Tests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase11Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase11
  COMMAND SparseVoxelTerrainConvergencePhase11Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase11
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase11;svraster.terrain.convergence;placement;cpu")
