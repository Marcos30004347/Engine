cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase6Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase6Tests.cpp)
target_link_libraries(SparseVoxelTerrainPhase6Tests PRIVATE Engine)
add_dependencies(SparseVoxelTerrainPhase6Tests map_export_pages_test)

add_test(
  NAME SparseVoxelTerrainPhase6WorldMapAtlas
  COMMAND SparseVoxelTerrainPhase6Tests)
set_tests_properties(
  SparseVoxelTerrainPhase6WorldMapAtlas
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase6;svraster.terrain.atlas;world-map;cpu")

add_executable(
  SparseVoxelTerrainConvergencePhase6Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase6Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase6Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase6
  COMMAND SparseVoxelTerrainConvergencePhase6Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase6
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase6;svraster.terrain.convergence;svraster.streaming;cpu")
