cmake_minimum_required(VERSION 3.10)

project (ConcurrentPriorityQueueTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentPriorityQueueTests ${TEST_DIR}/ConcurrentPriorityQueueTests.cpp)
target_link_libraries(ConcurrentPriorityQueueTests PRIVATE Engine)
add_test(NAME ConcurrentPriorityQueueTests COMMAND ConcurrentPriorityQueueTests)