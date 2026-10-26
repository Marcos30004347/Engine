cmake_minimum_required(VERSION 3.10)

project (GraphRewriteGpuRewriteTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteGpuRewriteTests ${TEST_DIR}/GraphRewriteGpuRewriteTests.cpp)
target_include_directories(GraphRewriteGpuRewriteTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteGpuRewriteTests PRIVATE Engine)
add_dependencies(GraphRewriteGpuRewriteTests graphrewrite_publish_cs)
add_test(NAME GraphRewriteGpuRewriteTests COMMAND GraphRewriteGpuRewriteTests)
