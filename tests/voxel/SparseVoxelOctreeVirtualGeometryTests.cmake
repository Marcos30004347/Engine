cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelOctreeVirtualGeometryTests
  ${TEST_DIR}/SparseVoxelOctreeVirtualGeometryTests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelOctreeVirtualGeometryTests PRIVATE Engine)

add_test(
  NAME SparseVoxelOctreeVirtualGeometryBuild
  COMMAND SparseVoxelOctreeVirtualGeometryTests)
set_tests_properties(
  SparseVoxelOctreeVirtualGeometryBuild
  PROPERTIES
    TIMEOUT 300
    LABELS "svraster.terrain.triangle;svraster.virtualgeometry;cpu")
