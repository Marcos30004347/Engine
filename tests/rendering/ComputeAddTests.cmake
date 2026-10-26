cmake_minimum_required(VERSION 3.10)

project (ComputeAddTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ComputeAddTests ${TEST_DIR}/ComputeAddTests.cpp)
target_link_libraries(ComputeAddTests PRIVATE Engine)
add_test(NAME ComputeAddTests COMMAND ComputeAddTests)