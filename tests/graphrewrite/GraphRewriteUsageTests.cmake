cmake_minimum_required(VERSION 3.10)

project (GraphRewriteUsageTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteUsageTests ${TEST_DIR}/GraphRewriteUsageTests.cpp)
target_include_directories(GraphRewriteUsageTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteUsageTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteUsageTests PRIVATE Engine)
add_test(NAME GraphRewriteUsageTests COMMAND GraphRewriteUsageTests)
