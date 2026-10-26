cmake_minimum_required(VERSION 3.10)

project (GraphRewriteStoryTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteStoryTests ${TEST_DIR}/GraphRewriteStoryTests.cpp)
target_include_directories(GraphRewriteStoryTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteStoryTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteStoryTests PRIVATE Engine)
add_test(NAME GraphRewriteStoryTests COMMAND GraphRewriteStoryTests)
