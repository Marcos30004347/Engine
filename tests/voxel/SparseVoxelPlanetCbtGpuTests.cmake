cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(
  SparseVoxelPlanetCbtGpuTests
  ${TEST_DIR}/SparseVoxelPlanetCbtGpuTests.cpp)
target_link_libraries(SparseVoxelPlanetCbtGpuTests PRIVATE Engine)
add_dependencies(
  SparseVoxelPlanetCbtGpuTests
  sparse_voxel_planet_cbt_allocator
  sparse_voxel_planet_cbt_update)
add_test(
  NAME SparseVoxelPlanetCbtPhase2GPU
  COMMAND SparseVoxelPlanetCbtGpuTests)
set_tests_properties(
  SparseVoxelPlanetCbtPhase2GPU
  PROPERTIES LABELS "svraster.planet.cbt.phase2;gpu")
