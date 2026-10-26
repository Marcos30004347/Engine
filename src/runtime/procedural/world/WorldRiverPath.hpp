#pragma once
#include "WorldRiverGraph.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::world::rewrite {
struct RiverPath
{
  std::vector<Position> points;
  std::vector<double> distances;

  double length() const
  {
    return distances.empty() ? 0 : distances.back();
  }

  Position at(double distance) const
  {
    distance = std::clamp(distance, 0., length());
    auto it = std::upper_bound(distances.begin(), distances.end(), distance);
    size_t j = std::clamp(size_t(it - distances.begin()), size_t(1), points.size() - 1);
    double t = (distance - distances[j - 1]) / (distances[j] - distances[j - 1]);
    auto a = points[j - 1], b = points[j];
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
  }

  double project(double x, double z, double &station) const
  {
    double best = 1e30;
    for (size_t j = 1; j < points.size(); ++j)
    {
      auto a = points[j - 1], b = points[j];
      double dx = b.x - a.x, dz = b.z - a.z;
      double t = std::clamp(((x - a.x) * dx + (z - a.z) * dz) / (dx * dx + dz * dz), 0., 1.);
      double d = std::hypot(x - a.x - t * dx, z - a.z - t * dz);
      if (d < best)
      {
        best = d;
        station = distances[j - 1] + t * (distances[j] - distances[j - 1]);
      }
    }
    return best;
  }
};

inline bool makeRiverPath(std::vector<Position> points, Position a, Position b, RiverPath &out)
{
  if (points.size() < 2 || points.size() > 8192)
  {
    return false;
  }
  auto equal = [](Position p, Position q) {
    return p.x == q.x && p.y == q.y && p.z == q.z;
  };
  if (!equal(points.front(), a) || !equal(points.back(), b))
  {
    return false;
  }
  RiverPath r;
  r.points = std::move(points);
  r.distances.push_back(0);
  for (size_t i = 0; i < r.points.size(); ++i)
  {
    auto p = r.points[i];
    for (double v : {p.x, p.y, p.z})
    {
      if (!std::isfinite(v) || std::abs(v) >= 1e12)
      {
        return false;
      }
    }
    if (i)
    {
      auto q = r.points[i - 1];
      double d = std::hypot(p.x - q.x, p.z - q.z);
      if (d < 1e-6 || p.y > q.y + 1e-10 || (p.x - q.x) * (b.x - a.x) + (p.z - q.z) * (b.z - a.z) <= 0)
      {
        return false;
      }
      r.distances.push_back(r.distances.back() + d);
    }
  }
  if (r.length() > 1e6)
  {
    return false;
  }
  // Water falls linearly in true arc length. Consumers size one hydraulic reach.
  for (size_t i = 0; i < r.points.size(); ++i)
  {
    if (std::abs(r.points[i].y - (a.y + (b.y - a.y) * r.distances[i] / r.length())) > 1e-7)
    {
      return false;
    }
  }
  out = std::move(r);
  return true;
}

inline bool resolveRiverPaths(const RiverGrowthProduct &g, std::vector<RiverPath> &out)
{
  if (!g.reachPaths.empty() && g.reachPaths.size() != g.nodes.size())
  {
    return false;
  }
  std::vector<RiverPath> r(g.nodes.size());
  size_t total = 0;
  for (size_t i = 0; i < g.nodes.size(); ++i)
  {
    auto n = g.nodes[i];
    if (n.receiver == UINT32_MAX)
    {
      if (!g.reachPaths.empty() && !g.reachPaths[i].empty())
      {
        return false;
      }
      continue;
    }
    if (n.receiver >= i)
    {
      return false;
    }
    auto p = g.reachPaths.empty() ? std::vector<Position>{n.position, g.nodes[n.receiver].position} : g.reachPaths[i];
    total += p.size();
    if (total > 131072 || !makeRiverPath(std::move(p), n.position, g.nodes[n.receiver].position, r[i]))
    {
      return false;
    }
  }
  out = std::move(r);
  return true;
}
} // namespace procedural::world::rewrite
