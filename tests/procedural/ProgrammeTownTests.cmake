cmake_minimum_required(VERSION 3.10)

project (ProgrammeTownTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ProgrammeTownTests ${TEST_DIR}/ProgrammeTownTests.cpp)
target_link_libraries(ProgrammeTownTests PRIVATE Engine)
add_test(NAME ProgrammeTownTests COMMAND ProgrammeTownTests)
set_tests_properties(ProgrammeTownTests PROPERTIES LABELS "world-gen;dwellings;programme")
