#pragma once
#include "WorldCityLayout.hpp"
#include "WorldFarmFields.hpp"
#include "WorldFinalPatches.hpp"
#include "WorldRuralPatches.hpp"

namespace procedural::world::rewrite {
enum class FarmBuildingUse : uint8_t
{
  Farmhouse,
  Barn,
  WorkerCottage
};

struct FarmBuildingSite
{
  uint64_t id = 0;
  FarmBuildingUse use = FarmBuildingUse::Farmhouse;
  std::vector<Position> footprint;
  Position entrance;
  double foundation = 0, height = 0;
};

struct FarmHolding
{
  uint64_t patch = 0, source = 0;
  std::vector<Position> yard, access;
  std::vector<FarmBuildingSite> buildings;
  double yardArea = 0;
};

struct CityTerritoryInput
{
  uint64_t producer = 0, revision = 1;
  // Frozen, convex reservations include the whole accepted city, suburbs,
  // protected nature and immutable regional roads. Sources are connected road exits.
  std::vector<SurfaceContour> occupied;
  std::vector<RuralSource> ruralSources;
  // Coarse director cells can span a much larger area than the wall envelope.
  // They carry terrain/political eligibility; circulation cells are nonallocatable.
  std::vector<CityPlanCell> cells;
  std::vector<DistrictRequest> districts;
  std::vector<uint64_t> gateCells;
};

struct CityTerritorySettings
{
  CityPlanSettings planning;
  RuralPatchSettings rural;
  FarmFieldSettings fields;
  double maximumFoundationRelief = 1.5;
  bool farmsteads = true;
  uint32_t farmsteadInterval = 1; // One yard per N selected agricultural regions.
};

struct CityTerritory
{
  uint64_t revision = 0, suitabilityFingerprint = 0;
  CityMasterPlan regions; // Multi-cell urban/estate/military allocations; internal seams are not roads.
  RuralPatchProduct rural;
  std::vector<FarmFieldProduct> fields; // Same index order as rural.patches.
  std::vector<FarmHolding> holdings;
  std::vector<std::string> rejections;
  WorldContourBatch surfaces; // Native world-builder input; buildings remain separate instances.
  double cropArea = 0, farmyardArea = 0;
};

// Allocates outer districts first, then terrain-driven farmland around them.
// Planning access is not an engineered urban road. Farm access follows the rural
// generator's validated tracks. No world geometry is regenerated or silently resized.
// Use multiple bounded territory jobs for huge worlds; final builder budgets apply.
// Atomic output, purpose IDs scoped by producer, world-metre coordinates.
bool generateCityTerritory(const CityTerritoryInput &, const WorldSuitability &, const CityTerritorySettings &, const std::function<double(double, double)> &terrain, CityTerritory &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
