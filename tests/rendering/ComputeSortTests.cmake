cmake_minimum_required(VERSION 3.10)

project (ComputeSortTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

include(${CMAKE_SOURCE_DIR}/cmake/WGSL2SpirV.cmake)

wgsl2spirv(
    NAME wgsl_radixsort
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/sort/wgsl/radixsort.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/radixsort.spirv
)


# wgsl2spirv(
#     NAME wgsl_virtualgeometry_cull_multipass
#     SRC ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/wgsl/culling_multipass.wgsl
#     DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/culling_multipass.spirv
#     DEFINES FRUSTUM_CULLING USE_REVERSE_Z OCCLUSION_CULLING
#     INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/virtualgeometry/
# )

add_executable(ComputeSortTests ${TEST_DIR}/ComputeSortTests.cpp)
target_link_libraries(ComputeSortTests PRIVATE Engine)
add_test(NAME ComputeSortTests COMMAND ComputeSortTests)

add_dependencies(ComputeSortTests wgsl_radixsort)

