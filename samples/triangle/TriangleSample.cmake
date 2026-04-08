cmake_minimum_required(VERSION 3.10)

project (TriangleSample)
get_filename_component(SAMPLE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(TriangleSample ${SAMPLE_DIR}/TriangleSample.cpp)
target_link_libraries(TriangleSample PRIVATE Engine)
add_test(NAME TriangleSample COMMAND TriangleSample)
