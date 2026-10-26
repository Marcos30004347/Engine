cmake_minimum_required(VERSION 3.10)

project (GraphRewriteVerifierTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteVerifierTests ${TEST_DIR}/GraphRewriteVerifierTests.cpp)
target_include_directories(GraphRewriteVerifierTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteVerifierTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteVerifierTests PRIVATE Engine)
add_test(NAME GraphRewriteVerifierTests COMMAND GraphRewriteVerifierTests)
