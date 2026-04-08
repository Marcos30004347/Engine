cmake_minimum_required(VERSION 3.10)

project(OBJMaterialBridgeTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(OBJMaterialBridgeTests ${TEST_DIR}/OBJMaterialBridgeTests.cpp)
target_link_libraries(OBJMaterialBridgeTests PRIVATE Engine)
add_test(NAME OBJMaterialBridgeTests COMMAND OBJMaterialBridgeTests)

add_dependencies(OBJMaterialBridgeTests copy_mesh_assets)
