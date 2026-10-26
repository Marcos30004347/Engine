cmake_minimum_required(VERSION 3.10)

project(VirtualTextureSystemTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(VirtualTextureSystemTests ${TEST_DIR}/VirtualTextureSystemTests.cpp)
target_link_libraries(VirtualTextureSystemTests PRIVATE Engine)
add_test(NAME VirtualTextureSystemTests COMMAND VirtualTextureSystemTests)

add_dependencies(VirtualTextureSystemTests copy_mesh_assets)
