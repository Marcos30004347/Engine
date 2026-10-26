#pragma once

#include "Generator.hpp"
#include "RoadGrowth.hpp"
#include "procedural/world/WorldPipeline.hpp"
#include "procedural/world/WorldSettlementSiteAdapter.hpp"

#include <cstdint>
#include <memory>

namespace procedural::world::generator {

// World-scale roads: the Parish-Muller road growth run over the whole map.
// Legal land is every land leaf of the map graph, the heightmap is the map
// surface, and the population density is the sum of the kingdom's settlements
// (each a Gaussian of its population). Highways leave every settlement toward
// its kingdom links and follow the density fan, so they bend toward the
// towns; the Elevation pattern keeps them off the steep ground. Streets
// (country lanes) branch off with a delay near the settlements.
struct WorldRoadsSettings
{
  double highwayLength = 420, laneLength = 260;
  double intersectionSnapMetres = 160, roadSnapMetres = 110;
  double highwayBranchProbability = .08, laneBranchProbability = .28;
  int laneDelay = 3;
  uint32_t maximumSegments = 700;
  double densitySigmaMetres = 1600; // Reach of a settlement's pull on the highways.
  double sampleStepMetres = 60;     // Land test spacing along a segment.
};

struct WorldRoadsInput
{
  std::shared_ptr<const GeneratedWorld> world;
  rewrite::KingdomPlan kingdom;
  WorldRoadsSettings settings;
};

class WorldRoads final : public Generator<WorldRoadsInput, RoadNetwork>
{
public:
  explicit WorldRoads(uint64_t domain);

  static bool grow(const GeneratedWorld &world, const rewrite::KingdomPlan &kingdom, const WorldRoadsSettings &settings, RoadNetwork &output, Ledger &ledger, std::string *error);

protected:
  bool run(const WorldRoadsInput &input, RoadNetwork &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
