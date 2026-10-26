cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(SparseVoxelTerrainPageDeterminismTests ${TEST_DIR}/SparseVoxelTerrainPageDeterminismTests.cpp)
target_link_libraries(SparseVoxelTerrainPageDeterminismTests PRIVATE Engine)
add_test(NAME SparseVoxelTerrainPageDeterminismTests COMMAND SparseVoxelTerrainPageDeterminismTests)
set_tests_properties(
  SparseVoxelTerrainPageDeterminismTests
  PROPERTIES LABELS "svraster.terrain.cpu;svraster.streaming;world-map")
