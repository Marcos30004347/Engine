cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  VisibilityPayloadTests
  ${TEST_DIR}/VisibilityPayloadTests.cpp)
target_link_libraries(VisibilityPayloadTests PRIVATE Engine)

add_test(NAME VisibilityPayload COMMAND VisibilityPayloadTests)
set_tests_properties(
  VisibilityPayload
  PROPERTIES
    TIMEOUT 60
    LABELS "virtualgeometry;cpu")
