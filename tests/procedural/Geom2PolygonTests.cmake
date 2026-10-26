cmake_minimum_required(VERSION 3.10)

project (Geom2PolygonTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(Geom2PolygonTests ${TEST_DIR}/Geom2PolygonTests.cpp)
target_link_libraries(Geom2PolygonTests PRIVATE Engine)
add_test(NAME Geom2PolygonTests COMMAND Geom2PolygonTests)
