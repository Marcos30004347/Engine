cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  TerrainPageStreamingPipelineTests
  ${TEST_DIR}/TerrainPageStreamingPipelineTests.cpp)
target_link_libraries(TerrainPageStreamingPipelineTests PRIVATE Engine)

add_test(
  NAME TerrainPageStreamingPipeline
  COMMAND TerrainPageStreamingPipelineTests
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set_tests_properties(
  TerrainPageStreamingPipeline
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain;streaming;cpu")
