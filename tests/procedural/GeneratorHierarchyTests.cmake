cmake_minimum_required(VERSION 3.10)

project (GeneratorHierarchyTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GeneratorHierarchyTests ${TEST_DIR}/GeneratorHierarchyTests.cpp)
target_link_libraries(GeneratorHierarchyTests PRIVATE Engine)
add_test(NAME GeneratorHierarchyTests COMMAND GeneratorHierarchyTests)
