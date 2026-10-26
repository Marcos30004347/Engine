#pragma once
#include "WorldRiverGraph.hpp"

namespace procedural::world::rewrite {
struct WatershedCell
{
  uint32_t riverNode = 0;
  double areaMetres2 = 0;
  // Convex fragments, clipped to triangulated physical land; internal fragment
  // edges are clipping artifacts and never become crest constraints.
  std::vector<std::vector<Position>> fragments;
};

struct CrestVertex
{
  Position position;
  bool coastal = false;
  std::vector<uint32_t> equidistantSites;
};

struct WatershedEdge
{
  uint32_t a = 0, b = 0;
  uint32_t leftCell = 0, rightCell = 0;
  bool riverCrossing = false;
};

struct WatershedProduct
{
  std::vector<WatershedCell> cells;
  std::vector<CrestVertex> vertices;
  std::vector<WatershedEdge> edges;
  double landAreaMetres2 = 0;
  bool sourceGraphTruncated = false;
};

// Bounded reference construction (at most 4096 river sites): half-plane Voronoi,
// then convex intersection with land triangles. Does not infer drainage coverage
// from Voronoi coverage. Truncated input requires an explicit opt-in.
bool buildWatersheds(const LandProduct &, const RiverGrowthProduct &, const ReliefControls &, bool allowTruncated, WatershedProduct &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
