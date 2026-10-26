cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase6Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase6Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase6Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase6RasterHierarchy
  COMMAND SparseVoxelTerrainV2Phase6Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase6RasterHierarchy
  PROPERTIES
    TIMEOUT 60
    LABELS "svraster.terrain.v2.phase6;svraster.terrain.cpu")

add_executable(
  SparseVoxelTerrainV2Phase6GPUTests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase6GPUTests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase6GPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTerrainV2Phase6GPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTerrainV2Phase6GPURasterHierarchy
  COMMAND SparseVoxelTerrainV2Phase6GPUTests)
set_tests_properties(
  SparseVoxelTerrainV2Phase6GPURasterHierarchy
  PROPERTIES
    TIMEOUT 180
    LABELS "svraster.terrain.v2.phase6;svraster.terrain.gpu")

file(READ
  "${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_raster_hierarchy.wgsl"
  TERRAIN_V2_RASTER_HIERARCHY_SHADER)
if(TERRAIN_V2_RASTER_HIERARCHY_SHADER MATCHES "cell < 512" OR
   TERRAIN_V2_RASTER_HIERARCHY_SHADER MATCHES "cell <= 511" OR
   TERRAIN_V2_RASTER_HIERARCHY_SHADER MATCHES "microSurface" OR
   TERRAIN_V2_RASTER_HIERARCHY_SHADER MATCHES "microGeometry")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 6 reintroduced single-thread 512-cell emission or microgeometry")
endif()
