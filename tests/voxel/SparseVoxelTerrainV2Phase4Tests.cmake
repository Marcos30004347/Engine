cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase4Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase4Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase4Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase4FineRaster
  COMMAND SparseVoxelTerrainV2Phase4Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase4FineRaster
  PROPERTIES
    TIMEOUT 60
    LABELS "svraster.terrain.v2.phase4;svraster.terrain.cpu")

add_executable(
  SparseVoxelTerrainV2Phase4GPUTests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase4GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase4GPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTerrainV2Phase4GPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainV2Phase4GPUFineRaster
  COMMAND SparseVoxelTerrainV2Phase4GPUTests ${CMAKE_SOURCE_DIR}/world-data)
set_tests_properties(
  SparseVoxelTerrainV2Phase4GPUFineRaster
  PROPERTIES
    TIMEOUT 240
    LABELS "svraster.terrain.v2.phase4;svraster.terrain.gpu;svraster.terrain.world-map")

file(READ
  "${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_fine_raster.wgsl"
  TERRAIN_V2_FINE_RASTER_SHADER)
if(TERRAIN_V2_FINE_RASTER_SHADER MATCHES "nodeIndex \\* 512" OR
   TERRAIN_V2_FINE_RASTER_SHADER MATCHES "cell < 512")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 4 reintroduced a serial 512-cell node path")
endif()
