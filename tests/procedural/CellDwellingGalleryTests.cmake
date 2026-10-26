cmake_minimum_required(VERSION 3.10)

project (CellDwellingGalleryTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(CellDwellingGalleryTests ${TEST_DIR}/CellDwellingGalleryTests.cpp)
target_link_libraries(CellDwellingGalleryTests PRIVATE Engine)
add_test(NAME CellDwellingGalleryTests COMMAND CellDwellingGalleryTests ${CMAKE_BINARY_DIR}/tests/cell-dwelling-gallery.json)
set_tests_properties(CellDwellingGalleryTests PROPERTIES LABELS "world-gen;gallery;dwellings;cells")
