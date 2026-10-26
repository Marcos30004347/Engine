cmake_minimum_required(VERSION 3.10)

project (TriangleExample)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

include(${CMAKE_SOURCE_DIR}/cmake/HLSL2SpirV.cmake)

hlsl2spirv(
    NAME hlsl_test_triangle_vertex
    SRC ${CMAKE_SOURCE_DIR}/tests/rendering/assets/shaders/triangle/hlsl/vertex.hlsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/triangle/spirv/vertex.spirv
)

hlsl2spirv(
    NAME hlsl_test_triangle_fragment
    SRC ${CMAKE_SOURCE_DIR}/tests/rendering/assets/shaders/triangle/hlsl/fragment.hlsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/triangle/spirv/fragment.spirv
)

add_executable(TriangleExample ${TEST_DIR}/TriangleExample.cpp)
target_link_libraries(TriangleExample PRIVATE Engine)
# Not a ctest: this is an interactive windowed sample whose loop ends only
# when the window is closed, so ctest would hang on it forever. The target
# stays buildable and can be run by hand.

add_dependencies(TriangleExample hlsl_test_triangle_vertex)
add_dependencies(TriangleExample hlsl_test_triangle_fragment)