cmake_minimum_required(VERSION 3.10)

project (CellDwellingTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(CellDwellingTests ${TEST_DIR}/CellDwellingTests.cpp)
target_link_libraries(CellDwellingTests PRIVATE Engine)
add_test(NAME CellDwellingTests COMMAND CellDwellingTests)
set_tests_properties(CellDwellingTests PROPERTIES LABELS "world-gen;dwellings;cells")
