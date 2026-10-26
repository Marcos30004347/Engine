cmake_minimum_required(VERSION 3.10)

project (ProgrammeClassesTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ProgrammeClassesTests ${TEST_DIR}/ProgrammeClassesTests.cpp)
target_link_libraries(ProgrammeClassesTests PRIVATE Engine)
add_test(NAME ProgrammeClassesTests COMMAND ProgrammeClassesTests)
set_tests_properties(ProgrammeClassesTests PROPERTIES LABELS "world-gen;dwellings;programme")
