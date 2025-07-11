cmake_minimum_required(VERSION 3.10)

project (JobSystemTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(JobSystemTests ${TEST_DIR}/JobSystemTests.cpp)
target_link_libraries(JobSystemTests PRIVATE Engine)
add_test(NAME JobSystemTests COMMAND JobSystemTests)
