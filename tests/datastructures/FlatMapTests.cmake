cmake_minimum_required(VERSION 3.10)

project (FlatMapTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(FlatMapTests ${TEST_DIR}/FlatMapTests.cpp)
target_link_libraries(FlatMapTests PRIVATE Engine)
add_test(NAME FlatMapTests COMMAND FlatMapTests)