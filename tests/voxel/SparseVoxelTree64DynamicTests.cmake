add_executable(
  SparseVoxelTree64DynamicTests
  ${TEST_DIR}/SparseVoxelTree64DynamicTests.cpp)
target_link_libraries(SparseVoxelTree64DynamicTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTree64T7DynamicCPU
  COMMAND SparseVoxelTree64DynamicTests)
set_tests_properties(
  SparseVoxelTree64T7DynamicCPU
  PROPERTIES
    TIMEOUT 20
    LABELS "svraster.tree64.t7.cpu;svraster.tree64.streaming.phase9;cpu")
