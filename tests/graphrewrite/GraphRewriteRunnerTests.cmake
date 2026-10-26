cmake_minimum_required(VERSION 3.10)

project (GraphRewriteRunnerTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteRunnerTests ${TEST_DIR}/GraphRewriteRunnerTests.cpp)
target_include_directories(GraphRewriteRunnerTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteRunnerTests PRIVATE Engine)
add_dependencies(GraphRewriteRunnerTests graphrewrite_publish_cs)
add_test(NAME GraphRewriteRunnerTests COMMAND GraphRewriteRunnerTests)
