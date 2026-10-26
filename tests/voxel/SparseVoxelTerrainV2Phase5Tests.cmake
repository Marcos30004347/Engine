cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase5Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase5Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase5Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase5Tree64
  COMMAND SparseVoxelTerrainV2Phase5Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase5Tree64
  PROPERTIES
    TIMEOUT 60
    LABELS "svraster.terrain.v2.phase5;svraster.terrain.cpu")

add_executable(
  SparseVoxelTerrainV2Phase5GPUTests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase5GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase5GPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTerrainV2Phase5GPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainV2Phase5GPUTree64
  COMMAND SparseVoxelTerrainV2Phase5GPUTests)
set_tests_properties(
  SparseVoxelTerrainV2Phase5GPUTree64
  PROPERTIES
    TIMEOUT 120
    LABELS "svraster.terrain.v2.phase5;svraster.terrain.gpu")

file(READ
  "${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_tree64.wgsl"
  TERRAIN_V2_TREE64_SHADER)
if(TERRAIN_V2_TREE64_SHADER MATCHES "nodeIndex \\* 512" OR
   TERRAIN_V2_TREE64_SHADER MATCHES "cell < 512" OR
   TERRAIN_V2_TREE64_SHADER MATCHES "semanticHash" OR
   TERRAIN_V2_TREE64_SHADER MATCHES "payloadHash")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 5 reintroduced serial page work or payload hashing")
endif()
