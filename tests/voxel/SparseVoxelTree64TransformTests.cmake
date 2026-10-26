add_executable(
  SparseVoxelTree64TransformTests
  ${TEST_DIR}/SparseVoxelTree64TransformTests.cpp)
target_link_libraries(SparseVoxelTree64TransformTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTree64T6TransformCPU
  COMMAND SparseVoxelTree64TransformTests)
set_tests_properties(
  SparseVoxelTree64T6TransformCPU
  PROPERTIES
    TIMEOUT 30
    LABELS "svraster.tree64.t6.cpu;cpu")
