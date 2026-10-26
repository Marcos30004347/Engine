cmake_minimum_required(VERSION 3.10)

project (GraphRewritePagePoolTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewritePagePoolTests ${TEST_DIR}/GraphRewritePagePoolTests.cpp)
target_include_directories(GraphRewritePagePoolTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewritePagePoolTests PRIVATE Engine)
add_test(NAME GraphRewritePagePoolTests COMMAND GraphRewritePagePoolTests)
