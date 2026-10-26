cmake_minimum_required(VERSION 3.10)

project(SparseVoxelBinningTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SparseVoxelBinningTests ${TEST_DIR}/SparseVoxelBinningTests.cpp)
target_link_libraries(SparseVoxelBinningTests PRIVATE Engine)
add_dependencies(SparseVoxelBinningTests SparseVoxelRasterShaders)

foreach(GATE RANGE 3 9)
  add_test(NAME SparseVoxelBinningG${GATE}Tests COMMAND SparseVoxelBinningTests --g${GATE})
  set(GATE_LABEL "g0${GATE}")
  set_tests_properties(SparseVoxelBinningG${GATE}Tests PROPERTIES LABELS "svraster.${GATE_LABEL};svraster.m1")
endforeach()

add_test(NAME SparseVoxelDepthG19Tests COMMAND SparseVoxelBinningTests --g19)
set_tests_properties(SparseVoxelDepthG19Tests PROPERTIES LABELS "svraster.g19;svraster.m3;gpu")
