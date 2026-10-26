cmake_minimum_required(VERSION 3.10)

project (GraphRewriteDiagnosticsTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteDiagnosticsTests ${TEST_DIR}/GraphRewriteDiagnosticsTests.cpp)
target_include_directories(GraphRewriteDiagnosticsTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteDiagnosticsTests PRIVATE Engine)
add_test(NAME GraphRewriteDiagnosticsTests COMMAND GraphRewriteDiagnosticsTests)
