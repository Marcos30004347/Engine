cmake_minimum_required(VERSION 3.10)

project (BufferAllocatorTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(BufferAllocatorTests ${TEST_DIR}/BufferAllocatorTests.cpp)
target_link_libraries(BufferAllocatorTests PRIVATE Engine)
add_test(NAME BufferAllocatorTests COMMAND BufferAllocatorTests)