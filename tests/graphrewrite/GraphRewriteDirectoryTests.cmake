cmake_minimum_required(VERSION 3.10)

project (GraphRewriteDirectoryTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteDirectoryTests ${TEST_DIR}/GraphRewriteDirectoryTests.cpp)
target_include_directories(GraphRewriteDirectoryTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteDirectoryTests PRIVATE Engine)
add_test(NAME GraphRewriteDirectoryTests COMMAND GraphRewriteDirectoryTests)
