add_executable(
  SparseVoxelTree64Tests
  ${TEST_DIR}/SparseVoxelTree64Tests.cpp)
target_link_libraries(SparseVoxelTree64Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTree64T1CPU
  COMMAND SparseVoxelTree64Tests)
set_tests_properties(
  SparseVoxelTree64T1CPU
  PROPERTIES LABELS "svraster.tree64.t1;cpu")
