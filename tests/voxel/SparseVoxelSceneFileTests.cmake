cmake_minimum_required(VERSION 3.10)

project(SparseVoxelSceneFileTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SparseVoxelSceneFileTests ${TEST_DIR}/SparseVoxelSceneFileTests.cpp)
target_include_directories(SparseVoxelSceneFileTests PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/lz4/lib)
target_link_libraries(SparseVoxelSceneFileTests PRIVATE Engine)

add_test(NAME VirtualSparseVoxelSceneFileG14Tests COMMAND SparseVoxelSceneFileTests --g14)
set_tests_properties(VirtualSparseVoxelSceneFileG14Tests PROPERTIES LABELS "svraster.g14;svraster.m2")

add_test(NAME VirtualSparseVoxelSceneFileG15Tests COMMAND SparseVoxelSceneFileTests --g15)
set_tests_properties(VirtualSparseVoxelSceneFileG15Tests PROPERTIES LABELS "svraster.g15;svraster.m2;svraster.phase6")
