cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase1Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase1Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase1Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase1ChunkContract
  COMMAND SparseVoxelTerrainV2Phase1Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase1ChunkContract
  PROPERTIES
    TIMEOUT 30
    LABELS "svraster.terrain.v2.phase1;svraster.terrain.cpu")

