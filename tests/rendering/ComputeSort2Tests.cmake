cmake_minimum_required(VERSION 3.10)

project (ComputeSort2Tests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

include(${CMAKE_SOURCE_DIR}/cmake/WGSL2SpirV.cmake)

wgsl2spirv(
    NAME wgsl_radixsort2
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/sort/wgsl/radixsort2.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/radixsort2.spirv
)

add_executable(ComputeSort2Tests ${TEST_DIR}/ComputeSort2Tests.cpp)
target_link_libraries(ComputeSort2Tests PRIVATE Engine)
add_test(NAME ComputeSort2Tests COMMAND ComputeSort2Tests)

add_dependencies(ComputeSort2Tests wgsl_radixsort2)
