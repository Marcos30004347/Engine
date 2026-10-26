cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVirtualGeometryPhase10Tests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryPhase10Tests.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryPhase10Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVirtualGeometryPhase10SceneBridge
  COMMAND SparseVoxelTerrainVirtualGeometryPhase10Tests)
set_tests_properties(
  SparseVoxelTerrainVirtualGeometryPhase10SceneBridge
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.virtualgeometry;svraster.terrain.phase10;svraster.streaming;cpu")
