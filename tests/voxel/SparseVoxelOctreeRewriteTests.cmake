cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelOctreeRewriteTests
  ${TEST_DIR}/SparseVoxelOctreeRewriteTests.cpp)
target_link_libraries(SparseVoxelOctreeRewriteTests PRIVATE Engine)

add_test(
  NAME SparseVoxelOctreePageRewrites
  COMMAND SparseVoxelOctreeRewriteTests)
set_tests_properties(
  SparseVoxelOctreePageRewrites
  PROPERTIES
    TIMEOUT 900
    LABELS "svraster.virtualgeometry;svraster.streaming;cpu")
