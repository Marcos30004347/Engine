#pragma once
#include "WorldCityBuildingRegions.hpp"
#include "WorldCityWalls.hpp"
#include "WorldRoadEarthworks.hpp"

namespace procedural::world::rewrite {
struct SettlementAssemblyInput
{
  std::vector<CityPlanCell> cells;
  std::vector<DistrictRequest> districts;
  std::vector<uint64_t> gateCells;
  std::vector<std::vector<Position>> streets; // Authored/routed paths, stations <=40m apart.
  std::vector<CityWallEra> eras;
  CityPlanSettings planning;
  RoadSettings roads;
  RoadEarthworkSettings earthworks;
  CityWallSettings walls;
  CityBuildingRegionSettings buildings;
  SurfaceRefinement refinement;
};

struct SettlementAssembly
{
  CityMasterPlan plan;
  RoadNetworkProduct roads;
  CityWalls walls;
  CityBuildingRegions buildings;
  SurfaceArrangement map;
  SurfaceArrangementMesh mesh;
};

// Prepared terrain integration. All streets must connect geometrically and serve
// every district anchor and planning gate. No implicit bridges or automatic routing.
// Publishes atomically after final contour arrangement, frontage rebinding and CDT.
bool assembleSettlement(const SettlementAssemblyInput &, const RoadField &terrain, SettlementAssembly &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
