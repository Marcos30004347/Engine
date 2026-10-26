cmake_minimum_required(VERSION 3.10)

project (ProgrammeDwellingTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ProgrammeDwellingTests ${TEST_DIR}/ProgrammeDwellingTests.cpp)
target_link_libraries(ProgrammeDwellingTests PRIVATE Engine)
add_test(NAME ProgrammeDwellingTests COMMAND ProgrammeDwellingTests)
set_tests_properties(ProgrammeDwellingTests PROPERTIES LABELS "world-gen;dwellings;programme")
