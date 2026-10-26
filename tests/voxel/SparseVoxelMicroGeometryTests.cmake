add_executable(
  SparseVoxelMicroGeometryTests
  ${TEST_DIR}/SparseVoxelMicroGeometryTests.cpp)
target_link_libraries(SparseVoxelMicroGeometryTests PRIVATE Engine)

add_test(
  NAME SparseVoxelMicroGeometryCPUReferenceTests
  COMMAND SparseVoxelMicroGeometryTests)
set_tests_properties(
  SparseVoxelMicroGeometryCPUReferenceTests
  PROPERTIES LABELS "svraster.microgeometry.cpu;svraster.phase0")
