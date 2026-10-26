cmake_minimum_required(VERSION 3.10)

project(SphericalVoronoiMeshTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SphericalVoronoiMeshTests ${TEST_DIR}/SphericalVoronoiMeshTests.cpp)
target_link_libraries(SphericalVoronoiMeshTests PRIVATE Engine)
add_test(NAME SphericalVoronoiMeshTests COMMAND SphericalVoronoiMeshTests)
