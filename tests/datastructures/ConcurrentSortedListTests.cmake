cmake_minimum_required(VERSION 3.10)

project (ConcurrentSortedListTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentSortedListTests ${TEST_DIR}/ConcurrentSortedListTests.cpp)
target_link_libraries(ConcurrentSortedListTests PRIVATE Engine)
add_test(NAME ConcurrentSortedListTests COMMAND ConcurrentSortedListTests)