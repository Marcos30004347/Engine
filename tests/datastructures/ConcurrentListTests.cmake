cmake_minimum_required(VERSION 3.10)

project (ConcurrentListTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentListTests ${TEST_DIR}/ConcurrentListTests.cpp)
target_link_libraries(ConcurrentListTests PRIVATE Engine)
add_test(NAME ConcurrentListTests COMMAND ConcurrentListTests)
