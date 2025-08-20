cmake_minimum_required(VERSION 3.10)

project (GPUHeapTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GPUHeapTests ${TEST_DIR}/GPUHeapTests.cpp)
target_link_libraries(GPUHeapTests PRIVATE Engine)
add_test(NAME GPUHeapTests COMMAND GPUHeapTests)