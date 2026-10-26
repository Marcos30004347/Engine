cmake_minimum_required(VERSION 3.14)

add_executable(
  SparseVoxelOccupancyMipGPUTests
  ${TEST_DIR}/SparseVoxelOccupancyMipGPUTests.cpp)
target_link_libraries(SparseVoxelOccupancyMipGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelOccupancyMipGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelOccupancyMipGPUReferenceTests
  COMMAND SparseVoxelOccupancyMipGPUTests)
set_tests_properties(
  SparseVoxelOccupancyMipGPUReferenceTests
  PROPERTIES LABELS "svraster.g24.gpu;svraster.g25.gpu;svraster.g27.gpu;svraster.g28.gpu;svraster.m4;svraster.m5;gpu")
