cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVirtualGeometryPhase12Tests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryPhase12Tests.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryPhase12Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVirtualGeometryPhase12BenchmarkGate
  COMMAND SparseVoxelTerrainVirtualGeometryPhase12Tests)
set_tests_properties(
  SparseVoxelTerrainVirtualGeometryPhase12BenchmarkGate
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.virtualgeometry;svraster.terrain.phase12;svraster.renderer;svraster.benchmark;cpu")
