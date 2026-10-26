cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVirtualGeometryPhase9Tests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryPhase9Tests.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryPhase9Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVirtualGeometryPhase9ResidencyPublication
  COMMAND SparseVoxelTerrainVirtualGeometryPhase9Tests)
set_tests_properties(
  SparseVoxelTerrainVirtualGeometryPhase9ResidencyPublication
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.virtualgeometry;svraster.terrain.phase9;svraster.streaming;svraster.gpu-publication;cpu")
