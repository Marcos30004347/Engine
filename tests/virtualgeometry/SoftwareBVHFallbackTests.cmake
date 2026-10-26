cmake_minimum_required(VERSION 3.10)

project(SoftwareBVHFallbackTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SoftwareBVHFallbackTests ${TEST_DIR}/SoftwareBVHFallbackTests.cpp)
target_link_libraries(SoftwareBVHFallbackTests PRIVATE Engine)
add_test(NAME SoftwareBVHFallbackTests COMMAND SoftwareBVHFallbackTests)
