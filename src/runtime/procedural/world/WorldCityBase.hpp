#pragma once
#include "WorldCityCompletion.hpp"
#include "WorldCityRelief.hpp"

namespace procedural::world::rewrite {
enum class CityBaseTerrain { Hillside, Ridge, Valley };
enum class CityBaseLayout { Tapered, Staggered, Broad, Voronoi, Districts };
struct CityBaseSettings {
  uint64_t seed=41;
  uint32_t tiers=4;
  double width=84, tierDepth=36, tierRise=5.4;
  double lotFrontage=9, alleyWidth=3;
  double laneMeanderMetres=.65; // District lanes bend within reserved public corridors.
  CityBaseTerrain terrain=CityBaseTerrain::Hillside;
  CityBaseLayout layout=CityBaseLayout::Tapered;
};
struct CityBasePlan {
  CityBaseSettings settings;
  CityCompletionPlan circulation; // Blocks and public ways only; no occupied lots.
  CityReliefPlan relief;
  std::vector<CitySkeletonRegion> buildable; // Reserved envelopes after circulation clearance.
  // Planar Voronoi cells before stair courts are cut. The first `tiers` sites
  // own the terraces; the remaining sites shape the surrounding unbuilt land.
  std::vector<Position> landSites;
  std::vector<std::vector<Position>> landCells;
};
// A compact curved street with shared lot seams and inward public frontage.
bool planCityStreetQuarter(uint64_t seed, CityCompletionPlan &, std::string *error=nullptr);
// Terrain/rock masses, decks and connected pedestrian routes precede lotting.
// Both phases are transactional and deterministic for the same settings.
bool planCityBase(const CityBaseSettings &, CityBasePlan &, std::string *error=nullptr);
bool populateCityBase(const CityBasePlan &, CityCompletionPlan &, CityReliefPlan &, std::string *error=nullptr);
}
