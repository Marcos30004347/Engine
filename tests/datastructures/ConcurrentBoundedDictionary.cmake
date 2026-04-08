cmake_minimum_required(VERSION 3.10)

project (ConcurrentBoundedDictionary)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(ConcurrentBoundedDictionary ${TEST_DIR}/ConcurrentBoundedDictionary.cpp)
target_link_libraries(ConcurrentBoundedDictionary PRIVATE Engine)
add_test(NAME ConcurrentBoundedDictionary COMMAND ConcurrentBoundedDictionary)