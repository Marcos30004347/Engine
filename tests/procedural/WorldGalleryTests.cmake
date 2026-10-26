cmake_minimum_required(VERSION 3.10)

project (WorldGalleryTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(WorldGalleryTests ${TEST_DIR}/WorldGalleryTests.cpp)
target_link_libraries(WorldGalleryTests PRIVATE Engine)
add_test(NAME WorldGalleryTests COMMAND WorldGalleryTests ${CMAKE_BINARY_DIR}/tests/world-gallery.json)
set_tests_properties(WorldGalleryTests PROPERTIES TIMEOUT 300 LABELS "world-gen;gallery")
