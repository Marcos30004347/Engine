add_executable(
  SparseVoxelTerrainTree64Tests
  ${TEST_DIR}/SparseVoxelTerrainTree64Tests.cpp)
target_link_libraries(SparseVoxelTerrainTree64Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainTree64PageCPU
  COMMAND SparseVoxelTerrainTree64Tests)
set_tests_properties(
  SparseVoxelTerrainTree64PageCPU
  PROPERTIES
    TIMEOUT 30
    LABELS "svraster.tree64.terrain;svraster.tree64.streaming.phase8;svraster.tree64.streaming.phase9;svraster.tree64.streaming.stability;svraster.tree64.raster-parity;cpu")
