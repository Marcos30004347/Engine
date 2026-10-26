cmake_minimum_required(VERSION 3.10)

project (RoofSkeletonTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(RoofSkeletonTests ${TEST_DIR}/RoofSkeletonTests.cpp)
target_link_libraries(RoofSkeletonTests PRIVATE Engine)
add_test(NAME RoofSkeletonTests COMMAND RoofSkeletonTests)
set_tests_properties(RoofSkeletonTests PROPERTIES LABELS "world-gen;geom2;roofs")
