cmake_minimum_required(VERSION 3.10)

project(PlanetPackageTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(PlanetPackageTests ${TEST_DIR}/PlanetPackageTests.cpp)
target_link_libraries(PlanetPackageTests PRIVATE Engine lz4)
add_test(NAME PlanetPackageTests COMMAND PlanetPackageTests)
