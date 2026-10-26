cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase1Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase1Tests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainPhase1Tests PRIVATE Engine)

add_executable(
  SparseVoxelTerrainConvergencePhase1Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase1Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase1Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainPhase1CanonicalColumns
  COMMAND SparseVoxelTerrainPhase1Tests)
set_tests_properties(
  SparseVoxelTerrainPhase1CanonicalColumns
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase1;svraster.terrain.cpu;world-map")

add_test(
  NAME SparseVoxelTerrainConvergencePhase1
  COMMAND SparseVoxelTerrainConvergencePhase1Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase1
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase1;svraster.terrain.convergence;svraster.streaming")
