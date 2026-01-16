cmake_minimum_required(VERSION 3.10)

project (TriangleExample)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(TriangleExample ${TEST_DIR}/TriangleExample.cpp)
target_link_libraries(TriangleExample PRIVATE Engine)
add_test(NAME TriangleExample COMMAND TriangleExample)