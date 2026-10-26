cmake_minimum_required(VERSION 3.10)

project (ProgrammeMansionTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ProgrammeMansionTests ${TEST_DIR}/ProgrammeMansionTests.cpp)
target_link_libraries(ProgrammeMansionTests PRIVATE Engine)
add_test(NAME ProgrammeMansionTests COMMAND ProgrammeMansionTests)
set_tests_properties(ProgrammeMansionTests PROPERTIES LABELS "world-gen;dwellings;programme")
