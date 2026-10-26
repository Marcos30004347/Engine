#pragma once

#include "procedural/geom2/QuadMesh.hpp"

#include <functional>
#include <limits>

namespace procedural::world::citywfc {

// A* over the cells of a quad grid.
//
// One router for every way the city is threaded: arteries between gates, streets
// from a district to an artery, alleys splitting an oversized block. They differ
// only in what a step costs, so the cost is a callback and nothing else here
// knows what a road is.
//
// Two terms in that cost do the work that separates a road network from a set of
// shortest paths, and both live in the caller's cost function rather than here:
//
//   reuse   - a cell that is already road costs a fraction of a fresh one, so
//             the second route prefers the first route's cells and the paths
//             bundle and fork instead of running parallel one cell apart.
//   turning - a penalty on the angle between successive steps, which is what
//             keeps a route straight without smoothing it afterwards.

inline constexpr double kBlocked = std::numeric_limits<double>::infinity();

// The cost of stepping from `from` to `to`, having arrived at `from` from
// `previous` (kNoFace at the start). Return kBlocked to forbid the step.
using StepCost = std::function<double(uint32_t previous, uint32_t from, uint32_t to)>;

struct RouteSettings
{
  uint32_t maximumExpansions = 2000000;

  // The most a step can be discounted below its straight-line length. The
  // heuristic is scaled by this so it stays admissible: with a reuse discount
  // of 0.3, a remaining distance of d can cost as little as 0.3 d, and a
  // heuristic of d would overestimate and cut off the cheap bundled route that
  // reuse exists to find.
  double lowestCostFactor = .3;
};

struct RouteReport
{
  uint32_t expansions = 0;
  double cost = 0;
  bool reachedGoal = false;
};

// The cheapest chain of cells from `from` to `to`, inclusive of both.
//
// False when the goal is unreachable or the expansion budget runs out; `report`
// says which, and `path` is left empty.
bool routeCells(const geom2::QuadMesh &mesh,
                const geom2::FaceAdjacency &adjacency,
                uint32_t from,
                uint32_t to,
                const StepCost &cost,
                const RouteSettings &settings,
                std::vector<uint32_t> &path,
                RouteReport &report);

} // namespace procedural::world::citywfc
