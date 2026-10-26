cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase8Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase8Tests.cpp)
target_link_libraries(SparseVoxelTerrainPhase8Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainPhase8Qualification
  COMMAND SparseVoxelTerrainPhase8Tests)
set_tests_properties(
  SparseVoxelTerrainPhase8Qualification
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase8;svraster.terrain.qualification;cpu")

add_executable(
  SparseVoxelTerrainConvergencePhase8Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase8Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase8Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase8
  COMMAND SparseVoxelTerrainConvergencePhase8Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase8
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase8;svraster.terrain.convergence;world-map;cpu")
