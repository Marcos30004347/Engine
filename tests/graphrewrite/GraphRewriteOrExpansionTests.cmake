cmake_minimum_required(VERSION 3.10)

project (GraphRewriteOrExpansionTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteOrExpansionTests ${TEST_DIR}/GraphRewriteOrExpansionTests.cpp)
target_include_directories(GraphRewriteOrExpansionTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteOrExpansionTests PRIVATE Engine)
add_test(NAME GraphRewriteOrExpansionTests COMMAND GraphRewriteOrExpansionTests)
