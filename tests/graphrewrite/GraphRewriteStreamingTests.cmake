cmake_minimum_required(VERSION 3.10)

project (GraphRewriteStreamingTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteStreamingTests ${TEST_DIR}/GraphRewriteStreamingTests.cpp)
target_include_directories(GraphRewriteStreamingTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteStreamingTests PRIVATE Engine)
add_test(NAME GraphRewriteStreamingTests COMMAND GraphRewriteStreamingTests)
