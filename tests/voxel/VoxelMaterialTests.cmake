add_executable(
  VoxelMaterialTests
  ${TEST_DIR}/VoxelMaterialTests.cpp)
target_link_libraries(VoxelMaterialTests PRIVATE Engine)

add_test(
  NAME VoxelMaterialStableSlotTests
  COMMAND VoxelMaterialTests)
set_tests_properties(
  VoxelMaterialStableSlotTests
  PROPERTIES LABELS "svraster.voxelmaterial.cpu;svraster.phase0;svraster.phase1;svraster.phase2;svraster.phase3;svraster.phase4;svraster.phase5;svraster.phase6;svraster.phase7;svraster.phase8")
