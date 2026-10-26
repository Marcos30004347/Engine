cmake_minimum_required(VERSION 3.10)

project (AsyncTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(AsyncTests ${TEST_DIR}/AsyncTests.cpp)
target_link_libraries(AsyncTests PRIVATE Engine)
add_test(NAME AsyncTests COMMAND AsyncTests)
