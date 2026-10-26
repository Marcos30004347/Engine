cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(SparseVoxelPlanetChunkTests ${TEST_DIR}/SparseVoxelPlanetChunkTests.cpp)
target_link_libraries(SparseVoxelPlanetChunkTests PRIVATE Engine)
add_test(NAME SparseVoxelPlanetChunkTests COMMAND SparseVoxelPlanetChunkTests)
set_tests_properties(
  SparseVoxelPlanetChunkTests
  PROPERTIES LABELS "svraster.terrain.cpu;svraster.phase0;svraster.phase2;svraster.phase6;svraster.planet.phase7")
