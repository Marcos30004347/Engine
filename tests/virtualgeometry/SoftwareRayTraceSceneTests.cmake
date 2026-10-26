cmake_minimum_required(VERSION 3.10)

project(SoftwareRayTraceSceneTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SoftwareRayTraceSceneTests ${TEST_DIR}/SoftwareRayTraceSceneTests.cpp)
target_link_libraries(SoftwareRayTraceSceneTests PRIVATE Engine)
add_test(NAME SoftwareRayTraceSceneTests COMMAND SoftwareRayTraceSceneTests)
