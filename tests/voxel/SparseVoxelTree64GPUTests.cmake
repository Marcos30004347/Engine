add_executable(
  SparseVoxelTree64GPUTests
  ${TEST_DIR}/SparseVoxelTree64GPUTests.cpp)
target_link_libraries(SparseVoxelTree64GPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTree64GPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTree64GPUReferenceTests
  COMMAND SparseVoxelTree64GPUTests)
set_tests_properties(
  SparseVoxelTree64GPUReferenceTests
  PROPERTIES LABELS "svraster.tree64.t2.gpu;gpu")
