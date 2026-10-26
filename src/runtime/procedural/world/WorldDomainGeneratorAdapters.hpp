#pragma once
#include "WorldCityDemand.hpp"
#include "WorldCityLayout.hpp"
#include "WorldCityTerritory.hpp"
#include "WorldCoastDetail.hpp"
#include "WorldGeneratorDirector.hpp"
#include "WorldLandGenerator.hpp"
#include "WorldRoadEarthworks.hpp"
#include "WorldRuntimeBuild.hpp"
#include "WorldSettlementDirector.hpp"
#include "WorldWatersheds.hpp"
namespace procedural::world::rewrite
{
// Explicit value briefs keep all context dependencies visible to the scheduler.
// Function-backed terrain fields require versioned, immutable captures.
struct CityPlanGenerationBrief
{
  std::vector<CityPlanCell> cells;
  std::vector<DistrictRequest> districts;
  std::vector<uint64_t> gates;
  CityPlanSettings settings;
};
struct CityLayoutGenerationBrief
{
  CityPolygon polygon;
  std::vector<std::vector<Position>> reservations;
  std::vector<CityStreet> arteries;
  std::vector<CityGate> gates;
  CitySettings settings;
  std::function<double(double, double)> terrain;
};
struct RuralGenerationBrief
{
  WorldSuitability suitability;
  std::vector<SurfaceContour> reservations;
  std::vector<RuralSource> sources;
  RuralPatchSettings settings;
};
struct TerritoryGenerationBrief
{
  CityTerritoryInput input;
  WorldSuitability suitability;
  CityTerritorySettings settings;
  std::function<double(double, double)> terrain;
};
GenerationTask cityMasterPlanGenerator(uint64_t domain);
GenerationTask cityLayoutGenerator(uint64_t domain);
GenerationTask ruralPatchesGenerator(uint64_t domain);
GenerationTask cityTerritoryGenerator(uint64_t domain);
GenerationTask settlementProgrammeGenerator(uint64_t domain);
GenerationTask runtimeWorldGenerator(uint64_t domain);

struct LandGenerationBrief
{
  WorldSpec spec;
  double spacing = 100;
  std::vector<Position> cores;
  double landFraction = .7, oceanMargin = 0;
};
struct CoastGenerationBrief
{
  LandProduct land;
  CoastDetailSettings settings;
};
struct RiverGenerationBrief
{
  LandProduct land;
  ReliefControls relief;
  std::vector<RiverMouth> mouths;
  RiverGrowthSettings settings;
};
struct WatershedGenerationBrief
{
  LandProduct land;
  RiverGrowthProduct rivers;
  ReliefControls relief;
  bool allowTruncated = false;
};
struct ChannelGenerationBrief
{
  RiverGrowthProduct rivers;
  WatershedProduct watersheds;
  double rainfall = 1, runoff = 1;
  ChannelSettings settings;
  bool allowTruncated = false;
};
struct RoadGenerationBrief
{
  std::vector<RoadAnchor> anchors;
  RoadField terrain, resistance;
  RoadSettings settings;
  uint64_t revision = 1;
};
struct EarthworkGenerationBrief
{
  RoadNetworkProduct roads;
  RoadField terrain;
  RoadWaterField water;
  std::vector<SurfaceContour> contours;
  std::vector<BridgeSpan> bridges;
  RoadEarthworkSettings settings;
};
struct CityDemandGenerationBrief
{
  std::vector<SettlementHousehold> households;
  CityDemandSettings settings;
};
GenerationTask landPartitionGenerator(uint64_t domain);
GenerationTask coastDetailGenerator(uint64_t domain);
GenerationTask riverNetworkGenerator(uint64_t domain);
GenerationTask watershedGenerator(uint64_t domain);
GenerationTask channelGenerator(uint64_t domain);
GenerationTask roadNetworkGenerator(uint64_t domain);
GenerationTask roadEarthworkGenerator(uint64_t domain);
GenerationTask cityDemandGenerator(uint64_t domain);
} // namespace procedural::world::rewrite
