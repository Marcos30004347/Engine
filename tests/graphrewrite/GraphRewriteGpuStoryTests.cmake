cmake_minimum_required(VERSION 3.10)

project (GraphRewriteGpuStoryTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteGpuStoryTests ${TEST_DIR}/GraphRewriteGpuStoryTests.cpp)
target_include_directories(GraphRewriteGpuStoryTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteGpuStoryTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteGpuStoryTests PRIVATE Engine)
add_dependencies(GraphRewriteGpuStoryTests graphrewrite_rewrite_cs graphrewrite_match_cs)
add_test(NAME GraphRewriteGpuStoryTests COMMAND GraphRewriteGpuStoryTests)
