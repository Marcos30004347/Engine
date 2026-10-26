add_executable(
  SparseVoxelOccupancyDDATests
  ${TEST_DIR}/SparseVoxelOccupancyDDATests.cpp)
target_link_libraries(SparseVoxelOccupancyDDATests PRIVATE Engine)

add_test(
  NAME SparseVoxelOccupancyDDACPUReferenceTests
  COMMAND SparseVoxelOccupancyDDATests)
set_tests_properties(
  SparseVoxelOccupancyDDACPUReferenceTests
  PROPERTIES LABELS "svraster.g29.cpu;svraster.m5")
