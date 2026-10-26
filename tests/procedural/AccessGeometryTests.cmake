cmake_minimum_required(VERSION 3.10)

project (AccessGeometryTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(AccessGeometryTests ${TEST_DIR}/AccessGeometryTests.cpp)
target_link_libraries(AccessGeometryTests PRIVATE Engine)
add_test(NAME AccessGeometryTests COMMAND AccessGeometryTests ${CMAKE_BINARY_DIR}/tests/access-geometry.json)
set_tests_properties(AccessGeometryTests PROPERTIES LABELS "world-gen;dwellings;access")
