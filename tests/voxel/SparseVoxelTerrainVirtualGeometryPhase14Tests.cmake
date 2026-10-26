cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVirtualGeometryPhase14Tests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryPhase14Tests.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryPhase14Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVirtualGeometryPhase14SceneRuntimeInstall
  COMMAND SparseVoxelTerrainVirtualGeometryPhase14Tests)
set_tests_properties(
  SparseVoxelTerrainVirtualGeometryPhase14SceneRuntimeInstall
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.virtualgeometry;svraster.terrain.phase14;svraster.streaming;gpu")
