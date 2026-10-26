cmake_minimum_required(VERSION 3.10)

project (GraphRewriteRuleGraphTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteRuleGraphTests ${TEST_DIR}/GraphRewriteRuleGraphTests.cpp)
target_include_directories(GraphRewriteRuleGraphTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteRuleGraphTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteRuleGraphTests PRIVATE Engine)
add_test(NAME GraphRewriteRuleGraphTests COMMAND GraphRewriteRuleGraphTests)
