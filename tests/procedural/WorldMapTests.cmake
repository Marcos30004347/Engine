cmake_minimum_required(VERSION 3.10)

project(WorldMapTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(WorldMapTests ${TEST_DIR}/WorldMapTests.cpp)
target_link_libraries(WorldMapTests PRIVATE Engine)
add_test(NAME WorldMapTests COMMAND WorldMapTests)

add_executable(MapPlacementTests ${TEST_DIR}/MapPlacementTests.cpp)
target_link_libraries(MapPlacementTests PRIVATE Engine)
add_test(NAME MapPlacementTests COMMAND MapPlacementTests)
set_tests_properties(
  MapPlacementTests
  PROPERTIES
    TIMEOUT 30
    LABELS "world-map;placement;svraster.tree64.streaming.stability")

if(HAS_VULKAN)
  add_executable(WorldMapGpuTests ${TEST_DIR}/WorldMapGpuTests.cpp)
  target_link_libraries(WorldMapGpuTests PRIVATE Engine)
  add_dependencies(WorldMapGpuTests map_export_pages_test)
  target_compile_definitions(
    WorldMapGpuTests
    PRIVATE MAP_EXPORT_PAGES_TEST_SHADER_PATH="${CMAKE_BINARY_DIR}/tests/assets/shaders/spirv/procedural/map-export-pages-test.spirv")
  add_test(NAME WorldMapGpuTests COMMAND WorldMapGpuTests)
  set_tests_properties(WorldMapGpuTests PROPERTIES LABELS "world-map;gpu")
endif()
