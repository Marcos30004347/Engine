cmake_minimum_required(VERSION 3.10)

project (CPQ)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(CPQ ${TEST_DIR}/CPQ.cpp)
target_link_libraries(CPQ PRIVATE Engine)
add_test(NAME CPQ COMMAND CPQ)