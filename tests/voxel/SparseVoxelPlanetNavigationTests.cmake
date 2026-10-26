cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(
  SparseVoxelPlanetNavigationTests
  ${TEST_DIR}/SparseVoxelPlanetNavigationTests.cpp)
target_link_libraries(SparseVoxelPlanetNavigationTests PRIVATE Engine)
add_test(
  NAME SparseVoxelPlanetNavigationTests
  COMMAND SparseVoxelPlanetNavigationTests)
set_tests_properties(
  SparseVoxelPlanetNavigationTests
  PROPERTIES LABELS
  "svraster.navigation.cpu;svraster.planet.phase1;svraster.planet.phase3;svraster.planet.phase6;svraster.planet.phase7")
