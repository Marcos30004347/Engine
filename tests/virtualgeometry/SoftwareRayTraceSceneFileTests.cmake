cmake_minimum_required(VERSION 3.10)

project(SoftwareRayTraceSceneFileTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SoftwareRayTraceSceneFileTests ${TEST_DIR}/SoftwareRayTraceSceneFileTests.cpp)
target_link_libraries(SoftwareRayTraceSceneFileTests PRIVATE Engine)
add_test(NAME SoftwareRayTraceSceneFileTests COMMAND SoftwareRayTraceSceneFileTests)
