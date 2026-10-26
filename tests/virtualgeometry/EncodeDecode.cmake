cmake_minimum_required(VERSION 3.10)

project (EncodeDecode)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(EncodeDecode ${TEST_DIR}/EncodeDecode.cpp)
target_link_libraries(EncodeDecode PRIVATE Engine)
add_test(NAME EncodeDecode COMMAND EncodeDecode)

add_dependencies(EncodeDecode copy_mesh_assets)
