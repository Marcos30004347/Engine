cmake_minimum_required(VERSION 3.10)

project(SoftwareBVHRuntimeTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SoftwareBVHRuntimeTests ${TEST_DIR}/SoftwareBVHRuntimeTests.cpp)
target_link_libraries(SoftwareBVHRuntimeTests PRIVATE Engine)
add_dependencies(SoftwareBVHRuntimeTests SoftwareBVHShaders)
add_test(NAME SoftwareBVHRuntimeTests COMMAND SoftwareBVHRuntimeTests)
