cmake_minimum_required(VERSION 3.10)

project (GraphRewriteCpuCacheTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteCpuCacheTests ${TEST_DIR}/GraphRewriteCpuCacheTests.cpp)
target_include_directories(GraphRewriteCpuCacheTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteCpuCacheTests PRIVATE Engine)
add_test(NAME GraphRewriteCpuCacheTests COMMAND GraphRewriteCpuCacheTests)
