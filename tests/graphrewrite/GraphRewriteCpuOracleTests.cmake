cmake_minimum_required(VERSION 3.10)

project (GraphRewriteCpuOracleTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteCpuOracleTests ${TEST_DIR}/GraphRewriteCpuOracleTests.cpp)
target_include_directories(GraphRewriteCpuOracleTests PRIVATE ${TEST_DIR})
target_compile_definitions(GraphRewriteCpuOracleTests PRIVATE
    GRAPHREWRITE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/graphrewrite")
target_link_libraries(GraphRewriteCpuOracleTests PRIVATE Engine)
add_test(NAME GraphRewriteCpuOracleTests COMMAND GraphRewriteCpuOracleTests)
