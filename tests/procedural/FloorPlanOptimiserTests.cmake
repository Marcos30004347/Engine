cmake_minimum_required(VERSION 3.10)

project (FloorPlanOptimiserTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(FloorPlanOptimiserTests ${TEST_DIR}/FloorPlanOptimiserTests.cpp)
target_link_libraries(FloorPlanOptimiserTests PRIVATE Engine)
add_test(NAME FloorPlanOptimiserTests COMMAND FloorPlanOptimiserTests)
set_tests_properties(FloorPlanOptimiserTests PROPERTIES LABELS "world-gen;dwellings;programme")
