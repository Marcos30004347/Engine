cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

wgsl2spirv(
  NAME atmosphere_pass_gpu_inspect
  SRC ${TEST_DIR}/assets/atmosphere_pass_inspect.wgsl
  DST ${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/atmosphere/atmosphere-pass-inspect.spirv)

add_executable(
  AtmospherePassGPUTests
  ${TEST_DIR}/AtmospherePassGPUTests.cpp)
target_link_libraries(AtmospherePassGPUTests PRIVATE Engine)
add_dependencies(
  AtmospherePassGPUTests
  AtmosphereShaders
  SparseVoxelRasterShaders
  atmosphere_pass_gpu_inspect)

add_test(
  NAME AtmospherePassGPUValidationTests
  COMMAND AtmospherePassGPUTests)
set_tests_properties(
  AtmospherePassGPUValidationTests
  PROPERTIES
    LABELS "atmosphere.gpu;atmosphere.phase1;atmosphere.phase2;atmosphere.phase3;atmosphere.phase4;atmosphere.phase5;atmosphere.phase6;gpu"
    TIMEOUT 300)
