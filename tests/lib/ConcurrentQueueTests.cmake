cmake_minimum_required(VERSION 3.10)

project (ConcurrentQueueTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentQueueTests ${TEST_DIR}/ConcurrentQueueTests.cpp)
target_link_libraries(ConcurrentQueueTests PRIVATE Engine)
add_test(NAME ConcurrentQueueTests COMMAND ConcurrentQueueTests)