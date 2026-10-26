cmake_minimum_required(VERSION 3.10)

project (GraphRewritePolicyTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewritePolicyTests ${TEST_DIR}/GraphRewritePolicyTests.cpp)
target_include_directories(GraphRewritePolicyTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewritePolicyTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewritePolicyTests PRIVATE Engine)
add_test(NAME GraphRewritePolicyTests COMMAND GraphRewritePolicyTests)
