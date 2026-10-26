cmake_minimum_required(VERSION 3.10)

project (TriangleReverseZ)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

include(${CMAKE_SOURCE_DIR}/cmake/HLSL2SpirV.cmake)

hlsl2spirv(
    NAME hlsl_test_triangle_vertex_reverseZ
    SRC ${CMAKE_SOURCE_DIR}/tests/rendering/assets/shaders/triangle/hlsl/vertexReverseZ.hlsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/triangle/spirv/vertexReverseZ.spirv
)

add_executable(TriangleReverseZ ${TEST_DIR}/TriangleReverseZ.cpp)
target_link_libraries(TriangleReverseZ PRIVATE Engine)
add_test(NAME TriangleReverseZ COMMAND TriangleReverseZ)

add_dependencies(TriangleReverseZ hlsl_test_triangle_vertex_reverseZ)
add_dependencies(TriangleReverseZ hlsl_test_triangle_fragment)