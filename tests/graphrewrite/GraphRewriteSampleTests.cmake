cmake_minimum_required(VERSION 3.10)

project (GraphRewriteSampleTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteSampleTests ${TEST_DIR}/GraphRewriteSampleTests.cpp)
target_include_directories(GraphRewriteSampleTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteSampleTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteSampleTests PRIVATE Engine)
add_dependencies(GraphRewriteSampleTests graphrewrite_publish_cs)
add_test(NAME GraphRewriteSampleTests COMMAND GraphRewriteSampleTests)
