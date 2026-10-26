#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace procedural::world::generator {

// Curtain wall after Watabou's CurtainWall.hx. The circumference of the
// walled core is smoothed by pulling every vertex toward its two neighbours,
// v' = (prev + f v + next) / (2 + f), with f = min(1, 40 / cells). Watabou's
// Polygon.set moves the shared Point, so the cells beside the wall deform
// with it; here the same vertex move is reported for every polygon that
// carries the vertex. A vertex on a pinned ring (the urban leaf boundary the
// farms already use) stays, and a move that would leave a cell non-convex is
// halved up to three times, then dropped. The smoothed outline is finally
// resampled into a ring of same-sized straight sections, which is the wall
// and the boundary of the layout.
struct CurtainWallInput
{
  ConvexRing outline;                  // Circumference of the core cells (CCW).
  std::vector<ConvexRing> cells;       // Every planning cell that may share a vertex.
  std::vector<ConvexRing> pinnedRings; // Rings whose vertices must not move.
  size_t coreCells = 1;                // Sets the smoothing factor.
  double sectionMetres = 75;           // Wall section length between towers.
  unsigned minimumSections = 8;
};

struct CurtainWallPlan
{
  ConvexRing ring;                                  // The wall, CCW.
  std::vector<std::pair<Position, Position>> moves; // Vertex moves (from, to) to apply to shared polygons.
  size_t smoothed = 0, pinned = 0, halvings = 0;
  double smoothing = 1;
};

class CurtainWall final : public Generator<CurtainWallInput, CurtainWallPlan>
{
public:
  explicit CurtainWall(uint64_t domain);

  static bool plan(const CurtainWallInput &input, CurtainWallPlan &output, Ledger &ledger, std::string *error);

  // Apply the plan's moves to any polygon that shares a moved vertex.
  static void remap(const CurtainWallPlan &plan, ConvexRing &polygon);

protected:
  bool run(const CurtainWallInput &input, CurtainWallPlan &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
