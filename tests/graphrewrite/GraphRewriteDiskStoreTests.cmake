cmake_minimum_required(VERSION 3.10)

project (GraphRewriteDiskStoreTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(GraphRewriteDiskStoreTests ${TEST_DIR}/GraphRewriteDiskStoreTests.cpp)
target_include_directories(GraphRewriteDiskStoreTests PRIVATE ${TEST_DIR})
target_link_libraries(GraphRewriteDiskStoreTests PRIVATE Engine)
add_test(NAME GraphRewriteDiskStoreTests COMMAND GraphRewriteDiskStoreTests)
