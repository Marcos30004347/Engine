cmake_minimum_required(VERSION 3.10)

project (GraphRewriteCompilerTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteCompilerTests ${TEST_DIR}/GraphRewriteCompilerTests.cpp)
target_include_directories(GraphRewriteCompilerTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteCompilerTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteCompilerTests PRIVATE Engine)
add_test(NAME GraphRewriteCompilerTests COMMAND GraphRewriteCompilerTests)
