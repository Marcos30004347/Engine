cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVirtualGeometryRefinementTests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryRefinementTests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryRefinementTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVirtualGeometryRuntimeRefinement
  COMMAND SparseVoxelTerrainVirtualGeometryRefinementTests)
set_tests_properties(
  SparseVoxelTerrainVirtualGeometryRuntimeRefinement
  PROPERTIES
    TIMEOUT 300
    LABELS "svraster.terrain.virtualgeometry;svraster.streaming;cpu")
