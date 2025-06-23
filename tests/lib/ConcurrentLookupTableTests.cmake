cmake_minimum_required(VERSION 3.10)

project (ConcurrentLookupTableTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentLookupTableTests ${TEST_DIR}/ConcurrentLookupTableTests.cpp)
target_link_libraries(ConcurrentLookupTableTests PRIVATE Engine)
add_test(NAME ConcurrentLookupTableTests COMMAND ConcurrentLookupTableTests)