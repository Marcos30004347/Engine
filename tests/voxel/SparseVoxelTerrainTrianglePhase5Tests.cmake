cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainTrianglePhase5Tests
  ${TEST_DIR}/SparseVoxelTerrainTrianglePhase5Tests.cpp)
target_link_libraries(SparseVoxelTerrainTrianglePhase5Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainTrianglePhase5StreamingDependencies
  COMMAND SparseVoxelTerrainTrianglePhase5Tests)
set_tests_properties(
  SparseVoxelTerrainTrianglePhase5StreamingDependencies
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.phase5;svraster.streaming;svraster.dependencies;cpu")
