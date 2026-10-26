cmake_minimum_required(VERSION 3.10)

project (GraphRewriteEndToEndTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteEndToEndTests ${TEST_DIR}/GraphRewriteEndToEndTests.cpp)
target_include_directories(GraphRewriteEndToEndTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteEndToEndTests PRIVATE Engine)
add_dependencies(GraphRewriteEndToEndTests graphrewrite_publish_cs)
add_test(NAME GraphRewriteEndToEndTests COMMAND GraphRewriteEndToEndTests)
