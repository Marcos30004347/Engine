cmake_minimum_required(VERSION 3.10)

project (ConcurrentLinkedListTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentLinkedListTests ${TEST_DIR}/ConcurrentLinkedListTests.cpp)
target_link_libraries(ConcurrentLinkedListTests PRIVATE Engine)
add_test(NAME ConcurrentLinkedListTests COMMAND ConcurrentLinkedListTests)
