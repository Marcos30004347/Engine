cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(SparseVoxelWorldPageDirectoryTests ${TEST_DIR}/SparseVoxelWorldPageDirectoryTests.cpp)
target_link_libraries(SparseVoxelWorldPageDirectoryTests PRIVATE Engine)
add_test(NAME SparseVoxelWorldPageDirectoryTests COMMAND SparseVoxelWorldPageDirectoryTests)
set_tests_properties(
  SparseVoxelWorldPageDirectoryTests
  PROPERTIES LABELS "svraster.terrain.cpu;svraster.streaming;world-map")
