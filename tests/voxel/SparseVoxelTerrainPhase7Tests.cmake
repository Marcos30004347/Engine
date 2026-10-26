cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase7Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase7Tests.cpp)
target_link_libraries(SparseVoxelTerrainPhase7Tests PRIVATE Engine)
add_dependencies(SparseVoxelTerrainPhase7Tests terrain_gpu_column_build)

add_test(
  NAME SparseVoxelTerrainPhase7GPUColumns
  COMMAND SparseVoxelTerrainPhase7Tests)
set_tests_properties(
  SparseVoxelTerrainPhase7GPUColumns
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase7;svraster.terrain.columns;cpu")

add_executable(
  SparseVoxelTerrainPhase7GPUTests
  ${TEST_DIR}/SparseVoxelTerrainPhase7GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainPhase7GPUTests PRIVATE Engine)
add_dependencies(SparseVoxelTerrainPhase7GPUTests SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainPhase7GPUAtlasDifferential
  COMMAND SparseVoxelTerrainPhase7GPUTests)
set_tests_properties(
  SparseVoxelTerrainPhase7GPUAtlasDifferential
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase7;svraster.terrain.columns;gpu")

add_executable(
  SparseVoxelTerrainConvergencePhase7Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase7Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase7Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase7
  COMMAND SparseVoxelTerrainConvergencePhase7Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase7
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase7;svraster.terrain.convergence;world-map;cpu")
