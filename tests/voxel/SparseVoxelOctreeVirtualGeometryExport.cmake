cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelOctreeVirtualGeometryExport
  ${TEST_DIR}/SparseVoxelOctreeVirtualGeometryExport.cpp)
target_link_libraries(SparseVoxelOctreeVirtualGeometryExport PRIVATE Engine)

add_test(
  NAME SparseVoxelOctreeVirtualGeometryExport
  COMMAND SparseVoxelOctreeVirtualGeometryExport)
set_tests_properties(
  SparseVoxelOctreeVirtualGeometryExport
  PROPERTIES
    TIMEOUT 300
    LABELS "svraster.virtualgeometry;cpu")
