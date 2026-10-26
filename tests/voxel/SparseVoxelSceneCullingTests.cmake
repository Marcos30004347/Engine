cmake_minimum_required(VERSION 3.14)

add_executable(SparseVoxelSceneCullingTests ${TEST_DIR}/SparseVoxelSceneCullingTests.cpp)
target_link_libraries(SparseVoxelSceneCullingTests PRIVATE Engine)
add_dependencies(SparseVoxelSceneCullingTests SparseVoxelRasterShaders)

add_test(NAME SparseVoxelSceneManagerG10Tests COMMAND SparseVoxelSceneCullingTests --g10)
set_tests_properties(SparseVoxelSceneManagerG10Tests PROPERTIES LABELS "svraster.g10;svraster.m1")

add_test(NAME SparseVoxelGPUCullingG11Tests COMMAND SparseVoxelSceneCullingTests --g11)
set_tests_properties(SparseVoxelGPUCullingG11Tests PROPERTIES LABELS "svraster.g11;svraster.m1;gpu")

add_test(NAME SparseVoxelGPUOcclusionG21Tests COMMAND SparseVoxelSceneCullingTests --g21)
set_tests_properties(SparseVoxelGPUOcclusionG21Tests PROPERTIES LABELS "svraster.g21;svraster.m3;gpu")

add_test(NAME SparseVoxelOccupancyTLASQueryG24Tests COMMAND SparseVoxelSceneCullingTests --g24)
set_tests_properties(SparseVoxelOccupancyTLASQueryG24Tests PROPERTIES LABELS "svraster.g24.gpu;svraster.m4;gpu")
