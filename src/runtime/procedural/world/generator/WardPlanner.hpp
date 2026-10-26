#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::world::generator {

// Ward planner: the coarse patches of a settlement and their refinement into
// blocks. Watabou's wards are relaxed Voronoi patches whose shared edges are
// the streets; a planned ward is gridded along a district axis instead
// (Might of Merchants mixes both). The planner only produces convex cells;
// the layout clips them to its usable land and derives streets from the
// shared edges.
enum class WardRecipe : uint8_t
{
  Organic,   // Poisson sites, Voronoi, Lloyd relaxation, organic bisection.
  Recursive, // One box, recursive bisection.
  Planned,   // One box on the planned axis, gridded bisection.
  Roads,     // Roads come from another generator; wards are the usable pieces.
  Given      // The wards are given (the settlement's planning cells): Watabou's patches are the blocks.
};

struct PlannedCut
{
  double nx = 0, nz = 0, offset = 0;
  uint32_t depth = 0;
};

struct WardPlanInput
{
  WardRecipe recipe = WardRecipe::Organic;
  ConvexRing domain;
  std::vector<ConvexRing> usable;
  std::vector<ConvexRing> givenCells; // Recipe Given: these convex cells are the wards, uncut.
  double minX = 0, maxX = 0, minZ = 0, maxZ = 0;
  double siteSpacing = 28, targetBlockArea = 950, minimumBlockArea = 120;
  double plannedBlockAspect = 1.5, plannedAxisRadians = 0, plannedDistrictTurnRadians = 0;
  double frontageInset = 6, minimumWidth = 4; // A block must keep an interior court after both frontages.
  uint32_t maximumSites = 64, maximumSplitDepth = 10, siteRelaxations = 0;
  uint32_t maximumSides = 0;    // > 0: an organic block with more sides is cut; the cut with the best halves wins.
  double maximumBlockDepth = 0; // > 0: an organic block wider than this across its narrowest direction is cut (two lot rows).
  bool mixedBlockGrammar = false;
  std::function<double(uint64_t, uint64_t)> random;  // Deterministic unit draws by key and purpose.
  std::function<uint64_t(uint64_t)> wardSeed;        // Seed for the bisection of coarse ward i.
  std::function<bool(const ConvexRing &)> plannedAt; // A coarse ward that is gridded (mixed grammar).
};

struct WardPlan
{
  std::vector<Position> sites;
  std::vector<ConvexRing> cells;
  std::vector<PlannedCut> plannedCuts;
  uint32_t splitAttempts = 0, unrefinedBlocks = 0;
  bool anyPlanned = false;
};

class WardPlanner final : public Generator<WardPlanInput, WardPlan>
{
public:
  explicit WardPlanner(uint64_t domain);

  static bool plan(const WardPlanInput &input, WardPlan &output, Ledger &ledger, std::string *error);

protected:
  bool run(const WardPlanInput &input, WardPlan &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
