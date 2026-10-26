#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::world::generator {

// Watabou's ward geometry (TownGeneratorOS Ward.hx), reimplemented: the
// patch is inset per edge into its city block (getCityBlock), the block is
// bisected at its longest edge again and again into buildings with alleys
// between the larger groups (createAlleys / Cutter.bisect / Polygon.cut with
// a gap), and an outskirts ward keeps only the buildings near its roads, its
// gates and the city (filterOutskirts). Units are metres; Watabou's unit is
// taken as 2.5 m, so every ward's minSq is scaled by 6.25 by the caller.
struct WardEdgePopulation
{
  Position a, b;
  double factor = 1; // 1 on a road or beside an enclosed city ward, .4 beside an open city ward.
};

struct WardBuildingsInput
{
  ConvexRing patch;              // Convex, CCW.
  std::vector<double> edgeInset; // Per edge of `patch` (empty: none): MAIN/2 on walls, plaza and arteries, REGULAR/2 inside, ALLEY/2 outside.
  double minSq = 60;             // CommonWard minSq (scaled to m2).
  double gridChaos = .6;
  double sizeChaos = .6;
  double emptyProb = .04;
  double alley = 1.5; // Ward.ALLEY scaled: the gap between the larger building groups.
  // A building must hold at least a small house: pieces below the area,
  // narrower than the width across any edge normal, or with a corner sharper
  // than the angle become yards (open), never buildings.
  double minimumBuildingArea = 30;
  double minimumBuildingWidth = 3.5;
  double minimumCornerRadians = .52;
  bool outskirts = false;
  std::vector<WardEdgePopulation> populated; // filterOutskirts: the edges buildings gather along.
  std::vector<double> vertexDensity;         // filterOutskirts: per vertex of `patch` (gate 1, city-only vertex 2 * random, else 0).
  uint64_t key = 1;
  std::function<double(uint64_t, uint64_t)> random; // Unit draws by key and purpose.
};

struct WardBuildingsPlan
{
  std::vector<ConvexRing> buildings;
  std::vector<ConvexRing> open; // Pieces too small or too sharp for a house: yards.
  ConvexRing block;
  uint32_t cuts = 0, dropped = 0, filtered = 0;
};

class WardBuildings final : public Generator<WardBuildingsInput, WardBuildingsPlan>
{
public:
  explicit WardBuildings(uint64_t domain);

  static bool plan(const WardBuildingsInput &input, WardBuildingsPlan &output, Ledger &ledger, std::string *error);

  // Ward.createOrthoBuilding: slices along the longest edge's axis and its
  // normal until the pieces are below minBlockSq, keeping each with `fill`.
  static std::vector<ConvexRing> orthoBuilding(const ConvexRing &poly, double minBlockSq, double fill, uint64_t key, const std::function<double(uint64_t, uint64_t)> &random);

protected:
  bool run(const WardBuildingsInput &input, WardBuildingsPlan &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
