cmake_minimum_required(VERSION 3.10)

project (EventLoopTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(EventLoopTests ${TEST_DIR}/EventLoopTests.cpp)
target_link_libraries(EventLoopTests PRIVATE Engine)
add_test(NAME EventLoopTests COMMAND EventLoopTests)