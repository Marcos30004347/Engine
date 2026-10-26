cmake_minimum_required(VERSION 3.10)

project(FallbackMeshFileTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(FallbackMeshFileTests ${TEST_DIR}/FallbackMeshFileTests.cpp)
target_link_libraries(FallbackMeshFileTests PRIVATE Engine)
add_test(NAME FallbackMeshFileTests COMMAND FallbackMeshFileTests)
