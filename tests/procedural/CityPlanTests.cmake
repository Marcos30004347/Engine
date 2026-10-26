cmake_minimum_required(VERSION 3.10)

project (CityPlanTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(CityPlanTests ${TEST_DIR}/CityPlanTests.cpp)
target_link_libraries(CityPlanTests PRIVATE Engine)
target_include_directories(CityPlanTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityPlanTests COMMAND CityPlanTests ${CMAKE_BINARY_DIR}/tests/city-wfc)
set_tests_properties(CityPlanTests PROPERTIES TIMEOUT 300 LABELS "world-gen;city-wfc")
