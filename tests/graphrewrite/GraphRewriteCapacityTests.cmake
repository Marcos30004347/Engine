cmake_minimum_required(VERSION 3.10)

project (GraphRewriteCapacityTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteCapacityTests ${TEST_DIR}/GraphRewriteCapacityTests.cpp)
target_include_directories(GraphRewriteCapacityTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteCapacityTests PRIVATE Engine)
add_dependencies(GraphRewriteCapacityTests graphrewrite_publish_cs)
add_test(NAME GraphRewriteCapacityTests COMMAND GraphRewriteCapacityTests)
