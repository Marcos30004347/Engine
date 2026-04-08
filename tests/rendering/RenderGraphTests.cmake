cmake_minimum_required(VERSION 3.10)

project (RenderGraphTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(RenderGraphTests ${TEST_DIR}/RenderGraphTests.cpp)
target_link_libraries(RenderGraphTests PRIVATE Engine)
add_test(NAME RenderGraphTests COMMAND RenderGraphTests)