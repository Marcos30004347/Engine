cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelOctreeStreamingGPUTests
  ${TEST_DIR}/SparseVoxelOctreeStreamingGPUTests.cpp)
target_link_libraries(SparseVoxelOctreeStreamingGPUTests PRIVATE Engine)
add_dependencies(SparseVoxelOctreeStreamingGPUTests wgsl_virtualgeometry_page_decode_cs)
add_dependencies(SparseVoxelOctreeStreamingGPUTests wgsl_virtualgeometry_cull_prepass_multipass)
add_dependencies(SparseVoxelOctreeStreamingGPUTests wgsl_virtualgeometry_streaming_page_selection_cs)

add_test(
  NAME SparseVoxelOctreeStreamingGPU
  COMMAND SparseVoxelOctreeStreamingGPUTests)
set_tests_properties(
  SparseVoxelOctreeStreamingGPU
  PROPERTIES
    TIMEOUT 600
    LABELS "svraster.virtualgeometry;svraster.streaming;gpu")
