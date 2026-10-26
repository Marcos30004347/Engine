add_executable(
  SparseVoxelOccupancyVolumeTests
  ${TEST_DIR}/SparseVoxelOccupancyVolumeTests.cpp)
target_link_libraries(SparseVoxelOccupancyVolumeTests PRIVATE Engine)
add_dependencies(SparseVoxelOccupancyVolumeTests SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelOccupancyLayoutG23Tests
  COMMAND SparseVoxelOccupancyVolumeTests)
set_tests_properties(
  SparseVoxelOccupancyLayoutG23Tests
  PROPERTIES LABELS "svraster.g23;svraster.m4")

add_test(
  NAME SparseVoxelOccupancyPyramidCPUReferenceTests
  COMMAND SparseVoxelOccupancyVolumeTests)
set_tests_properties(
  SparseVoxelOccupancyPyramidCPUReferenceTests
  PROPERTIES LABELS "svraster.g27.cpu;svraster.m5")
