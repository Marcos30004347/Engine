cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainTrianglePhase2Tests
  ${TEST_DIR}/SparseVoxelTerrainTrianglePhase2Tests.cpp)
target_link_libraries(SparseVoxelTerrainTrianglePhase2Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainTrianglePhase2Streaming
  COMMAND SparseVoxelTerrainTrianglePhase2Tests)
set_tests_properties(
  SparseVoxelTerrainTrianglePhase2Streaming
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.phase2;svraster.streaming;cpu")
