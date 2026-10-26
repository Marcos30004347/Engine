cmake_minimum_required(VERSION 3.10)

project (Geom2QuadMeshTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(Geom2QuadMeshTests ${TEST_DIR}/Geom2QuadMeshTests.cpp)
target_link_libraries(Geom2QuadMeshTests PRIVATE Engine)
add_test(NAME Geom2QuadMeshTests COMMAND Geom2QuadMeshTests)
set_tests_properties(Geom2QuadMeshTests PROPERTIES LABELS "geom2;city-wfc")
