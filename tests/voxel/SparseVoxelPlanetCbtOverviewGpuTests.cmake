cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(
  SparseVoxelPlanetCbtOverviewGpuTests
  ${TEST_DIR}/SparseVoxelPlanetCbtOverviewGpuTests.cpp)
target_link_libraries(SparseVoxelPlanetCbtOverviewGpuTests PRIVATE Engine)
add_dependencies(
  SparseVoxelPlanetCbtOverviewGpuTests
  sparse_voxel_planet_cbt_allocator
  sparse_voxel_planet_cbt_update
  sparse_voxel_planet_overview
  sparse_voxel_planet_overview_coverage)
add_test(
  NAME SparseVoxelPlanetCbtPhase4GPU
  COMMAND SparseVoxelPlanetCbtOverviewGpuTests)
set_tests_properties(
  SparseVoxelPlanetCbtPhase4GPU
  PROPERTIES LABELS "svraster.planet.cbt.phase4;gpu")
add_test(
  NAME SparseVoxelPlanetCbtPhase5GPU
  COMMAND SparseVoxelPlanetCbtOverviewGpuTests --phase5)
set_tests_properties(
  SparseVoxelPlanetCbtPhase5GPU
  PROPERTIES LABELS "svraster.planet.cbt.phase5;gpu")
add_test(
  NAME SparseVoxelPlanetCbtPhase6GPU
  COMMAND SparseVoxelPlanetCbtOverviewGpuTests --phase6)
set_tests_properties(
  SparseVoxelPlanetCbtPhase6GPU
  PROPERTIES LABELS "svraster.planet.cbt.phase6;gpu")
add_test(
  NAME SparseVoxelPlanetCbtPhase7GPU
  COMMAND SparseVoxelPlanetCbtOverviewGpuTests --phase7)
set_tests_properties(
  SparseVoxelPlanetCbtPhase7GPU
  PROPERTIES LABELS "svraster.planet.cbt.phase7;gpu")
