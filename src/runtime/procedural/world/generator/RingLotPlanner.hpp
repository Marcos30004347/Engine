#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::world::generator {

// Ring lots: a convex block is copied inward by the lot depth (every edge
// moved in, corners on the miters, acute corners chamfered), the band between
// the outline and the copy is cut into lots of equal area per street edge
// (cuts at the same fraction of the outer and inner edge, so every lot of an
// edge has the same area), and the copy is the block's common court. A poor
// block keeps no court: a large court splits the block through the middle
// with an alley and both halves are rung again, a small one is cut into
// interior lots. Lots on an edge without a street stay open (gardens).
struct RingLotInput
{
  ConvexRing block; // Convex, CCW, already clear of the street ribbons.
  double depth = 14;
  double targetArea = 120;     // Every lot of an edge has the strip area / round(strip area / target).
  double minimumDepth = 5;     // The depth shrinks until the inner copy is valid; below this the block is not rung.
  double maximumMiter = 2.2;   // A corner whose miter is longer than this many depths is chamfered.
  std::vector<bool> fronted;   // Per outer edge: the edge has a street. Empty: every edge.
  bool fillInterior = false;   // No court: the interior is divided or cut into lots.
  double fillCourtBelow = 0;   // A court smaller than this is cut into interior lots instead of kept.
  double divideCourtAbove = 0; // With fillInterior: a court above this area splits the block with an alley.
  double alleyWidth = 2.5;
  uint32_t maximumSplits = 4;
  double axisRadians = 0;     // Interior lot cuts follow this axis and its normal.
  bool solidFallback = false; // A block too narrow for a ring is cut whole into lots of the target area (no fans).
  double minimumFrontage = 0; // Optional dwelling-grid admission width.
  uint64_t key = 1;
  std::function<double(uint64_t, uint64_t)> random;
};

struct RingLotPlan
{
  std::vector<std::vector<Position>> alleys;
  std::vector<ConvexRing> lots;
  std::vector<ConvexRing> open;   // Lots on edges without a street.
  std::vector<ConvexRing> courts; // Common ground kept in the middle.
  uint32_t splits = 0, chamfers = 0;
  double depthUsed = 0;
};

class RingLotPlanner final : public Generator<RingLotInput, RingLotPlan>
{
public:
  explicit RingLotPlanner(uint64_t domain);

  static bool plan(const RingLotInput &input, RingLotPlan &output, Ledger &ledger, std::string *error);

protected:
  bool run(const RingLotInput &input, RingLotPlan &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
