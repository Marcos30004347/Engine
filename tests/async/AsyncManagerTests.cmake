cmake_minimum_required(VERSION 3.10)

project (AsyncManagerTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(AsyncManagerTests ${TEST_DIR}/AsyncManagerTests.cpp)
target_link_libraries(AsyncManagerTests PRIVATE Engine)
add_test(NAME AsyncManagerTests COMMAND AsyncManagerTests)
