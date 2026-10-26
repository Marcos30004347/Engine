add_executable(
  SparseVoxelTree64SpecializationTests
  ${TEST_DIR}/SparseVoxelTree64SpecializationTests.cpp)
target_link_libraries(SparseVoxelTree64SpecializationTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTree64T8SpecializationCPU
  COMMAND SparseVoxelTree64SpecializationTests)
set_tests_properties(
  SparseVoxelTree64T8SpecializationCPU
  PROPERTIES LABELS "svraster.tree64.t8.cpu;cpu")
