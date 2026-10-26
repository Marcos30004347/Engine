cmake_minimum_required(VERSION 3.10)

project (IntegrationGalleryTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(IntegrationGalleryTests ${TEST_DIR}/IntegrationGalleryTests.cpp)
target_link_libraries(IntegrationGalleryTests PRIVATE Engine)
add_test(NAME IntegrationGalleryTests COMMAND IntegrationGalleryTests ${CMAKE_BINARY_DIR}/tests/integration-gallery.json)
set_tests_properties(IntegrationGalleryTests PROPERTIES TIMEOUT 300 LABELS "world-gen;gallery;integration")
