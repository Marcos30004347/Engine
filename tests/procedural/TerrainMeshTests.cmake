cmake_minimum_required(VERSION 3.10)

project(TerrainMeshTests)
get_filename_component(TEST_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)

add_executable(TerrainMeshTests ${TEST_DIR}/TerrainMeshTests.cpp)
target_link_libraries(TerrainMeshTests PRIVATE Engine)
add_test(NAME TerrainMeshTests COMMAND TerrainMeshTests)
set_tests_properties(
  TerrainMeshTests
  PROPERTIES
    TIMEOUT 120
    LABELS "world-map;terrain-mesh")

add_executable(TerrainCliffTests ${TEST_DIR}/TerrainCliffTests.cpp)
target_link_libraries(TerrainCliffTests PRIVATE Engine)
add_test(NAME TerrainCliffTests COMMAND TerrainCliffTests)
set_tests_properties(
  TerrainCliffTests
  PROPERTIES
    TIMEOUT 180
    LABELS "world-map;terrain-mesh;cliffs")

add_executable(TerrainRasterTests ${TEST_DIR}/TerrainRasterTests.cpp)
target_link_libraries(TerrainRasterTests PRIVATE Engine)
add_test(NAME TerrainRasterTests COMMAND TerrainRasterTests)
set_tests_properties(
  TerrainRasterTests
  PROPERTIES
    TIMEOUT 300
    LABELS "world-map;terrain-mesh;raster")

add_executable(TerrainMapSourceTests ${TEST_DIR}/TerrainMapSourceTests.cpp)
target_link_libraries(TerrainMapSourceTests PRIVATE Engine)
add_test(NAME TerrainMapSourceTests COMMAND TerrainMapSourceTests)
set_tests_properties(TerrainMapSourceTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-mesh;raster")

add_executable(TerrainLandformTests ${TEST_DIR}/TerrainLandformTests.cpp)
target_link_libraries(TerrainLandformTests PRIVATE Engine)
add_test(NAME TerrainLandformTests COMMAND TerrainLandformTests)
set_tests_properties(TerrainLandformTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-mesh;cliffs")

add_executable(TerrainGuideTests ${TEST_DIR}/TerrainGuideTests.cpp)
target_link_libraries(TerrainGuideTests PRIVATE Engine)
add_test(NAME TerrainGuideTests COMMAND TerrainGuideTests)
set_tests_properties(TerrainGuideTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-mesh;guides")

add_executable(TerrainRiverSurfaceTests ${TEST_DIR}/TerrainRiverSurfaceTests.cpp)
target_link_libraries(TerrainRiverSurfaceTests PRIVATE Engine)
add_test(NAME TerrainRiverSurfaceTests COMMAND TerrainRiverSurfaceTests)
set_tests_properties(TerrainRiverSurfaceTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-mesh;rivers")


add_executable(TerrainSettlementTests ${TEST_DIR}/TerrainSettlementTests.cpp)
target_link_libraries(TerrainSettlementTests PRIVATE Engine)
add_test(NAME TerrainSettlementTests COMMAND TerrainSettlementTests)
set_tests_properties(TerrainSettlementTests PROPERTIES TIMEOUT 180 LABELS "world-map;terrain-mesh;settlements")

add_executable(TerrainParcelTests ${TEST_DIR}/TerrainParcelTests.cpp)
target_link_libraries(TerrainParcelTests PRIVATE Engine)
add_test(NAME TerrainParcelTests COMMAND TerrainParcelTests)
set_tests_properties(TerrainParcelTests PROPERTIES TIMEOUT 180 LABELS "world-map;terrain-mesh;parcels")

# CPU-only generation contracts: no renderer/device required to validate topology.
add_executable(WorldGenerationContractsTests
    ${TEST_DIR}/WorldGenerationContractsTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldGenerationContracts.cpp)
target_include_directories(WorldGenerationContractsTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldGenerationContractsTests COMMAND WorldGenerationContractsTests)
set_tests_properties(WorldGenerationContractsTests PROPERTIES TIMEOUT 30 LABELS "world-map;terrain-rewrite")

add_executable(WorldPatchMeshTests
    ${TEST_DIR}/WorldPatchMeshTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldGenerationContracts.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPatchMesh.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/ConstrainedDelaunay.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp)
target_include_directories(WorldPatchMeshTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldPatchMeshTests COMMAND WorldPatchMeshTests)
set_tests_properties(WorldPatchMeshTests PROPERTIES TIMEOUT 30 LABELS "world-map;terrain-rewrite;terrain-mesh")

add_executable(WorldLandProductTests
    ${TEST_DIR}/WorldLandProductTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCoastDetail.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldLandGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapPartition.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldBrief.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapCoast.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/PoissonDiskSampler.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldLandProduct.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldGenerationContracts.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPatchMesh.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/ConstrainedDelaunay.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp)
target_include_directories(WorldLandProductTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldLandProductTests COMMAND WorldLandProductTests)
set_tests_properties(WorldLandProductTests PROPERTIES TIMEOUT 60 LABELS "world-map;terrain-rewrite")

add_executable(WorldRiverGraphTests
    ${TEST_DIR}/WorldRiverGraphTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldLandProduct.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldGenerationContracts.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPatchMesh.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/ConstrainedDelaunay.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp)
target_include_directories(WorldRiverGraphTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldRiverGraphTests COMMAND WorldRiverGraphTests)
set_tests_properties(WorldRiverGraphTests PROPERTIES TIMEOUT 60 LABELS "world-map;terrain-rewrite;rivers")

add_executable(WorldWatershedTests
    ${TEST_DIR}/WorldWatershedTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldLandGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCoastDetail.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapPartition.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldBrief.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapCoast.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/PoissonDiskSampler.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldWatersheds.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldReliefSurface.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldLandProduct.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldGenerationContracts.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPatchMesh.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/ConstrainedDelaunay.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp)
target_include_directories(WorldWatershedTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldWatershedTests COMMAND WorldWatershedTests)
set_tests_properties(WorldWatershedTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-rewrite;watersheds")

get_target_property(RELIEF_ACCEPTANCE_SOURCES WorldWatershedTests SOURCES)
list(REMOVE_ITEM RELIEF_ACCEPTANCE_SOURCES "${TEST_DIR}/WorldWatershedTests.cpp")
add_executable(WorldReliefAcceptanceTests
    ${TEST_DIR}/WorldReliefAcceptanceTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRidgeFeatures.cpp
    ${RELIEF_ACCEPTANCE_SOURCES})
target_include_directories(WorldReliefAcceptanceTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldReliefAcceptanceTests COMMAND WorldReliefAcceptanceTests)
set_tests_properties(WorldReliefAcceptanceTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-rewrite;relief-acceptance")

if(APPLE)
  add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/tools/TerrainRewriteNativeCapture"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tools"
    COMMAND xcrun swiftc -O "${CMAKE_SOURCE_DIR}/tools/terrain/render_rewrite_metal.swift"
            -o "${CMAKE_BINARY_DIR}/tools/TerrainRewriteNativeCapture"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/terrain/render_rewrite_metal.swift"
    VERBATIM)
  add_custom_target(TerrainRewriteNativeCapture
    DEPENDS "${CMAKE_BINARY_DIR}/tools/TerrainRewriteNativeCapture")
endif()

add_executable(WorldSplineLandformsTests
    ${TEST_DIR}/WorldSplineLandformsTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldGenerationContracts.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPatchMesh.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/ConstrainedDelaunay.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp)
target_include_directories(WorldSplineLandformsTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldSplineLandformsTests COMMAND WorldSplineLandformsTests)
set_tests_properties(WorldSplineLandformsTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-rewrite;splines")

set(PHYSICAL_MESH_SOURCES
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldGenerationContracts.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPatchMesh.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/ConstrainedDelaunay.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp)
add_executable(WorldPhysicalTerrainTests ${TEST_DIR}/WorldPhysicalTerrainTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPhysicalTerrain.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp ${PHYSICAL_MESH_SOURCES})
add_executable(WorldRiverChannelsTests ${TEST_DIR}/WorldRiverChannelsTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPhysicalTerrain.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldChannelMesh.cpp ${PHYSICAL_MESH_SOURCES})
foreach(TARGET WorldPhysicalTerrainTests WorldRiverChannelsTests)
  target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
  add_test(NAME ${TARGET} COMMAND ${TARGET})
  set_tests_properties(${TARGET} PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-rewrite;physical-water")
endforeach()

add_executable(WorldRiverRoutingTests ${TEST_DIR}/WorldRiverRoutingTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverRouting.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPhysicalTerrain.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldChannelMesh.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldRiverRoutingTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldRiverRoutingTests COMMAND WorldRiverRoutingTests)
set_tests_properties(WorldRiverRoutingTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-rewrite;river-routing")

add_executable(WorldSuitabilityTests ${TEST_DIR}/WorldSuitabilityTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSuitability.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPhysicalTerrain.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldSuitabilityTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldSuitabilityTests COMMAND WorldSuitabilityTests)
set_tests_properties(WorldSuitabilityTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-rewrite;suitability")

add_executable(WorldRoadNetworkTests ${TEST_DIR}/WorldRoadNetworkTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRoadNetwork.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSettlementRoads.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSuitability.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPhysicalTerrain.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldRoadNetworkTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldRoadNetworkTests COMMAND WorldRoadNetworkTests)
set_tests_properties(WorldRoadNetworkTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-rewrite;roads")

add_executable(WorldRoadEarthworksTests ${TEST_DIR}/WorldRoadEarthworksTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRoadEarthworks.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRoadNetwork.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldRoadEarthworksTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldRoadEarthworksTests COMMAND WorldRoadEarthworksTests)
set_tests_properties(WorldRoadEarthworksTests PROPERTIES TIMEOUT 180 LABELS "world-map;terrain-rewrite;road-earthworks")

add_executable(TownLayoutTests ${TEST_DIR}/TownLayoutTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Polygon.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Arrangement.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Boolean.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/TownLayout.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/TownStreets.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/TownBlocks.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/WardBuildings.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/RingLotPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/LotSplitter.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/PatchTiling.cpp)
target_include_directories(TownLayoutTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
# The layout splits its blocks on the worker pool, which lives in the engine library.
target_link_libraries(TownLayoutTests PRIVATE Engine)
add_test(NAME TownLayoutTests COMMAND TownLayoutTests)
set_tests_properties(TownLayoutTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-rewrite;city-layout")

add_executable(WorldCityLayoutTests ${TEST_DIR}/WorldCityLayoutTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityMasterPlan.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityBuildingRegions.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityLayout.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/citylayout/HousingStrategy.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/citylayout/LayoutStage.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/RoadGrowth.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/WardPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/LotPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/RingLotPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/WardBuildings.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/PatchTiling.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldCityLayoutTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityLayoutTests COMMAND WorldCityLayoutTests)
set_tests_properties(WorldCityLayoutTests PROPERTIES TIMEOUT 240 LABELS "world-map;terrain-rewrite;city-layout")

add_executable(WorldCityMasterPlanTests ${TEST_DIR}/WorldCityMasterPlanTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityMasterPlan.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldCityMasterPlanTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityMasterPlanTests COMMAND WorldCityMasterPlanTests)
set_tests_properties(WorldCityMasterPlanTests PROPERTIES TIMEOUT 180 LABELS "world-map;terrain-rewrite;city-plan")

add_executable(WorldCityWallsTests ${TEST_DIR}/WorldCityWallsTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityWalls.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldCityWallsTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityWallsTests COMMAND WorldCityWallsTests)
set_tests_properties(WorldCityWallsTests PROPERTIES TIMEOUT 180 LABELS "world-map;terrain-rewrite;city-walls")

add_executable(WorldCityBuildingRegionsTests ${TEST_DIR}/WorldCityBuildingRegionsTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityBuildingRegions.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldCityBuildingRegionsTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityBuildingRegionsTests COMMAND WorldCityBuildingRegionsTests)
set_tests_properties(WorldCityBuildingRegionsTests PROPERTIES TIMEOUT 240 LABELS "world-map;terrain-rewrite;city-building-regions")

add_executable(WorldSettlementAssemblyTests ${TEST_DIR}/WorldSettlementAssemblyTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSettlementAssembly.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityMasterPlan.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityWalls.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityBuildingRegions.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRoadNetwork.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRoadEarthworks.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldSettlementAssemblyTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldSettlementAssemblyTests COMMAND WorldSettlementAssemblyTests)
set_tests_properties(WorldSettlementAssemblyTests PROPERTIES TIMEOUT 240 LABELS "world-map;terrain-rewrite;settlement-assembly")

add_executable(WorldCityTerritoryTests ${TEST_DIR}/WorldCityTerritoryTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldBrief.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapPartition.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapCoast.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/PoissonDiskSampler.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityTerritory.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityLayout.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/citylayout/HousingStrategy.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/citylayout/LayoutStage.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/RoadGrowth.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/WardPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/LotPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/RingLotPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/WardBuildings.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/PatchTiling.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityMasterPlan.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityBuildingRegions.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRuralPatches.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/rural/PatchGrowth.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldFarmFields.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldFinalPatches.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchAudit.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchGeometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/FreezeStage.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSuitability.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPhysicalTerrain.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldCityTerritoryTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityTerritoryTests COMMAND WorldCityTerritoryTests)
set_tests_properties(WorldCityTerritoryTests PROPERTIES TIMEOUT 240 LABELS "world-map;terrain-rewrite;city-territory")

add_executable(WorldCitySkeletonTests ${TEST_DIR}/WorldCitySkeletonTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCitySkeleton.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/CityBlockPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/PlazaPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Polygon.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Boolean.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Offset.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Arrangement.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp)
target_include_directories(WorldCitySkeletonTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCitySkeletonTests COMMAND WorldCitySkeletonTests)
set_tests_properties(WorldCitySkeletonTests PROPERTIES TIMEOUT 60 LABELS "world-map;terrain-rewrite;city-skeleton")

add_executable(WorldCityExteriorTests ${TEST_DIR}/WorldCityExteriorTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCitySkeleton.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/CityBlockPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/PlazaPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Polygon.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Boolean.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Offset.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Arrangement.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityExterior.cpp)
target_include_directories(WorldCityExteriorTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityExteriorTests COMMAND WorldCityExteriorTests)
set_tests_properties(WorldCityExteriorTests PROPERTIES TIMEOUT 60 LABELS "world-map;terrain-rewrite;city-exterior")

add_executable(WorldCityAgricultureTests ${TEST_DIR}/WorldCityAgricultureTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityAgriculture.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCitySkeleton.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/CityBlockPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/PlazaPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Polygon.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Boolean.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Offset.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geom2/Arrangement.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp)
target_include_directories(WorldCityAgricultureTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityAgricultureTests COMMAND WorldCityAgricultureTests)
set_tests_properties(WorldCityAgricultureTests PROPERTIES TIMEOUT 60 LABELS "world-map;terrain-rewrite;city-agriculture")

add_executable(WorldCityCastleTests ${TEST_DIR}/WorldCityCastleTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityCastle.cpp)
target_include_directories(WorldCityCastleTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityCastleTests COMMAND WorldCityCastleTests)
set_tests_properties(WorldCityCastleTests PROPERTIES TIMEOUT 60 LABELS "world-map;terrain-rewrite;city-castle")

add_executable(WorldCityGrowthTests ${TEST_DIR}/WorldCityGrowthTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityGrowth.cpp)
target_include_directories(WorldCityGrowthTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityGrowthTests COMMAND WorldCityGrowthTests)
set_tests_properties(WorldCityGrowthTests PROPERTIES TIMEOUT 60 LABELS "world-map;terrain-rewrite;city-growth")

add_executable(WorldCityCompletionTests ${TEST_DIR}/WorldCityCompletionTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityCompletion.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityRelief.cpp)
target_include_directories(WorldCityCompletionTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCityCompletionTests COMMAND WorldCityCompletionTests)
set_tests_properties(WorldCityCompletionTests PROPERTIES TIMEOUT 60 LABELS "world-map;terrain-rewrite;city-completion")

add_executable(WorldRuralPatchesTests ${TEST_DIR}/WorldRuralPatchesTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRuralPatches.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/rural/PatchGrowth.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSuitability.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPhysicalTerrain.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldRuralPatchesTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldRuralPatchesTests COMMAND WorldRuralPatchesTests)
set_tests_properties(WorldRuralPatchesTests PROPERTIES TIMEOUT 180 LABELS "world-map;terrain-rewrite;rural-patches")

add_executable(WorldFarmFieldsTests ${TEST_DIR}/WorldFarmFieldsTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldFarmFields.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldFarmFieldsTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldFarmFieldsTests COMMAND WorldFarmFieldsTests)
set_tests_properties(WorldFarmFieldsTests PROPERTIES TIMEOUT 240 LABELS "world-map;terrain-rewrite;farm-fields")

add_executable(WorldFinalPatchesTests ${TEST_DIR}/WorldFinalPatchesTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldFinalPatches.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchAudit.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchGeometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/FreezeStage.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldFarmFields.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldFinalPatchesTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldFinalPatchesTests COMMAND WorldFinalPatchesTests)
set_tests_properties(WorldFinalPatchesTests PROPERTIES TIMEOUT 240 LABELS "world-map;terrain-rewrite;final-patches")

add_executable(WorldSurfaceRefinementTests ${TEST_DIR}/WorldSurfaceRefinementTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceRefinement.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldFinalPatches.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchAudit.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchGeometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/FreezeStage.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldSurfaceRefinementTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldSurfaceRefinementTests COMMAND WorldSurfaceRefinementTests)
set_tests_properties(WorldSurfaceRefinementTests PROPERTIES TIMEOUT 240 LABELS "world-map;terrain-rewrite;surface-refinement")

add_executable(WorldCoverDetailTests ${TEST_DIR}/WorldCoverDetailTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCoverDetail.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldFinalPatches.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchAudit.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchGeometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/FreezeStage.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldCoverDetailTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldCoverDetailTests COMMAND WorldCoverDetailTests)
set_tests_properties(WorldCoverDetailTests PROPERTIES TIMEOUT 240 LABELS "world-map;terrain-rewrite;cover-detail")

add_executable(WorldRuntimePublicationTests ${TEST_DIR}/WorldRuntimePublicationTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRuntimeArchive.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRuntimeDisplay.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRuntimeCollision.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRuntimeCover.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRuntimeBuild.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRuntimePublication.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCoverDetail.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceRefinement.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldFinalPatches.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchAudit.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/PatchGeometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/finalpatches/FreezeStage.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSurfaceArrangement.cpp ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldRuntimePublicationTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME WorldRuntimePublicationTests COMMAND WorldRuntimePublicationTests)
set_tests_properties(WorldRuntimePublicationTests PROPERTIES TIMEOUT 120 LABELS "world-map;terrain-rewrite;runtime-publication")

# Whole-island integration source export; composition is inspected at world scale.
add_executable(WorldIslandFoundation ${CMAKE_SOURCE_DIR}/tools/terrain/WorldIslandFoundation.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldRiverChannels.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldPhysicalTerrain.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSplineLandforms.cpp
    ${RELIEF_ACCEPTANCE_SOURCES})
target_include_directories(WorldIslandFoundation PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)

add_executable(WorldTerrainErosion ${CMAKE_SOURCE_DIR}/tools/terrain/WorldTerrainErosion.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/geometry/TerrainHydraulicErosion.cpp)
target_include_directories(WorldTerrainErosion PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)

add_executable(WorldSettlementDirectorTests ${TEST_DIR}/WorldSettlementDirectorTests.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityDirectorAdapter.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityDemand.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityMasterPlan.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldCityInteriors.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldSettlementDirector.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldDwellingPlan.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/generator/DwellingPlanner.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapPartition.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapGraph.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/MapCoast.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/WorldBrief.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/PoissonDiskSampler.cpp
    ${PHYSICAL_MESH_SOURCES})
target_include_directories(WorldSettlementDirectorTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
target_compile_definitions(WorldSettlementDirectorTests PRIVATE CITY_PROGRAMME_CATALOGUE_PATH="${CMAKE_SOURCE_DIR}/tools/terrain/data/city-building-programs.v1.json")
add_test(NAME WorldSettlementDirectorTests COMMAND WorldSettlementDirectorTests)
set_tests_properties(WorldSettlementDirectorTests PROPERTIES TIMEOUT 240 LABELS "world-map;city-director;interiors")

# Standalone director qualification, including the unchanged legacy pipeline.
set(DIRECTOR_MAP_SOURCES)
foreach(name WorldPipeline stages/WorldStage WorldPipelineEditSession WorldPipelineDirector WorldGeneratorDirector WorldGenerationSession WorldGenerationEdits WorldMapPatchAdapter WorldEditablePatches WorldPatchGeneratorAdapters WorldFarmFields WorldDwellingPlan generator/DwellingPlanner WorldSurfaceArrangement WorldPatchMesh WorldGenerationContracts
    MapEdges MapHierarchy MapPartition MapGraph MapLandmass Raisers WorldBrief MapCoast MapElevation
    MapTerrain MapFluvial MapPlateaus MapClimate MapRivers MapStylize MapSurface MapContours MapRegions)
  list(APPEND DIRECTOR_MAP_SOURCES ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/${name}.cpp)
endforeach()
add_executable(WorldGeneratorDirectorTests ${TEST_DIR}/WorldGeneratorDirectorTests.cpp
    ${TEST_DIR}/fixtures/WorldPipelineReference.cpp
    ${DIRECTOR_MAP_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Geometry.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/PoissonDiskSampler.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/ConstrainedDelaunay.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/Predicates.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/land/Blend.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/procedural/world/land/Dendry.cpp)
target_include_directories(WorldGeneratorDirectorTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME WorldGeneratorDirectorTests COMMAND WorldGeneratorDirectorTests)
set_tests_properties(WorldGeneratorDirectorTests PROPERTIES TIMEOUT 180 LABELS "world-map;generator-director")

add_executable(WorldGeneratorIntegrationTests ${TEST_DIR}/WorldGeneratorIntegrationTests.cpp)
target_link_libraries(WorldGeneratorIntegrationTests PRIVATE Engine)
add_test(NAME WorldGeneratorIntegrationTests COMMAND WorldGeneratorIntegrationTests)
set_tests_properties(WorldGeneratorIntegrationTests PROPERTIES TIMEOUT 180 LABELS "world-map;generator-director;runtime-publication")

# Actual-world settlement integration preview: SVG/HTML/JSON evidence for W0/W1.
add_executable(WorldSettlementPreview ${CMAKE_SOURCE_DIR}/tools/terrain/WorldSettlementPreview.cpp)
target_link_libraries(WorldSettlementPreview PRIVATE Engine)
target_include_directories(WorldSettlementPreview PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_executable(CityVoxelReleaseAuditTool ${CMAKE_SOURCE_DIR}/tools/terrain/CityVoxelReleaseAuditTool.cpp)
target_link_libraries(CityVoxelReleaseAuditTool PRIVATE Engine)
target_include_directories(CityVoxelReleaseAuditTool PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_executable(CityVoxelOBJPackTool ${CMAKE_SOURCE_DIR}/tools/terrain/CityVoxelOBJPackTool.cpp)
target_link_libraries(CityVoxelOBJPackTool PRIVATE Engine)
target_include_directories(CityVoxelOBJPackTool PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_executable(CityVoxelInstancePackTool ${CMAKE_SOURCE_DIR}/tools/terrain/CityVoxelInstancePackTool.cpp)
target_link_libraries(CityVoxelInstancePackTool PRIVATE Engine)
target_include_directories(CityVoxelInstancePackTool PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)

add_executable(CityBlockPlannerTests ${TEST_DIR}/CityBlockPlannerTests.cpp)
target_link_libraries(CityBlockPlannerTests PRIVATE runtime)
target_include_directories(CityBlockPlannerTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME CityBlockPlannerTests COMMAND CityBlockPlannerTests)
set_tests_properties(CityBlockPlannerTests PROPERTIES TIMEOUT 120 LABELS "world-map;city-block")

add_executable(GothicCastleTests ${TEST_DIR}/GothicCastleTests.cpp)
target_link_libraries(GothicCastleTests PRIVATE runtime)
target_include_directories(GothicCastleTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME GothicCastleTests COMMAND GothicCastleTests)
set_tests_properties(GothicCastleTests PROPERTIES TIMEOUT 120 LABELS "world-map;castle;dungeon")

add_executable(PlazaPlannerTests ${TEST_DIR}/PlazaPlannerTests.cpp)
target_link_libraries(PlazaPlannerTests PRIVATE runtime)
target_include_directories(PlazaPlannerTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime)
add_test(NAME PlazaPlannerTests COMMAND PlazaPlannerTests)
set_tests_properties(PlazaPlannerTests PROPERTIES TIMEOUT 120 LABELS "world-map;city-block;plaza")

add_executable(TradeRoomsTests ${TEST_DIR}/TradeRoomsTests.cpp)
target_link_libraries(TradeRoomsTests PRIVATE runtime)
target_include_directories(TradeRoomsTests PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf)
add_test(NAME TradeRoomsTests COMMAND TradeRoomsTests)
set_tests_properties(TradeRoomsTests PROPERTIES TIMEOUT 300 LABELS "world-map;dwellings;history")
add_executable(CityVoxelSceneStats ${CMAKE_SOURCE_DIR}/tools/terrain/CityVoxelSceneStats.cpp)
target_link_libraries(CityVoxelSceneStats PRIVATE Engine)
