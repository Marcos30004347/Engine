cmake_minimum_required(VERSION 3.10)

project (CityGridTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(CityGridTests ${TEST_DIR}/CityGridTests.cpp)
target_link_libraries(CityGridTests PRIVATE Engine)
target_include_directories(CityGridTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityGridTests COMMAND CityGridTests ${CMAKE_BINARY_DIR}/tests/city-wfc)
set_tests_properties(CityGridTests PROPERTIES TIMEOUT 300 LABELS "world-gen;city-wfc")
