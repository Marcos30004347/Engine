wgsl2spirv(
  NAME sparse_voxel_virtual_trace_abi
  SRC ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/sparse_voxel_virtual_trace_abi.wgsl
  DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/svraster/sparse-voxel-virtual-trace-abi.spirv
  INCLUDES ${CMAKE_SOURCE_DIR}/assets/shaders/voxel/svraster/wgsl/)

add_dependencies(SparseVoxelRasterShaders sparse_voxel_virtual_trace_abi)

add_executable(SparseVoxelVirtualTraceTests ${TEST_DIR}/SparseVoxelVirtualTraceTests.cpp)
target_link_libraries(SparseVoxelVirtualTraceTests PRIVATE Engine)
add_dependencies(SparseVoxelVirtualTraceTests sparse_voxel_virtual_trace_abi)

add_test(NAME SparseVoxelVirtualTraceABIContractTests COMMAND SparseVoxelVirtualTraceTests)
set_tests_properties(SparseVoxelVirtualTraceABIContractTests PROPERTIES LABELS "svraster.virtual-trace;cpu;abi")
