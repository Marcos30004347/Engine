add_executable(
  SparseVoxelTree64SceneGPUTests
  ${TEST_DIR}/SparseVoxelTree64SceneGPUTests.cpp)
target_link_libraries(SparseVoxelTree64SceneGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTree64SceneGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTree64T5SceneGPU
  COMMAND SparseVoxelTree64SceneGPUTests)
set_tests_properties(
  SparseVoxelTree64T5SceneGPU
  PROPERTIES LABELS "svraster.tree64.t5.gpu;gpu")
