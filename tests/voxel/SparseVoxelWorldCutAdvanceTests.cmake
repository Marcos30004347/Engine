cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(SparseVoxelWorldCutAdvanceTests ${TEST_DIR}/SparseVoxelWorldCutAdvanceTests.cpp)
target_link_libraries(SparseVoxelWorldCutAdvanceTests PRIVATE Engine)
add_test(NAME SparseVoxelWorldCutAdvanceTests COMMAND SparseVoxelWorldCutAdvanceTests)
set_tests_properties(
  SparseVoxelWorldCutAdvanceTests
  PROPERTIES LABELS "svraster.terrain.cpu;svraster.streaming;world-map")
