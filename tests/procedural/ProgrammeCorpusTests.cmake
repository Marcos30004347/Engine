cmake_minimum_required(VERSION 3.10)

project (ProgrammeCorpusTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ProgrammeCorpusTests ${TEST_DIR}/ProgrammeCorpusTests.cpp)
target_link_libraries(ProgrammeCorpusTests PRIVATE Engine)
target_compile_definitions(ProgrammeCorpusTests PRIVATE
    ARCHITECTURAL_PROGRAMME_CORPUS_DIR="${CMAKE_SOURCE_DIR}/assets/procedural/programmes")
add_test(NAME ProgrammeCorpusTests COMMAND ProgrammeCorpusTests)
set_tests_properties(ProgrammeCorpusTests PROPERTIES LABELS "world-gen;dwellings;programme")
