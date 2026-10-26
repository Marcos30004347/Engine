cmake_minimum_required(VERSION 3.10)

project (RobustPredicatesTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(RobustPredicatesTests ${TEST_DIR}/RobustPredicatesTests.cpp)
target_link_libraries(RobustPredicatesTests PRIVATE Engine)
add_test(NAME RobustPredicatesTests COMMAND RobustPredicatesTests)
