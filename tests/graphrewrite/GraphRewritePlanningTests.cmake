cmake_minimum_required(VERSION 3.10)

project (GraphRewritePlanningTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewritePlanningTests ${TEST_DIR}/GraphRewritePlanningTests.cpp)
target_include_directories(GraphRewritePlanningTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewritePlanningTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewritePlanningTests PRIVATE Engine)
add_test(NAME GraphRewritePlanningTests COMMAND GraphRewritePlanningTests)
