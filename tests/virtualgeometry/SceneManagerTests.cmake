cmake_minimum_required(VERSION 3.10)

project(SceneManagerTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SceneManagerTests ${TEST_DIR}/SceneManagerTests.cpp)
target_link_libraries(SceneManagerTests PRIVATE Engine)
add_test(NAME SceneManagerTests COMMAND SceneManagerTests)
