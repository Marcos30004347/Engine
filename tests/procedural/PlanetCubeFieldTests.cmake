cmake_minimum_required(VERSION 3.10)

project(PlanetCubeFieldTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(PlanetCubeFieldTests ${TEST_DIR}/PlanetCubeFieldTests.cpp)
target_link_libraries(PlanetCubeFieldTests PRIVATE Engine)
add_test(NAME PlanetCubeFieldTests COMMAND PlanetCubeFieldTests)
