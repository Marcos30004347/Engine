cmake_minimum_required(VERSION 3.10)

project (GraphRewriteEmitTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteEmitTests ${TEST_DIR}/GraphRewriteEmitTests.cpp)
target_include_directories(GraphRewriteEmitTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteEmitTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteEmitTests PRIVATE Engine)
add_test(NAME GraphRewriteEmitTests COMMAND GraphRewriteEmitTests)
