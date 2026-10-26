cmake_minimum_required(VERSION 3.10)

project (GraphRewriteIrTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteIrTests ${TEST_DIR}/GraphRewriteIrTests.cpp)
target_include_directories(GraphRewriteIrTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteIrTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteIrTests PRIVATE Engine)
add_test(NAME GraphRewriteIrTests COMMAND GraphRewriteIrTests)
