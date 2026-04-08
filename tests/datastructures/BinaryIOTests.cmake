cmake_minimum_required(VERSION 3.10)

project (BinaryIOTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(BinaryIOTests ${TEST_DIR}/BinaryIOTests.cpp)
target_link_libraries(BinaryIOTests PRIVATE Engine)
add_test(NAME BinaryIOTests COMMAND BinaryIOTests)

