cmake_minimum_required(VERSION 3.10)

project (SortTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SortTests ${TEST_DIR}/SortTests.cpp)
target_link_libraries(SortTests PRIVATE Engine)
add_test(NAME SortTests COMMAND SortTests)