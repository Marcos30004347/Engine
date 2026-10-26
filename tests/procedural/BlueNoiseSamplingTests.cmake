cmake_minimum_required(VERSION 3.10)

project (BlueNoiseSamplingTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(BlueNoiseSamplingTests ${TEST_DIR}/BlueNoiseSamplingTests.cpp)
target_link_libraries(BlueNoiseSamplingTests PRIVATE Engine)
add_test(NAME BlueNoiseSamplingTests COMMAND BlueNoiseSamplingTests)
