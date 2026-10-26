cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainTrianglePhase4Tests
  ${TEST_DIR}/SparseVoxelTerrainTrianglePhase4Tests.cpp)
target_link_libraries(SparseVoxelTerrainTrianglePhase4Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainTrianglePhase4Culling
  COMMAND SparseVoxelTerrainTrianglePhase4Tests)
set_tests_properties(
  SparseVoxelTerrainTrianglePhase4Culling
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.phase4;svraster.culling;cpu")
