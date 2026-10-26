cmake_minimum_required(VERSION 3.10)

project (JobTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(JobTests ${TEST_DIR}/JobTests.cpp)
target_link_libraries(JobTests PRIVATE Engine)
add_test(NAME JobTests COMMAND JobTests)
