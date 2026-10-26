cmake_minimum_required(VERSION 3.10)

project (GraphRewriteColumnsTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteColumnsTests ${TEST_DIR}/GraphRewriteColumnsTests.cpp)
target_include_directories(GraphRewriteColumnsTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteColumnsTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteColumnsTests PRIVATE Engine)
add_test(NAME GraphRewriteColumnsTests COMMAND GraphRewriteColumnsTests)
