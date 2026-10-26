cmake_minimum_required(VERSION 3.10)

project (ProgrammeLotTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ProgrammeLotTests ${TEST_DIR}/ProgrammeLotTests.cpp)
target_link_libraries(ProgrammeLotTests PRIVATE Engine)
add_test(NAME ProgrammeLotTests COMMAND ProgrammeLotTests)
set_tests_properties(ProgrammeLotTests PROPERTIES LABELS "world-gen;dwellings;programme")
