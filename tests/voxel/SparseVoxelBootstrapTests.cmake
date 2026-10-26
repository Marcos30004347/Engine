cmake_minimum_required(VERSION 3.10)

project(SparseVoxelBootstrapTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SparseVoxelBootstrapTests ${TEST_DIR}/SparseVoxelBootstrapTests.cpp)
target_link_libraries(SparseVoxelBootstrapTests PRIVATE Engine)
add_dependencies(SparseVoxelBootstrapTests SparseVoxelRasterShaders)

add_test(NAME SparseVoxelBootstrapG1Tests COMMAND SparseVoxelBootstrapTests --g1)
set_tests_properties(SparseVoxelBootstrapG1Tests PROPERTIES LABELS "svraster.g01;svraster.m1")

add_test(NAME SparseVoxelBootstrapG2Tests COMMAND SparseVoxelBootstrapTests --g2)
set_tests_properties(SparseVoxelBootstrapG2Tests PROPERTIES LABELS "svraster.g02;svraster.m1")

add_test(NAME SparseVoxelProducerSwapG12Tests COMMAND SparseVoxelBootstrapTests --g12)
set_tests_properties(SparseVoxelProducerSwapG12Tests PROPERTIES LABELS "svraster.g12;svraster.m1;gpu")
