cmake_minimum_required(VERSION 3.10)

project (ConcurrentStackTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentStackTests ${TEST_DIR}/ConcurrentStackTests.cpp)
target_link_libraries(ConcurrentStackTests PRIVATE Engine)
add_test(NAME ConcurrentStackTests COMMAND ConcurrentStackTests)