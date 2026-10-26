cmake_minimum_required(VERSION 3.10)

project (GraphRewriteClauseTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteClauseTests ${TEST_DIR}/GraphRewriteClauseTests.cpp)
target_include_directories(GraphRewriteClauseTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteClauseTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteClauseTests PRIVATE Engine)
add_test(NAME GraphRewriteClauseTests COMMAND GraphRewriteClauseTests)
