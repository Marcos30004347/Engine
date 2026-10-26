cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase7Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase7Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase7Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase7PublicationResidency
  COMMAND SparseVoxelTerrainV2Phase7Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase7PublicationResidency
  PROPERTIES
    TIMEOUT 90
    LABELS "svraster.terrain.v2.phase7;svraster.terrain.cpu")

add_executable(
  SparseVoxelTerrainV2Phase7GPUTests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase7GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase7GPUTests PRIVATE Engine)
add_dependencies(SparseVoxelTerrainV2Phase7GPUTests SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainV2Phase7GPUAtomicPublication
  COMMAND SparseVoxelTerrainV2Phase7GPUTests)
set_tests_properties(
  SparseVoxelTerrainV2Phase7GPUAtomicPublication
  PROPERTIES
    TIMEOUT 90
    LABELS "svraster.terrain.v2.phase7;svraster.terrain.gpu")

file(READ
  "${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_publication.wgsl"
  TERRAIN_V2_PUBLICATION_SHADER)
if(NOT TERRAIN_V2_PUBLICATION_SHADER MATCHES "validateTerrainGPUV2Publication" OR
   NOT TERRAIN_V2_PUBLICATION_SHADER MATCHES "commitTerrainGPUV2Publication" OR
   TERRAIN_V2_PUBLICATION_SHADER MATCHES "microGeometry" OR
   TERRAIN_V2_PUBLICATION_SHADER MATCHES "surfacePatch" OR
   TERRAIN_V2_PUBLICATION_SHADER MATCHES "vertexBuffer")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 7 publication lost its two-stage occupancy-only contract")
endif()
