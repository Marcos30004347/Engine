cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(SparseVoxelWorldTransientPageTests ${TEST_DIR}/SparseVoxelWorldTransientPageTests.cpp)
target_link_libraries(SparseVoxelWorldTransientPageTests PRIVATE Engine)
add_test(NAME SparseVoxelWorldTransientPageTests COMMAND SparseVoxelWorldTransientPageTests)
set_tests_properties(
  SparseVoxelWorldTransientPageTests
  PROPERTIES LABELS "svraster.terrain.cpu;svraster.streaming;world-map")
