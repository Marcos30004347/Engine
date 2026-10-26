cmake_minimum_required(VERSION 3.10)

project (Geom2CellGridTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(Geom2CellGridTests ${TEST_DIR}/Geom2CellGridTests.cpp)
target_link_libraries(Geom2CellGridTests PRIVATE Engine)
add_test(NAME Geom2CellGridTests COMMAND Geom2CellGridTests)
set_tests_properties(Geom2CellGridTests PROPERTIES LABELS "geom2;cells")
