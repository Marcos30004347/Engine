cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  TerrainGPUVirtualGeometryPageSetTests
  ${TEST_DIR}/TerrainGPUVirtualGeometryPageSetTests.cpp)
target_link_libraries(TerrainGPUVirtualGeometryPageSetTests PRIVATE Engine)

add_test(
  NAME TerrainGPUVirtualGeometryPageSet
  COMMAND TerrainGPUVirtualGeometryPageSetTests
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set_tests_properties(
  TerrainGPUVirtualGeometryPageSet
  PROPERTIES
    TIMEOUT 60
    LABELS "svraster.terrain;virtualgeometry;cpu")
