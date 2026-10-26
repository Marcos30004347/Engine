cmake_minimum_required(VERSION 3.10)

project (ConcurrentSkipListMapTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentSkipListMapTests ${TEST_DIR}/ConcurrentSkipListMapTests.cpp)
target_link_libraries(ConcurrentSkipListMapTests PRIVATE Engine)
add_test(NAME ConcurrentSkipListMapTests COMMAND ConcurrentSkipListMapTests)