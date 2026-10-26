cmake_minimum_required(VERSION 3.10)

project (Geom2ArrangementTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(Geom2ArrangementTests ${TEST_DIR}/Geom2ArrangementTests.cpp)
target_link_libraries(Geom2ArrangementTests PRIVATE Engine)
add_test(NAME Geom2ArrangementTests COMMAND Geom2ArrangementTests)
