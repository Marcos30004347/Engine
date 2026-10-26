cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainTrianglePhase6Tests
  ${TEST_DIR}/SparseVoxelTerrainTrianglePhase6Tests.cpp)
target_link_libraries(SparseVoxelTerrainTrianglePhase6Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainTrianglePhase6GPUBuildPublication
  COMMAND SparseVoxelTerrainTrianglePhase6Tests)
set_tests_properties(
  SparseVoxelTerrainTrianglePhase6GPUBuildPublication
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.triangle;svraster.terrain.phase6;svraster.streaming;svraster.gpu-publication;cpu")
