cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  VirtualGeometryGeneratedPayloadManagerTests
  ${TEST_DIR}/VirtualGeometryGeneratedPayloadManagerTests.cpp)
target_link_libraries(VirtualGeometryGeneratedPayloadManagerTests PRIVATE Engine)

add_test(
  NAME VirtualGeometryGeneratedPayloadManagerTests
  COMMAND VirtualGeometryGeneratedPayloadManagerTests)
set_tests_properties(
  VirtualGeometryGeneratedPayloadManagerTests
  PROPERTIES
    TIMEOUT 120
    LABELS "virtualgeometry;streaming;gpu-generated-payloads")
