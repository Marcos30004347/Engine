cmake_minimum_required(VERSION 3.10)

project (GraphRewritePageStoreTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewritePageStoreTests ${TEST_DIR}/GraphRewritePageStoreTests.cpp)
target_include_directories(GraphRewritePageStoreTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewritePageStoreTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewritePageStoreTests PRIVATE Engine)
add_test(NAME GraphRewritePageStoreTests COMMAND GraphRewritePageStoreTests)
