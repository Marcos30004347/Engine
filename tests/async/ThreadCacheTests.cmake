cmake_minimum_required(VERSION 3.10)

project (ThreadCacheTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ThreadCacheTests ${TEST_DIR}/ThreadCacheTests.cpp)
target_link_libraries(ThreadCacheTests PRIVATE Engine)
add_test(NAME ThreadCacheTests COMMAND ThreadCacheTests)