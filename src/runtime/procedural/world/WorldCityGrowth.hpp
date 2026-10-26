#pragma once

#include "WorldCityCastle.hpp"

namespace procedural::world::rewrite {

enum class CityWallLifecycle : uint8_t
{
  Absorbed,
  Standing,
  Planned
};

enum class CityGrowthZoneKind : uint8_t
{
  Suburb,
  Castle
};

struct CityGrowthOpening
{
  size_t edge = 0;
  double from = 0, to = 0; // Normalized stations on a boundary edge.
};

struct CityGrowthWall
{
  uint64_t id = 0, era = 0;
  CityWallLifecycle lifecycle = CityWallLifecycle::Standing;
  std::vector<Position> boundary;
  std::vector<Position> gates, towers;
  double perimeter = 0, enclosedArea = 0;
  bool palisade = false;
  std::vector<double> gateWidths;
  std::vector<CityGrowthOpening> openings;
  std::vector<bool> gatehouseFits;
};

struct CityGrowthZone
{
  uint64_t id = 0, source = 0;
  CityGrowthZoneKind kind = CityGrowthZoneKind::Suburb;
  std::vector<Position> boundary;
  bool insideStandingWall = false, insidePlannedWall = false;
};

struct CityGrowthEpoch
{
  uint32_t builtWeek = 0;
  uint32_t buildings = 0;
  double occupiedArea = 0;
  std::vector<uint64_t> blocks;
};

struct CityGrowthPlan
{
  uint64_t revision = 0;
  // Only physically retained circuits belong here. History-backed plans can
  // retain inner walls as well as the latest curtain; unbuilt works are traces.
  std::vector<CityGrowthWall> walls;
  std::vector<CityGrowthWall> traces;
  std::vector<CityGrowthZone> zones;
  bool historical = false;
  std::vector<CityGrowthEpoch> epochs;
  std::vector<std::string> omissions;
};

struct CityGrowthSettings
{
  double oldCoreFraction = .32;
  double minimumOuterGrowth = .03;
  double towerSpacing = 45;
};

// Composes one physical city wall plus chronological growth traces. The
// absorbed core and planned envelope never become defensive structures here;
// the castle publishes its own enclosure in the completion pass.
bool planCityGrowth(
    const SettlementFabric &,
    const CitySkeleton &,
    const CityCastlePlan &,
    const CityGrowthSettings &,
    CityGrowthPlan &,
    std::string *error = nullptr);

const char *cityWallLifecycleName(CityWallLifecycle);
const char *cityGrowthZoneKindName(CityGrowthZoneKind);

} // namespace procedural::world::rewrite
