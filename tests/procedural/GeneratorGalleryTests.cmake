cmake_minimum_required(VERSION 3.10)

project (GeneratorGalleryTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GeneratorGalleryTests ${TEST_DIR}/GeneratorGalleryTests.cpp)
target_link_libraries(GeneratorGalleryTests PRIVATE Engine)
add_test(NAME GeneratorGalleryTests COMMAND GeneratorGalleryTests ${CMAKE_BINARY_DIR}/tests/generator-gallery.json)
set_tests_properties(GeneratorGalleryTests PROPERTIES LABELS "world-gen;gallery")
