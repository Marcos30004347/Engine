cmake_minimum_required(VERSION 3.10)

project (ProgrammeFitTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ProgrammeFitTests ${TEST_DIR}/ProgrammeFitTests.cpp)
target_link_libraries(ProgrammeFitTests PRIVATE Engine)
target_compile_definitions(ProgrammeFitTests PRIVATE
    ARCHITECTURAL_PROGRAMME_CORPUS_DIR="${CMAKE_SOURCE_DIR}/assets/procedural/programmes")
add_test(NAME ProgrammeFitTests COMMAND ProgrammeFitTests)
set_tests_properties(ProgrammeFitTests PROPERTIES LABELS "world-gen;dwellings;programme")
