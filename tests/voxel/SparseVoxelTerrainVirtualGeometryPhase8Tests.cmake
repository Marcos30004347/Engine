cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVirtualGeometryPhase8Tests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryPhase8Tests.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryPhase8Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVirtualGeometryPhase8GPUBuildContract
  COMMAND SparseVoxelTerrainVirtualGeometryPhase8Tests)
set_tests_properties(
  SparseVoxelTerrainVirtualGeometryPhase8GPUBuildContract
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.virtualgeometry;svraster.terrain.phase8;svraster.streaming;svraster.gpu-publication;cpu")
