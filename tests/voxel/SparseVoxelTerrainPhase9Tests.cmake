cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase9Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase9Tests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainPhase9Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainPhase9Cleanup
  COMMAND SparseVoxelTerrainPhase9Tests)
set_tests_properties(
  SparseVoxelTerrainPhase9Cleanup
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase9;svraster.terrain.cleanup;cpu")

add_executable(
  SparseVoxelTerrainConvergencePhase9Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase9Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase9Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase9
  COMMAND SparseVoxelTerrainConvergencePhase9Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase9
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase9;svraster.terrain.convergence;cpu")
