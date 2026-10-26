cmake_minimum_required(VERSION 3.10)

project (ArchitecturalProgramTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ArchitecturalProgramTests ${TEST_DIR}/ArchitecturalProgramTests.cpp)
target_link_libraries(ArchitecturalProgramTests PRIVATE Engine)
add_test(NAME ArchitecturalProgramTests COMMAND ArchitecturalProgramTests)
set_tests_properties(ArchitecturalProgramTests PROPERTIES LABELS "world-gen;dwellings;programme")
