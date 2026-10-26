cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVirtualGeometryPhase11Tests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryPhase11Tests.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryPhase11Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVirtualGeometryPhase11RenderRoute
  COMMAND SparseVoxelTerrainVirtualGeometryPhase11Tests)
set_tests_properties(
  SparseVoxelTerrainVirtualGeometryPhase11RenderRoute
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.virtualgeometry;svraster.terrain.phase11;svraster.renderer;cpu")
