cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(
  SparseVoxelPlanetCbtTopologyGpuTests
  ${TEST_DIR}/SparseVoxelPlanetCbtTopologyGpuTests.cpp)
target_link_libraries(SparseVoxelPlanetCbtTopologyGpuTests PRIVATE Engine)
add_dependencies(
  SparseVoxelPlanetCbtTopologyGpuTests
  sparse_voxel_planet_cbt_allocator
  sparse_voxel_planet_cbt_update)
add_test(
  NAME SparseVoxelPlanetCbtPhase3GPU
  COMMAND SparseVoxelPlanetCbtTopologyGpuTests)
set_tests_properties(
  SparseVoxelPlanetCbtPhase3GPU
  PROPERTIES LABELS "svraster.planet.cbt.phase3;gpu")
