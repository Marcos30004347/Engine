cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainVGSlotInstallTests
  ${TEST_DIR}/SparseVoxelTerrainVGSlotInstallTests.cpp)
target_link_libraries(SparseVoxelTerrainVGSlotInstallTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainVGSlotInstall
  COMMAND SparseVoxelTerrainVGSlotInstallTests
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
set_tests_properties(
  SparseVoxelTerrainVGSlotInstall
  PROPERTIES
    TIMEOUT 60
    LABELS "svraster.terrain;virtualgeometry;cpu")
