cmake_minimum_required(VERSION 3.10)

project (GraphRewriteRuleParseTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteRuleParseTests ${TEST_DIR}/GraphRewriteRuleParseTests.cpp)
target_include_directories(GraphRewriteRuleParseTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteRuleParseTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteRuleParseTests PRIVATE Engine)
add_test(NAME GraphRewriteRuleParseTests COMMAND GraphRewriteRuleParseTests)
