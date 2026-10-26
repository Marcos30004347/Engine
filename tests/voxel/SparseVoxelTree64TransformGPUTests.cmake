add_executable(
  SparseVoxelTree64TransformGPUTests
  ${TEST_DIR}/SparseVoxelTree64TransformGPUTests.cpp)
target_link_libraries(SparseVoxelTree64TransformGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTree64TransformGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTree64T6TransformGPU
  COMMAND SparseVoxelTree64TransformGPUTests)
set_tests_properties(
  SparseVoxelTree64T6TransformGPU
  PROPERTIES LABELS "svraster.tree64.t6.gpu;gpu")
