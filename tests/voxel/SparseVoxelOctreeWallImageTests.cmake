cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelOctreeWallImageTests
  ${TEST_DIR}/SparseVoxelOctreeWallImageTests.cpp)
target_link_libraries(SparseVoxelOctreeWallImageTests PRIVATE Engine)

add_test(
  NAME SparseVoxelOctreeWallImage
  COMMAND SparseVoxelOctreeWallImageTests)
set_tests_properties(
  SparseVoxelOctreeWallImage
  PROPERTIES
    TIMEOUT 600
    LABELS "svraster.virtualgeometry;svraster.streaming;cpu")
