cmake_minimum_required(VERSION 3.10)

get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(
  SparseVoxelTerrainV2Phase10Tests
  ${TEST_DIR}/SparseVoxelTerrainV2Phase10Tests.cpp)
target_link_libraries(SparseVoxelTerrainV2Phase10Tests PRIVATE Engine)

add_test(
  NAME SparseVoxelTerrainV2Phase10Rollout
  COMMAND SparseVoxelTerrainV2Phase10Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase10Rollout
  PROPERTIES
    TIMEOUT 180
    LABELS "svraster.terrain.v2.phase10;svraster.terrain.rollout;cpu")

add_test(
  NAME SparseVoxelTerrainV2Phase10GPUOccupancySoak
  COMMAND SparseVoxelTerrainV2Phase4GPUTests ${CMAKE_SOURCE_DIR}/world-data)
set_tests_properties(
  SparseVoxelTerrainV2Phase10GPUOccupancySoak
  PROPERTIES TIMEOUT 240
    LABELS "svraster.terrain.v2.phase10;svraster.terrain.rollout;gpu")

add_test(
  NAME SparseVoxelTerrainV2Phase10GPUTree64Soak
  COMMAND SparseVoxelTerrainV2Phase5GPUTests)
set_tests_properties(
  SparseVoxelTerrainV2Phase10GPUTree64Soak
  PROPERTIES TIMEOUT 120
    LABELS "svraster.terrain.v2.phase10;svraster.terrain.rollout;gpu")

add_test(
  NAME SparseVoxelTerrainV2Phase10GPUPublicationSoak
  COMMAND SparseVoxelTerrainV2Phase7GPUTests)
set_tests_properties(
  SparseVoxelTerrainV2Phase10GPUPublicationSoak
  PROPERTIES TIMEOUT 120
    LABELS "svraster.terrain.v2.phase10;svraster.terrain.rollout;gpu")

add_test(
  NAME SparseVoxelTerrainV2Phase10GroundStabilitySoak
  COMMAND SparseVoxelTerrainV2Phase9Tests)
set_tests_properties(
  SparseVoxelTerrainV2Phase10GroundStabilitySoak
  PROPERTIES TIMEOUT 120
    LABELS "svraster.terrain.v2.phase10;svraster.terrain.rollout;cpu")

# Phase 10 keeps the renderer-independent runtime free of generated-payload
# read paths and synchronous renderer drains. Compact keys and telemetry are
# allowed; geometry, occupancy, materials, and Tree64 payloads are not.
file(READ
  "${CMAKE_SOURCE_DIR}/src/voxel/svraster/TerrainGPUV2Runtime.cpp"
  TERRAIN_V2_PHASE10_RUNTIME_SOURCE)
if(TERRAIN_V2_PHASE10_RUNTIME_SOURCE MATCHES "bufferRead\\(" OR
   TERRAIN_V2_PHASE10_RUNTIME_SOURCE MATCHES "readback" OR
   TERRAIN_V2_PHASE10_RUNTIME_SOURCE MATCHES "waitIdle\\(" OR
   TERRAIN_V2_PHASE10_RUNTIME_SOURCE MATCHES "drainFrame\\(")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 10 runtime contains a forbidden payload read or queue drain")
endif()

file(READ
  "${CMAKE_SOURCE_DIR}/src/voxel/svraster/TerrainGPUV2Rollout.cpp"
  TERRAIN_V2_PHASE10_ROLLOUT_SOURCE)
if(TERRAIN_V2_PHASE10_ROLLOUT_SOURCE MATCHES "payloadCache|bufferRead|readback")
  message(FATAL_ERROR
    "Terrain GPU V2 Phase 10 rollout must not own a CPU generated-payload cache")
endif()
