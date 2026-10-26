add_executable(
  SparseVoxelTree64BuildGPUTests
  ${TEST_DIR}/SparseVoxelTree64BuildGPUTests.cpp)
target_link_libraries(SparseVoxelTree64BuildGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTree64BuildGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTree64T3SingleChunkGPU
  COMMAND SparseVoxelTree64BuildGPUTests)
set_tests_properties(
  SparseVoxelTree64T3SingleChunkGPU
  PROPERTIES LABELS "svraster.tree64.t3.gpu;gpu")
