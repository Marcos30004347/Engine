cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
add_executable(
  AtmospherePhase0Tests
  ${TEST_DIR}/AtmospherePhase0Tests.cpp)
target_link_libraries(AtmospherePhase0Tests PRIVATE Engine)
add_test(NAME AtmospherePhase0Tests COMMAND AtmospherePhase0Tests)
set_tests_properties(
  AtmospherePhase0Tests
  PROPERTIES LABELS "atmosphere.cpu;atmosphere.phase0;atmosphere.phase1;atmosphere.phase2;atmosphere.phase3;atmosphere.phase4;atmosphere.phase5;atmosphere.phase6")
