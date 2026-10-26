add_executable(
  SparseVoxelSceneRayGPUTests
  ${TEST_DIR}/SparseVoxelSceneRayGPUTests.cpp)
target_link_libraries(SparseVoxelSceneRayGPUTests PRIVATE Engine)
add_dependencies(
  SparseVoxelSceneRayGPUTests
  SparseVoxelRasterShaders)

add_test(
  NAME SparseVoxelSceneRayGPUValidationTests
  COMMAND SparseVoxelSceneRayGPUTests)
set_tests_properties(
  SparseVoxelSceneRayGPUValidationTests
  PROPERTIES LABELS "svraster.scene-rays;gpu")

add_test(
  NAME SparseVoxelLightFieldVisibilityLocalityGPU
  COMMAND SparseVoxelSceneRayGPUTests --light-field-visibility-locality-only)
set_tests_properties(
  SparseVoxelLightFieldVisibilityLocalityGPU
  PROPERTIES LABELS "svraster.scene-rays;gpu;light-field")

add_test(
  NAME SparseVoxelFullRateViewportEdgeGPU
  COMMAND SparseVoxelSceneRayGPUTests --edge-schedule-only)
set_tests_properties(
  SparseVoxelFullRateViewportEdgeGPU
  PROPERTIES LABELS "svraster.scene-rays;gpu;light-field")

add_test(
  NAME SparseVoxelReSTIRHistoryGPU
  COMMAND SparseVoxelSceneRayGPUTests --restir-history-only)
set_tests_properties(
  SparseVoxelReSTIRHistoryGPU
  PROPERTIES LABELS "svraster.scene-rays;gpu;light-field;restir")

add_test(
  NAME SparseVoxelHitTemporalGPU
  COMMAND SparseVoxelSceneRayGPUTests --hit-temporal-only)
set_tests_properties(
  SparseVoxelHitTemporalGPU
  PROPERTIES LABELS "svraster.scene-rays;svraster.temporal;gpu")

add_test(
  NAME SparseVoxelScreenTree64MergeGPU
  COMMAND SparseVoxelSceneRayGPUTests --screen-world-merge-only)
set_tests_properties(
  SparseVoxelScreenTree64MergeGPU
  PROPERTIES LABELS "svraster.scene-rays;svraster.screen-trace;svraster.tree64;gpu")

add_test(
  NAME SparseVoxelTerrainRayOriginShadowSpanGPU
  COMMAND SparseVoxelSceneRayGPUTests --terrain-ray-origin-only)
set_tests_properties(
  SparseVoxelTerrainRayOriginShadowSpanGPU
  PROPERTIES LABELS "svraster.scene-rays;svraster.terrain;svraster.tree64;gpu")

add_test(
  NAME SparseVoxelLightingAccumulationGPU
  COMMAND SparseVoxelSceneRayGPUTests --lighting-accumulation-only)
set_tests_properties(
  SparseVoxelLightingAccumulationGPU
  PROPERTIES LABELS "svraster.scene-rays;svraster.temporal;gpu")
