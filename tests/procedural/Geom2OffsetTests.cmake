cmake_minimum_required(VERSION 3.10)

project (Geom2OffsetTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(Geom2OffsetTests ${TEST_DIR}/Geom2OffsetTests.cpp)
target_link_libraries(Geom2OffsetTests PRIVATE Engine)
add_test(NAME Geom2OffsetTests COMMAND Geom2OffsetTests)
