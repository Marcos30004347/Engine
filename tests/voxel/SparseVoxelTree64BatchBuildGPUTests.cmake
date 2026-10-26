add_executable(
  SparseVoxelTree64BatchBuildGPUTests
  ${TEST_DIR}/SparseVoxelTree64BatchBuildGPUTests.cpp)
target_link_libraries(SparseVoxelTree64BatchBuildGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTree64BatchBuildGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTree64T3BatchedGPU
  COMMAND SparseVoxelTree64BatchBuildGPUTests)
set_tests_properties(
  SparseVoxelTree64T3BatchedGPU
  PROPERTIES LABELS "svraster.tree64.t3.gpu;gpu")
