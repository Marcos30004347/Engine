#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace procedural::world::generator {

// A patch of the town: one planning cell. The walled core is `withinWalls`;
// the outskirts (gate wards) are city but not walled; everything else is
// countryside. The plaza is a patch whose edges carry no street segments.
struct TownPatch
{
  uint64_t id = 0;
  ConvexRing ring; // Convex, CCW; shared vertices coincide exactly.
  bool withinWalls = false;
  bool outskirts = false;
  bool plaza = false;
  uint8_t use = 7; // DistrictUse of the ward (Residential by default).
};

// Select gate/plaza and service routes on shared boundary and interior nodes.
// A* accounts for sampled terrain and reuses selected trunks; unselected cell
// edges remain planning boundaries rather than becoming streets. Shared
// segments are chained, then relaxed before the block and lot cuts.
struct TownStreetsInput
{
  std::vector<TownPatch> patches;
  ConvexRing border;           // The wall ring (walled) or the core circumference; its vertices block paths except at the gates.
  std::vector<Position> gates; // Snapped to the nearest border vertex.
  std::vector<Position> exits; // Per gate (optional): where its road heads; otherwise outward from the centre.
  Position centre;
  std::vector<Position> fixedVertices; // World planning boundaries must not drift during relaxation.

  // Where else a street starts. Watabou's network is gate-to-plaza and he has
  // eight to twelve gates; a settlement here has as many gates as it has rural
  // roads, which is two, so the core would hold two streets. An anchor is any
  // other point a street should run to the plaza from - a wall vertex between
  // two core patches (a postern the wall is not opened for), the corner of a
  // reserved compound - and its path is routed and smoothed exactly like a
  // gate's.
  std::vector<Position> anchors;
  double smoothing = 3; // Polygon.smoothVertexEq factor for the arteries (0: none).
  std::function<double(double, double)> terrain;
  std::vector<ConvexRing> obstacles; // Institutional compounds; roads may meet their boundary.
  double corridorHalfWidth = 2.5; // Check both shoulders, not just the centreline.
  double preferredGrade = .12, maximumGrade = .65;
  double maximumGateRelocation = 40; // Radius of the local boundary neighbourhood; zero pins gates.
  double existingStreetCost = .7; // Shared trunks are cheaper than a parallel new road.

};

struct TownStreetsPlan
{
  std::vector<std::vector<Position>> arteries;
  std::vector<Position> gates;                      // Snapped.
  std::vector<std::pair<Position, Position>> moves; // Original vertex -> smoothed vertex, to apply to the patches.
  uint32_t streets = 0, roads = 0, failed = 0, segments = 0;
};

class TownStreets final : public Generator<TownStreetsInput, TownStreetsPlan>
{
public:
  explicit TownStreets(uint64_t domain);

  static bool plan(const TownStreetsInput &input, TownStreetsPlan &output, Ledger &ledger, std::string *error);

  // Apply the plan's vertex moves to a polygon that shares moved vertices.
  static void remap(const TownStreetsPlan &plan, ConvexRing &polygon);

protected:
  bool run(const TownStreetsInput &input, TownStreetsPlan &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
