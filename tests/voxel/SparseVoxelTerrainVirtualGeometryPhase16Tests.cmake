cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVirtualGeometryPhase16Tests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryPhase16Tests.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryPhase16Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVirtualGeometryPhase16BenchmarkGate
  COMMAND SparseVoxelTerrainVirtualGeometryPhase16Tests)
set_tests_properties(
  SparseVoxelTerrainVirtualGeometryPhase16BenchmarkGate
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.virtualgeometry;svraster.terrain.phase16;svraster.renderer;svraster.benchmark;cpu")
