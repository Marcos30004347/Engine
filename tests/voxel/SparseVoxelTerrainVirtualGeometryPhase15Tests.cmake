cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVirtualGeometryPhase15Tests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryPhase15Tests.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryPhase15Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVirtualGeometryPhase15GBufferPublisherGate
  COMMAND SparseVoxelTerrainVirtualGeometryPhase15Tests)
set_tests_properties(
  SparseVoxelTerrainVirtualGeometryPhase15GBufferPublisherGate
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.virtualgeometry;svraster.terrain.phase15;svraster.renderer;cpu")
