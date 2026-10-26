cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase4Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase4Tests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainPhase4Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainPhase4Tree64Contract
  COMMAND SparseVoxelTerrainPhase4Tests)
set_tests_properties(
  SparseVoxelTerrainPhase4Tree64Contract
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase4;svraster.terrain.tree64;cpu")

add_executable(
  SparseVoxelTerrainPhase4GPUTests
  ${TEST_DIR}/SparseVoxelTerrainPhase4GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainPhase4GPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTerrainPhase4GPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainPhase4Tree64GPU
  COMMAND SparseVoxelTerrainPhase4GPUTests)
set_tests_properties(
  SparseVoxelTerrainPhase4Tree64GPU
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase4;svraster.terrain.tree64;gpu")

add_executable(
  SparseVoxelTerrainConvergencePhase4Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase4Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase4Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase4
  COMMAND SparseVoxelTerrainConvergencePhase4Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase4
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase4;svraster.terrain.convergence;svraster.streaming;cpu")
