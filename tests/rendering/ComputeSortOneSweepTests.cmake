cmake_minimum_required(VERSION 3.10)

project (ComputeSortOneSweepTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

include(${CMAKE_SOURCE_DIR}/cmake/WGSL2SpirV.cmake)

wgsl2spirv(
    NAME wgsl_onesweep
    SRC ${CMAKE_SOURCE_DIR}/assets/shaders/sort/wgsl/onesweep.wgsl
    DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/onesweep.spirv
)

add_executable(ComputeSortOneSweepTests ${TEST_DIR}/ComputeSortOneSweepTests.cpp)
target_link_libraries(ComputeSortOneSweepTests PRIVATE Engine)
add_test(NAME ComputeSortOneSweepTests COMMAND ComputeSortOneSweepTests)

add_dependencies(ComputeSortOneSweepTests wgsl_onesweep)
