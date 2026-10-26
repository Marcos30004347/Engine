cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(
  SparseVoxelPlanetCbtTests
  ${TEST_DIR}/SparseVoxelPlanetCbtTests.cpp)
target_link_libraries(SparseVoxelPlanetCbtTests PRIVATE Engine)
add_test(
  NAME SparseVoxelPlanetCbtPhase1CPU
  COMMAND SparseVoxelPlanetCbtTests)
set_tests_properties(
  SparseVoxelPlanetCbtPhase1CPU
  PROPERTIES LABELS "svraster.planet.cbt.phase1;cpu")
add_test(
  NAME SparseVoxelPlanetCbtPhase6CPU
  COMMAND SparseVoxelPlanetCbtTests --phase6)
set_tests_properties(
  SparseVoxelPlanetCbtPhase6CPU
  PROPERTIES LABELS "svraster.planet.cbt.phase6;cpu")
add_test(
  NAME SparseVoxelPlanetCbtPhase7CPU
  COMMAND SparseVoxelPlanetCbtTests --phase7)
set_tests_properties(
  SparseVoxelPlanetCbtPhase7CPU
  PROPERTIES LABELS "svraster.planet.cbt.phase7;cpu")
