add_executable(
  SparseVoxelTree64StreamingTests
  ${TEST_DIR}/SparseVoxelTree64StreamingTests.cpp)
target_link_libraries(SparseVoxelTree64StreamingTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTree64T4StreamingCPU
  COMMAND SparseVoxelTree64StreamingTests)
set_tests_properties(
  SparseVoxelTree64T4StreamingCPU
  PROPERTIES
    TIMEOUT 30
    LABELS "svraster.tree64.t4;svraster.tree64.streaming.phase6;svraster.tree64.streaming.phase8;svraster.tree64.streaming.phase9;svraster.tree64.streaming.stability;cpu")

add_executable(
  SparseVoxelTree64PageUploadGPUTests
  ${TEST_DIR}/SparseVoxelTree64PageUploadGPUTests.cpp)
target_link_libraries(SparseVoxelTree64PageUploadGPUTests PRIVATE Engine)
add_dependencies(SparseVoxelTree64PageUploadGPUTests SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelTree64PageUploadGPU
  COMMAND SparseVoxelTree64PageUploadGPUTests)
set_tests_properties(
  SparseVoxelTree64PageUploadGPU
  PROPERTIES
    TIMEOUT 30
    LABELS "svraster.tree64.t4;svraster.tree64.streaming.phase8;svraster.tree64.streaming.phase9;svraster.tree64.streaming.stability;gpu")

add_executable(
  SparseVoxelTree64SchedulerTests
  ${TEST_DIR}/SparseVoxelTree64SchedulerTests.cpp)
target_link_libraries(SparseVoxelTree64SchedulerTests PRIVATE Engine)

add_test(
  NAME SparseVoxelTree64PageSchedulerCPU
  COMMAND SparseVoxelTree64SchedulerTests)
set_tests_properties(
  SparseVoxelTree64PageSchedulerCPU
  PROPERTIES
    TIMEOUT 20
    LABELS "svraster.tree64.streaming.phase6;svraster.tree64.streaming.phase8;svraster.tree64.streaming.phase9;svraster.tree64.streaming.phase10;cpu")

add_test(
  NAME SparseVoxelTree64PageStreamingCutoverCPU
  COMMAND
    ${Python3_EXECUTABLE}
    ${TEST_DIR}/SparseVoxelTree64PageStreamingContract.py
    ${CMAKE_SOURCE_DIR}/samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp)
set_tests_properties(
  SparseVoxelTree64PageStreamingCutoverCPU
  PROPERTIES
    TIMEOUT 10
    LABELS "svraster.tree64.streaming.phase9;cpu")
