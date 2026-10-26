cmake_minimum_required(VERSION 3.10)

project (GraphRewriteSentenceTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteSentenceTests ${TEST_DIR}/GraphRewriteSentenceTests.cpp)
target_include_directories(GraphRewriteSentenceTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteSentenceTests PRIVATE Engine)
add_test(NAME GraphRewriteSentenceTests COMMAND GraphRewriteSentenceTests)
