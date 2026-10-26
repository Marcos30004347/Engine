cmake_minimum_required(VERSION 3.10)

project (GraphRewriteDurabilityTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteDurabilityTests ${TEST_DIR}/GraphRewriteDurabilityTests.cpp)
target_include_directories(GraphRewriteDurabilityTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteDurabilityTests PRIVATE Engine)
add_test(NAME GraphRewriteDurabilityTests COMMAND GraphRewriteDurabilityTests)
