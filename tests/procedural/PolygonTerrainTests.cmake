cmake_minimum_required(VERSION 3.10)

project(PolygonTerrainTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(PolygonTerrainTests ${TEST_DIR}/PolygonTerrainTests.cpp)
target_link_libraries(PolygonTerrainTests PRIVATE Engine lz4)
add_test(NAME PolygonTerrainTests COMMAND PolygonTerrainTests)
