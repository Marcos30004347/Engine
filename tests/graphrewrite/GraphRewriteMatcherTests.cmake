cmake_minimum_required(VERSION 3.10)

project (GraphRewriteMatcherTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteMatcherTests ${TEST_DIR}/GraphRewriteMatcherTests.cpp)
target_include_directories(GraphRewriteMatcherTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteMatcherTests PRIVATE Engine)
add_test(NAME GraphRewriteMatcherTests COMMAND GraphRewriteMatcherTests)
