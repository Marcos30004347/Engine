cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelVGPayloadContractTests
  ${TEST_DIR}/SparseVoxelVGPayloadContractTests.cpp
  ${TEST_DIR}/TerrainGenerationPhase0Utils.cpp)
target_link_libraries(SparseVoxelVGPayloadContractTests PRIVATE Engine)

add_test(
  NAME SparseVoxelVGPayloadContract
  COMMAND SparseVoxelVGPayloadContractTests)
set_tests_properties(
  SparseVoxelVGPayloadContract
  PROPERTIES
    TIMEOUT 600
    LABELS "svraster.virtualgeometry;virtualgeometry;virtualgeometry.lod;cpu")
