cmake_minimum_required(VERSION 3.10)

project (ConstrainedDelaunayTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConstrainedDelaunayTests ${TEST_DIR}/ConstrainedDelaunayTests.cpp)
target_link_libraries(ConstrainedDelaunayTests PRIVATE Engine)
add_test(NAME ConstrainedDelaunayTests COMMAND ConstrainedDelaunayTests)
