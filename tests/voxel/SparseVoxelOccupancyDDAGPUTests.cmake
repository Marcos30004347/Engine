add_executable(
  SparseVoxelOccupancyDDAGPUTests
  ${TEST_DIR}/SparseVoxelOccupancyDDAGPUTests.cpp)
target_link_libraries(SparseVoxelOccupancyDDAGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelOccupancyDDAGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelOccupancyDDAGPUReferenceTests
  COMMAND SparseVoxelOccupancyDDAGPUTests)
set_tests_properties(
  SparseVoxelOccupancyDDAGPUReferenceTests
  PROPERTIES LABELS "svraster.g29.gpu;svraster.m5;gpu")
