cmake_minimum_required(VERSION 3.10)

project (GraphRewritePublishTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewritePublishTests ${TEST_DIR}/GraphRewritePublishTests.cpp)
target_include_directories(GraphRewritePublishTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewritePublishTests PRIVATE Engine)
add_dependencies(GraphRewritePublishTests graphrewrite_publish_cs)
add_test(NAME GraphRewritePublishTests COMMAND GraphRewritePublishTests)
