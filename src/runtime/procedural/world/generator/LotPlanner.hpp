#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::world::generator {

// Lots after Watabou's Ward.createAlleys and Cutter.bisect. A block is cut
// through a point at `ratio` along its longest edge, perpendicular to that
// edge turned by a small angle, leaving an alley gap between the halves; each
// half smaller than the minimum lot area (scaled by the size chaos) becomes a
// lot or, with the empty probability, stays open, otherwise it is cut again.
// A half keeps splitting with an alley while it is large; small halves are
// cut without a gap so buildings touch. The block itself is first inset by
// its street clearance (getCityBlock).
struct LotPlanInput
{
  ConvexRing block;              // The block outline, CCW; concave outlines are handled piece by piece by the caller.
  double inset = 0;              // Street clearance taken from every edge (getCityBlock).
  double minimumArea = 60;       // minSq: lots stop splitting below this, scaled by the size chaos.
  double gridChaos = .5;         // Ratio spread and angle jitter of the cuts (0 = regular).
  double sizeChaos = .6;         // Spread of lot sizes: 2^(4 * sizeChaos * (rnd - 0.5)).
  double emptyProbability = .04; // A finished lot stays open with this probability.
  double alleyWidth = 1.2;       // Gap between halves while the halves are large.
  bool orthogonal = true;        // Watabou's createOrthoBuilding: every cut is parallel to one of two axes set by the block's longest edge, so no lot fans; a cut that would leave a sliver is not made.
  bool axisGiven = false;        // Use axisRadians for the orthogonal cuts (one axis pair for every piece of a block).
  double axisRadians = 0;
  uint32_t maximumDepth = 12;
  uint64_t key = 1;                                 // Deterministic seed of this block.
  std::function<double(uint64_t, uint64_t)> random; // Unit draws by key and purpose.
};

struct LotPlan
{
  std::vector<ConvexRing> lots;
  std::vector<ConvexRing> open; // Lots left empty by the empty probability.
  uint32_t cuts = 0, alleys = 0;
};

class LotPlanner final : public Generator<LotPlanInput, LotPlan>
{
public:
  explicit LotPlanner(uint64_t domain);

  static bool plan(const LotPlanInput &input, LotPlan &output, Ledger &ledger, std::string *error);

protected:
  bool run(const LotPlanInput &input, LotPlan &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
