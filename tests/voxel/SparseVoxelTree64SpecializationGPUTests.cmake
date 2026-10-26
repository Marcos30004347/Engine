add_executable(
  SparseVoxelTree64SpecializationGPUTests
  ${TEST_DIR}/SparseVoxelTree64SpecializationGPUTests.cpp)
target_link_libraries(SparseVoxelTree64SpecializationGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelTree64SpecializationGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTree64T8SpecializationGPU
  COMMAND SparseVoxelTree64SpecializationGPUTests)
set_tests_properties(
  SparseVoxelTree64T8SpecializationGPU
  PROPERTIES LABELS "svraster.tree64.t8.gpu;gpu")
