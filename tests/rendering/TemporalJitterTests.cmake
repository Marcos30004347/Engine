cmake_minimum_required(VERSION 3.14)

project (TemporalJitterTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(TemporalJitterTests ${TEST_DIR}/TemporalJitterTests.cpp)
target_link_libraries(TemporalJitterTests PRIVATE Engine)

add_test(NAME TemporalJitterSpanTests COMMAND TemporalJitterTests --span)
set_tests_properties(TemporalJitterSpanTests PROPERTIES LABELS "temporal.t0;upscaling")

add_test(NAME TemporalJitterSequenceTests COMMAND TemporalJitterTests --sequence)
set_tests_properties(TemporalJitterSequenceTests PROPERTIES LABELS "temporal.t0;upscaling")

add_test(NAME TemporalJitterPhaseCountTests COMMAND TemporalJitterTests --phases)
set_tests_properties(TemporalJitterPhaseCountTests PROPERTIES LABELS "temporal.t0;upscaling")
