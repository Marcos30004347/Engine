add_executable(
  SparseVoxelTree64DynamicGPUTests
  ${TEST_DIR}/SparseVoxelTree64DynamicGPUTests.cpp)
target_link_libraries(
  SparseVoxelTree64DynamicGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTree64DynamicGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTree64T7DynamicGPU
  COMMAND SparseVoxelTree64DynamicGPUTests)
set_tests_properties(
  SparseVoxelTree64T7DynamicGPU
  PROPERTIES
    TIMEOUT 30
    LABELS "svraster.tree64.t7.gpu;svraster.tree64.streaming.phase9;svraster.tree64.streaming.stability;svraster.tree64.raster-parity;gpu")
