cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(
  SparseVoxelPlanetOverviewTests
  ${TEST_DIR}/SparseVoxelPlanetOverviewTests.cpp)
target_link_libraries(SparseVoxelPlanetOverviewTests PRIVATE Engine)
add_test(
  NAME SparseVoxelPlanetOverviewTests
  COMMAND SparseVoxelPlanetOverviewTests)
set_tests_properties(
  SparseVoxelPlanetOverviewTests
  PROPERTIES LABELS
  "svraster.navigation.cpu;svraster.planet.phase2;svraster.planet.phase7")
