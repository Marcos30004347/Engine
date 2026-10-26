cmake_minimum_required(VERSION 3.10)

project (DwellingGalleryTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(DwellingGalleryTests ${TEST_DIR}/DwellingGalleryTests.cpp)
target_link_libraries(DwellingGalleryTests PRIVATE Engine)
add_test(NAME DwellingGalleryTests COMMAND DwellingGalleryTests ${CMAKE_BINARY_DIR}/tests/dwelling-gallery.json)
set_tests_properties(DwellingGalleryTests PROPERTIES LABELS "world-gen;gallery;dwellings")
