cmake_minimum_required(VERSION 3.10)

project (DwellingMeshTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(DwellingMeshTests ${TEST_DIR}/DwellingMeshTests.cpp)
target_link_libraries(DwellingMeshTests PRIVATE Engine)
add_test(NAME DwellingMeshTests COMMAND DwellingMeshTests)
set_tests_properties(DwellingMeshTests PROPERTIES LABELS "world-gen;dwellings;programme")
