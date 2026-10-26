cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  TerrainVirtualGeometrySceneScheduleTests
  ${TEST_DIR}/TerrainVirtualGeometrySceneScheduleTests.cpp)
target_link_libraries(TerrainVirtualGeometrySceneScheduleTests PRIVATE Engine)

add_test(
  NAME TerrainVirtualGeometrySceneSchedule
  COMMAND TerrainVirtualGeometrySceneScheduleTests)
set_tests_properties(
  TerrainVirtualGeometrySceneSchedule
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.virtualgeometry;virtualgeometry;virtualgeometry.streaming;cpu")
