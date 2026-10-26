cmake_minimum_required(VERSION 3.10)

project (GraphRewriteRewriteTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteRewriteTests ${TEST_DIR}/GraphRewriteRewriteTests.cpp)
target_include_directories(GraphRewriteRewriteTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteRewriteTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteRewriteTests PRIVATE Engine)
add_test(NAME GraphRewriteRewriteTests COMMAND GraphRewriteRewriteTests)
