cmake_minimum_required(VERSION 3.10)

project (ConcurrentHashMapTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentHashMapTests ${TEST_DIR}/ConcurrentHashMapTests.cpp)
target_link_libraries(ConcurrentHashMapTests PRIVATE Engine)
add_test(NAME ConcurrentHashMapTests COMMAND ConcurrentHashMapTests)
