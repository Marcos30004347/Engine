cmake_minimum_required(VERSION 3.10)

project (GraphRewriteParserCoreTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteParserCoreTests ${TEST_DIR}/GraphRewriteParserCoreTests.cpp)
target_include_directories(GraphRewriteParserCoreTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteParserCoreTests PRIVATE Engine)
add_test(NAME GraphRewriteParserCoreTests COMMAND GraphRewriteParserCoreTests)
