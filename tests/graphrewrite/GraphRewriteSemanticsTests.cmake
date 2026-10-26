cmake_minimum_required(VERSION 3.10)

project (GraphRewriteSemanticsTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteSemanticsTests ${TEST_DIR}/GraphRewriteSemanticsTests.cpp)
target_include_directories(GraphRewriteSemanticsTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteSemanticsTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteSemanticsTests PRIVATE Engine)
add_test(NAME GraphRewriteSemanticsTests COMMAND GraphRewriteSemanticsTests)
