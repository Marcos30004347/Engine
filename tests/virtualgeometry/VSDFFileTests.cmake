cmake_minimum_required(VERSION 3.10)

project(VSDFFileTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(VSDFFileTests ${TEST_DIR}/VSDFFileTests.cpp)
target_link_libraries(VSDFFileTests PRIVATE Engine)
add_test(NAME VSDFFileTests COMMAND VSDFFileTests)

