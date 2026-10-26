cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(
  SparseVoxelPlanetPickingTests
  ${TEST_DIR}/SparseVoxelPlanetPickingTests.cpp)
target_link_libraries(SparseVoxelPlanetPickingTests PRIVATE Engine)
add_test(
  NAME SparseVoxelPlanetPickingTests
  COMMAND SparseVoxelPlanetPickingTests)
set_tests_properties(
  SparseVoxelPlanetPickingTests
  PROPERTIES LABELS
  "svraster.navigation.cpu;svraster.planet.phase4;svraster.planet.phase6")
