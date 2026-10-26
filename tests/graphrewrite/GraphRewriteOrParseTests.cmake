cmake_minimum_required(VERSION 3.10)

project (GraphRewriteOrParseTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteOrParseTests ${TEST_DIR}/GraphRewriteOrParseTests.cpp)
target_include_directories(GraphRewriteOrParseTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteOrParseTests PRIVATE Engine)
add_test(NAME GraphRewriteOrParseTests COMMAND GraphRewriteOrParseTests)
