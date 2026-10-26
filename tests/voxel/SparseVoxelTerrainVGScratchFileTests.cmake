cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVGScratchFileTests
  ${TEST_DIR}/SparseVoxelTerrainVGScratchFileTests.cpp)
target_link_libraries(SparseVoxelTerrainVGScratchFileTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVGScratchFile
  COMMAND SparseVoxelTerrainVGScratchFileTests
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set_tests_properties(
  SparseVoxelTerrainVGScratchFile
  PROPERTIES
    TIMEOUT 60
    LABELS "svraster.terrain;virtualgeometry;cpu")
