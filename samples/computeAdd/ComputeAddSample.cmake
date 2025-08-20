cmake_minimum_required(VERSION 3.10)

project (ComputeAddSample)
get_filename_component(SAMPLE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ComputeAddSample ${SAMPLE_DIR}/ComputeAddSample.cpp)
target_link_libraries(ComputeAddSample PRIVATE Engine)
add_test(NAME ComputeAddSample COMMAND ComputeAddSample)
