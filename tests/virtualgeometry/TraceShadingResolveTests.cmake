cmake_minimum_required(VERSION 3.10)

project(TraceShadingResolveTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(TraceShadingResolveTests ${TEST_DIR}/TraceShadingResolveTests.cpp)
target_link_libraries(TraceShadingResolveTests PRIVATE Engine)
add_test(NAME TraceShadingResolveTests COMMAND TraceShadingResolveTests)
