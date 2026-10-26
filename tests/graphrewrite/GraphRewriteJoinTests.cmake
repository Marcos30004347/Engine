cmake_minimum_required(VERSION 3.10)

project (GraphRewriteJoinTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteJoinTests ${TEST_DIR}/GraphRewriteJoinTests.cpp)
target_include_directories(GraphRewriteJoinTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteJoinTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteJoinTests PRIVATE Engine)
add_test(NAME GraphRewriteJoinTests COMMAND GraphRewriteJoinTests)
