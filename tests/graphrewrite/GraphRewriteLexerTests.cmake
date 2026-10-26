cmake_minimum_required(VERSION 3.10)

project (GraphRewriteLexerTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteLexerTests ${TEST_DIR}/GraphRewriteLexerTests.cpp)
target_include_directories(GraphRewriteLexerTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteLexerTests PRIVATE Engine)
add_test(NAME GraphRewriteLexerTests COMMAND GraphRewriteLexerTests)
