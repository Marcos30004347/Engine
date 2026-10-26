cmake_minimum_required(VERSION 3.10)

project (GraphRewriteBenchmarkTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteBenchmarkTests ${TEST_DIR}/GraphRewriteBenchmarkTests.cpp)
target_include_directories(GraphRewriteBenchmarkTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteBenchmarkTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteBenchmarkTests PRIVATE Engine)
add_dependencies(GraphRewriteBenchmarkTests graphrewrite_publish_cs)
add_dependencies(GraphRewriteBenchmarkTests graphrewrite_match_cs graphrewrite_rewrite_cs)
add_test(NAME GraphRewriteBenchmarkTests COMMAND GraphRewriteBenchmarkTests 200000)
