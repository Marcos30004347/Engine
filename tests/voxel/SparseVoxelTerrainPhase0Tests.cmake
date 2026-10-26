cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainPhase0Tests
  ${TEST_DIR}/SparseVoxelTerrainPhase0Tests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainPhase0Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainPhase0CPU
  COMMAND SparseVoxelTerrainPhase0Tests)
set_tests_properties(
  SparseVoxelTerrainPhase0CPU
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase0;svraster.terrain.cpu;world-map")

add_executable(
  SparseVoxelTerrainConvergencePhase0Tests
  ${TEST_DIR}/SparseVoxelTerrainConvergencePhase0Tests.cpp)
target_link_libraries(SparseVoxelTerrainConvergencePhase0Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainConvergencePhase0
  COMMAND SparseVoxelTerrainConvergencePhase0Tests)
set_tests_properties(
  SparseVoxelTerrainConvergencePhase0
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase0;svraster.terrain.convergence;svraster.g16")

add_executable(
  SparseVoxelTerrainPhase0Benchmark
  ${TEST_DIR}/SparseVoxelTerrainPhase0Benchmark.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainPhase0Benchmark PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainPhase0BenchmarkSmoke
  COMMAND ${CMAKE_COMMAND}
    -DBENCHMARK=$<TARGET_FILE:SparseVoxelTerrainPhase0Benchmark>
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-generation-phase0-smoke.json
    -P ${TEST_DIR}/SparseVoxelTerrainPhase0BenchmarkSmoke.cmake)
set_tests_properties(
  SparseVoxelTerrainPhase0BenchmarkSmoke
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase0;svraster.terrain.benchmark;world-map")

add_executable(
  SparseVoxelTerrainTriangleMeshingTests
  ${TEST_DIR}/SparseVoxelTerrainTriangleMeshingTests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelTerrainTriangleMeshingTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainTriangleMeshingCPU
  COMMAND SparseVoxelTerrainTriangleMeshingTests)
set_tests_properties(
  SparseVoxelTerrainTriangleMeshingCPU
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.phase0;svraster.terrain.mesh;svraster.terrain.benchmark;cpu")
