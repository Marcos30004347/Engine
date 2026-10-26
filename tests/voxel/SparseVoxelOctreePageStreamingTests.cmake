cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelOctreePageStreamingTests
  ${TEST_DIR}/SparseVoxelOctreePageStreamingTests.cpp)
target_link_libraries(SparseVoxelOctreePageStreamingTests PRIVATE Engine)

add_test(
  NAME SparseVoxelOctreePageStreaming
  COMMAND SparseVoxelOctreePageStreamingTests)
set_tests_properties(
  SparseVoxelOctreePageStreaming
  PROPERTIES
    TIMEOUT 300
    LABELS "svraster.virtualgeometry;svraster.streaming;cpu")
