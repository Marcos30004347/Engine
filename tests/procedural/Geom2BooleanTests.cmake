cmake_minimum_required(VERSION 3.10)

project (Geom2BooleanTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(Geom2BooleanTests ${TEST_DIR}/Geom2BooleanTests.cpp)
target_link_libraries(Geom2BooleanTests PRIVATE Engine)
add_test(NAME Geom2BooleanTests COMMAND Geom2BooleanTests)
