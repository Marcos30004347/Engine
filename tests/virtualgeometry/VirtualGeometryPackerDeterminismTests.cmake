cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  VirtualGeometryPackerDeterminismTests
  ${TEST_DIR}/VirtualGeometryPackerDeterminismTests.cpp
  ${TEST_DIR}/../voxel/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(VirtualGeometryPackerDeterminismTests PRIVATE Engine)
# The voxel-route golden uses the terrain fixtures the voxel tests share.
target_include_directories(VirtualGeometryPackerDeterminismTests PRIVATE ${TEST_DIR}/../voxel)

add_test(
  NAME VirtualGeometryPackerDeterminism
  COMMAND VirtualGeometryPackerDeterminismTests
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set_tests_properties(
  VirtualGeometryPackerDeterminism
  PROPERTIES
    TIMEOUT 120
    LABELS "virtualgeometry;cpu")
