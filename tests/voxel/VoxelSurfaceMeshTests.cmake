add_executable(VoxelSurfaceMeshTests
    ${CMAKE_CURRENT_LIST_DIR}/VoxelSurfaceMeshTests.cpp)
target_link_libraries(VoxelSurfaceMeshTests PRIVATE Engine)
add_test(NAME VoxelSurfaceMeshTests COMMAND VoxelSurfaceMeshTests)
set_tests_properties(VoxelSurfaceMeshTests PROPERTIES LABELS "virtualgeometry;voxel;dwellings")
