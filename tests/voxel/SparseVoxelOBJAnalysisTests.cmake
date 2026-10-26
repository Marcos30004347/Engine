cmake_minimum_required(VERSION 3.10)

project(SparseVoxelOBJAnalysisTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(SparseVoxelOBJAnalysisTests ${TEST_DIR}/SparseVoxelOBJAnalysisTests.cpp)
target_link_libraries(SparseVoxelOBJAnalysisTests PRIVATE Engine)
target_compile_definitions(SparseVoxelOBJAnalysisTests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_test(NAME SparseVoxelOBJAnalysisG17ATests COMMAND SparseVoxelOBJAnalysisTests --g17a)
set_tests_properties(SparseVoxelOBJAnalysisG17ATests PROPERTIES LABELS "svraster.g17a;svraster.m2")

add_test(NAME SparseVoxelOBJBuilderG17BTests COMMAND SparseVoxelOBJAnalysisTests --g17b)
set_tests_properties(SparseVoxelOBJBuilderG17BTests PROPERTIES LABELS "svraster.g17b;svraster.m2")

add_test(NAME SparseVoxelOBJExplicitSaveG17CTests COMMAND SparseVoxelOBJAnalysisTests --g17c)
set_tests_properties(SparseVoxelOBJExplicitSaveG17CTests PROPERTIES LABELS "svraster.g17c;svraster.m2")
