add_executable(
  SparseVoxelTree64PublishGPUTests
  ${TEST_DIR}/SparseVoxelTree64PublishGPUTests.cpp)
target_link_libraries(SparseVoxelTree64PublishGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTree64PublishGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTree64T4PublishGPU
  COMMAND SparseVoxelTree64PublishGPUTests)
set_tests_properties(
  SparseVoxelTree64T4PublishGPU
  PROPERTIES
    TIMEOUT 30
    LABELS "svraster.tree64.t4;gpu")
