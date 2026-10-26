cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainTrianglePhase3Tests
  ${TEST_DIR}/SparseVoxelTerrainTrianglePhase3Tests.cpp)
target_link_libraries(SparseVoxelTerrainTrianglePhase3Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainTrianglePhase3Renderer
  COMMAND SparseVoxelTerrainTrianglePhase3Tests)
set_tests_properties(
  SparseVoxelTerrainTrianglePhase3Renderer
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.phase3;svraster.renderer;cpu")
