#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"

#include <cstdint>
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

// Streets after Watabou's Topology and Model.buildStreets: a graph of every
// patch vertex, the wall vertices blocked except the gates; every gate gets a
// street to the nearest plaza vertex over inner vertices only, and a road
// outward over outer vertices only; the segments are deduplicated (those
// along the plaza dropped) and chained into arteries (tidyUpRoads), which
// are then smoothed, moving the shared patch vertices with them.
struct TownStreetsInput
{
  std::vector<TownPatch> patches;
  ConvexRing border;           // The wall ring (walled) or the core circumference; its vertices block paths except at the gates.
  std::vector<Position> gates; // Snapped to the nearest border vertex.
  std::vector<Position> exits; // Per gate (optional): where its road heads; otherwise outward from the centre.
  Position centre;
  double smoothing = 3; // Polygon.smoothVertexEq factor for the arteries (0: none).
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
