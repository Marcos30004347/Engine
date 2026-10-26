cmake_minimum_required(VERSION 3.10)

project (VirtualGeometrySceneTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(VirtualGeometrySceneTests ${TEST_DIR}/VirtualGeometrySceneTests.cpp)
target_link_libraries(VirtualGeometrySceneTests PRIVATE Engine)
add_test(NAME VirtualGeometrySceneTests COMMAND VirtualGeometrySceneTests)

add_dependencies(VirtualGeometrySceneTests copy_mesh_assets)
