cmake_minimum_required(VERSION 3.14)

add_test(
  NAME SparseVoxelLegacyRemovalG13Tests
  COMMAND ${CMAKE_COMMAND}
          -DENGINE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/voxel/SparseVoxelLegacyRemovalAudit.cmake
)
set_tests_properties(SparseVoxelLegacyRemovalG13Tests PROPERTIES LABELS "svraster.g13;svraster.m1")
