#include "procedural/world/citywfc/CityRouter.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

namespace procedural::world::citywfc {
namespace {

using geom2::kNoFace;
using geom2::toMetres;

double distanceBetween(const geom2::QuadMesh &mesh, const uint32_t a, const uint32_t b)
{
  const geom2::Point pa = mesh.centroid(a);
  const geom2::Point pb = mesh.centroid(b);

  const double dx = toMetres(pb.x - pa.x), dy = toMetres(pb.y - pa.y);

  return std::sqrt(dx * dx + dy * dy);
}

// A state is a cell reached from a particular predecessor, because the cost of
// leaving it depends on how it was entered: a turn penalty is a property of the
// pair, not of the cell. Keyed on the arriving edge rather than on the cell, so
// "straight through here" and "a right angle here" are different states and the
// search cannot quietly take the cheaper arrival and the cheaper departure.
struct State
{
  uint32_t cell = kNoFace;
  uint32_t from = kNoFace;

  bool operator<(const State &other) const
  {
    return cell != other.cell ? cell < other.cell : from < other.from;
  }
};

struct Open
{
  double estimate = 0;
  double spent = 0;
  State state;

  // Ties broken on the state, not on insertion order, so the result does not
  // depend on how the heap happened to reorder equal keys.
  bool operator>(const Open &other) const
  {
    if (estimate != other.estimate)
    {
      return estimate > other.estimate;
    }

    return other.state < state;
  }
};

} // namespace

bool routeCells(const geom2::QuadMesh &mesh,
                const geom2::FaceAdjacency &adjacency,
                const uint32_t from,
                const uint32_t to,
                const StepCost &cost,
                const RouteSettings &settings,
                std::vector<uint32_t> &path,
                RouteReport &report)
{
  path.clear();
  report = {};

  if (from >= mesh.faces() || to >= mesh.faces())
  {
    return false;
  }

  if (from == to)
  {
    path.push_back(from);
    report.reachedGoal = true;

    return true;
  }

  std::map<State, double> best;
  std::map<State, State> cameFrom;
  std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;

  const auto heuristic = [&](const uint32_t cell) {
    return settings.lowestCostFactor * distanceBetween(mesh, cell, to);
  };

  const State start{from, kNoFace};
  best[start] = 0;
  open.push({heuristic(from), 0, start});

  while (!open.empty())
  {
    const Open here = open.top();
    open.pop();

    const auto known = best.find(here.state);

    if (known == best.end() || here.spent > known->second)
    {
      continue;
    }

    if (here.state.cell == to)
    {
      report.reachedGoal = true;
      report.cost = here.spent;

      State walk = here.state;

      while (walk.cell != kNoFace)
      {
        path.push_back(walk.cell);

        const auto previous = cameFrom.find(walk);

        if (previous == cameFrom.end())
        {
          break;
        }

        walk = previous->second;
      }

      std::reverse(path.begin(), path.end());

      return true;
    }

    if (++report.expansions > settings.maximumExpansions)
    {
      return false;
    }

    for (uint32_t k = adjacency.offset[here.state.cell]; k < adjacency.offset[here.state.cell + 1]; ++k)
    {
      const uint32_t next = adjacency.neighbour[k];
      const double step = cost(here.state.from, here.state.cell, next);

      if (!(step < kBlocked))
      {
        continue;
      }

      const State moved{next, here.state.cell};
      const double spent = here.spent + step;
      const auto found = best.find(moved);

      if (found != best.end() && found->second <= spent)
      {
        continue;
      }

      best[moved] = spent;
      cameFrom[moved] = here.state;
      open.push({spent + heuristic(next), spent, moved});
    }
  }

  return false;
}

} // namespace procedural::world::citywfc
