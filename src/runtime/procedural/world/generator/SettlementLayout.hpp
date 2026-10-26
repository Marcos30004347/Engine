#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"
#include "procedural/world/WorldCityLayout.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::world::generator {

// The settlement, roads first (docs/architecture/settlement-generator.md):
// demand from the population; district-outline roads grown by the road
// L-system (RoadGrowth) into an edge graph (RoadGraph) whose faces are the
// districts; a use per face by heuristics against the remaining demand;
// each district cut into lots by scored chords (LotSplitter); each lot cut
// into buildings with alleys (WardBuildings). No wall. The result is the
// CityLayout the fabric, the director and the preview read.
struct SettlementLayoutInput
{
  ConvexRing region;             // Buildable land, a simple polygon (local frame).
  std::vector<ConvexRing> water; // Obstacles: lakes, river corridors, the sea.
  Position centre;
  std::vector<Position> exits; // Rural exits: main roads leave toward them.
  uint32_t population = 500;
  double householdSize = 4.5;
  double unit = 6.25;            // Watabou's square unit in m2.
  double blockTargetArea = 3200; // A district face; the road growth stops when the demand fits.
  double mainRoadHalfWidth = 2.5, minorRoadHalfWidth = 1.5, alley = 1.5;
  double mainRoadLength = 60, minorRoadLength = 34;
  double minimumFaceWidth = 22; // A proposed road closer than this to a parallel one is refused (no sliver faces).
  double marketArea = 1800;     // The most central face up to this area is the market square.
  double courtWardShare = .3;
  bool farmsteads = false;   // A village: lots are farmsteads (a house cluster per lot), not packed blocks.
  double deadEndReach = 1.6; // A dead end is extended up to this many minor road lengths to the first road it meets.
  uint64_t seed = 7, revision = 1;
  std::function<double(double, double)> terrain;
  std::function<double(uint64_t, uint64_t)> random;
};

class SettlementLayout final : public Generator<SettlementLayoutInput, rewrite::CityLayout>
{
public:
  explicit SettlementLayout(uint64_t domain);

  static bool plan(const SettlementLayoutInput &input, rewrite::CityLayout &output, Ledger &ledger, std::string *error);

protected:
  bool run(const SettlementLayoutInput &input, rewrite::CityLayout &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
