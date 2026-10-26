cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase5Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase5Tests.cpp)
target_link_libraries(SparseVoxelTerrainPhase5Tests PRIVATE Engine)
add_dependencies(SparseVoxelTerrainPhase5Tests terrain_gpu_joint_publication)

add_test(
  NAME SparseVoxelTerrainPhase5PublicationContract
  COMMAND SparseVoxelTerrainPhase5Tests)
set_tests_properties(
  SparseVoxelTerrainPhase5PublicationContract
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase5;svraster.terrain.publication;cpu")

add_executable(
  SparseVoxelTerrainConvergencePhase5Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase5Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase5Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase5
  COMMAND SparseVoxelTerrainConvergencePhase5Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase5
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase5;svraster.terrain.convergence;svraster.streaming;cpu")
