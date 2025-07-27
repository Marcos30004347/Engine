cmake_minimum_required(VERSION 3.10)

project (ThreadLocalStorageTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ThreadLocalStorageTests ${TEST_DIR}/ThreadLocalStorageTests.cpp)
target_link_libraries(ThreadLocalStorageTests PRIVATE Engine)
add_test(NAME ThreadLocalStorageTests COMMAND ThreadLocalStorageTests)