cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  VirtualGeometryRuntimeObjectLifecycleTests
  ${TEST_DIR}/VirtualGeometryRuntimeObjectLifecycleTests.cpp)
target_link_libraries(VirtualGeometryRuntimeObjectLifecycleTests PRIVATE Engine)

add_test(
  NAME VirtualGeometryRuntimeObjectLifecycle
  COMMAND VirtualGeometryRuntimeObjectLifecycleTests)
set_tests_properties(
  VirtualGeometryRuntimeObjectLifecycle
  PROPERTIES
    TIMEOUT 300
    LABELS "virtualgeometry;svraster.streaming;gpu")
