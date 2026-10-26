cmake_minimum_required(VERSION 3.10)

project(SphericalTerrainTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SphericalTerrainTests ${TEST_DIR}/SphericalTerrainTests.cpp)
target_link_libraries(SphericalTerrainTests PRIVATE Engine)
add_test(NAME SphericalTerrainTests COMMAND SphericalTerrainTests)
