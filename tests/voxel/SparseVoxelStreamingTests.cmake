cmake_minimum_required(VERSION 3.10)

project(SparseVoxelStreamingTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SparseVoxelStreamingTests ${TEST_DIR}/SparseVoxelStreamingTests.cpp)
target_link_libraries(SparseVoxelStreamingTests PRIVATE Engine)

add_executable(SparseVoxelStreamingGPUTests ${TEST_DIR}/SparseVoxelStreamingGPUTests.cpp)
target_link_libraries(SparseVoxelStreamingGPUTests PRIVATE Engine)
add_dependencies(SparseVoxelStreamingGPUTests SparseVoxelRasterShaders)

add_test(NAME SparseVoxelStreamingG16Tests COMMAND SparseVoxelStreamingTests)
set_tests_properties(SparseVoxelStreamingG16Tests PROPERTIES LABELS "svraster.g16;svraster.m2")

add_test(NAME SparseVoxelStreamingGPUContractG16Tests COMMAND SparseVoxelStreamingGPUTests)
set_tests_properties(SparseVoxelStreamingGPUContractG16Tests PROPERTIES LABELS "svraster.g16;svraster.m2;gpu")
