set(TEST_DIR ${CMAKE_CURRENT_LIST_DIR})

add_executable(
  SparseVoxelTerrainVirtualGeometryPhase17Tests
  ${TEST_DIR}/SparseVoxelTerrainVirtualGeometryPhase17Tests.cpp)
target_link_libraries(SparseVoxelTerrainVirtualGeometryPhase17Tests PRIVATE Engine)
add_dependencies(
  SparseVoxelTerrainVirtualGeometryPhase17Tests
  terrain_gpu_v2_generated_vg_emit_128)
target_compile_definitions(
  SparseVoxelTerrainVirtualGeometryPhase17Tests
  PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_test(
  NAME SparseVoxelTerrainVirtualGeometryPhase17ComputeRasterContract
  COMMAND SparseVoxelTerrainVirtualGeometryPhase17Tests)

set_tests_properties(
  SparseVoxelTerrainVirtualGeometryPhase17ComputeRasterContract
  PROPERTIES LABELS "voxel;terrain;virtual-geometry;phase17;compute-raster")
