cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainTriangleVirtualGeometryBridgeTests
  ${TEST_DIR}/SparseVoxelTerrainTriangleVirtualGeometryBridgeTests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainTriangleVirtualGeometryBridgeTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainTriangleVirtualGeometryBridge
  COMMAND SparseVoxelTerrainTriangleVirtualGeometryBridgeTests)
set_tests_properties(
  SparseVoxelTerrainTriangleVirtualGeometryBridge
  PROPERTIES
    TIMEOUT 180
    LABELS "svraster.terrain.triangle;svraster.terrain.virtualgeometry;svraster.terrain.phase7;cpu")
