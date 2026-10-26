add_executable(
  SparseVoxelTree64SceneTests
  ${TEST_DIR}/SparseVoxelTree64SceneTests.cpp)
target_link_libraries(SparseVoxelTree64SceneTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTree64T5SceneCPU
  COMMAND SparseVoxelTree64SceneTests)
set_tests_properties(
  SparseVoxelTree64T5SceneCPU
  PROPERTIES
    TIMEOUT 30
    LABELS "svraster.tree64.t5.cpu;cpu")
