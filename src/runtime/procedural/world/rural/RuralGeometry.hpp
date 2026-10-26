#pragma once

#include "procedural/world/WorldSuitability.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace procedural::world::rural {

using rewrite::Position;
using rewrite::WorldSuitability;

using Poly = std::vector<Position>;

// The geometry the rural allocator is written in.
//
// These were file-local to WorldRuralPatches.cpp, which meant the growth
// strategies could not leave that file without taking a copy of them. Moved
// verbatim, not re-expressed through geom2: geom2 has its own tolerances, and
// changing which cells a farm claims belongs in its own change.

// Twice the signed area of the triangle (a, b, c).
inline double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

// Signed area of a ring by the shoelace sum.
inline double area(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }
  return a * .5;
}

// The square an analysis cell occupies, as a ring.
inline Poly cellPoly(const WorldSuitability &w, uint32_t i)
{
  auto p = w.cells()[i].center;
  double h = w.settings().spacingMetres * .5;
  return {{p.x - h, 0, p.z - h}, {p.x + h, 0, p.z - h}, {p.x + h, 0, p.z + h}, {p.x - h, 0, p.z + h}};
}

// The outline of a set of analysis cells, as one ring.
//
// Returns false when the cells do not form a single simply connected blob,
// which is how a farm proves it is one holding rather than two that happen to
// share a source.
inline bool outline(const WorldSuitability &w, const std::vector<uint32_t> &cells, Poly &out)
{
  std::set<uint32_t> owned(cells.begin(), cells.end());
  std::map<uint64_t, uint64_t> edges;
  uint64_t stride = w.columns() + 1;
  for (auto i : cells)
  {
    uint64_t x = i % w.columns(), z = i / w.columns();
    uint64_t v[] = {z * stride + x, z * stride + x + 1, (z + 1) * stride + x + 1, (z + 1) * stride + x};
    bool exposed[] = {z == 0 || !owned.count(i - w.columns()), x + 1 == w.columns() || !owned.count(i + 1), z + 1 == w.rows() || !owned.count(i + w.columns()), x == 0 || !owned.count(i - 1)};
    for (int k = 0; k < 4; ++k)
    {
      if (exposed[k] && !edges.emplace(v[k], v[(k + 1) % 4]).second)
      {
        return false;
      }
    }
  }
  if (edges.empty())
  {
    return false;
  }
  auto start = edges.begin()->first, v = start;
  double h = w.settings().spacingMetres;
  do
  {
    auto it = edges.find(v);
    if (it == edges.end())
    {
      return false;
    }
    out.push_back({w.settings().minX + (v % stride) * h, 0, w.settings().minZ + (v / stride) * h});
    v = it->second;
    edges.erase(it);
  } while (v != start);
  if (!edges.empty())
  {
    return false; // No holes or disconnected fallback polygons.
  }
  for (int pass = 0; pass < 2; ++pass)
  {
    Poly next;
    for (size_t i = 0; i < out.size(); ++i)
    {
      auto a = out[(i + out.size() - 1) % out.size()], b = out[i], c = out[(i + 1) % out.size()];
      double turn = cross(a, b, c);
      if (std::abs(turn) < 1e-8)
      {
        continue;
      }
      if (turn > 0)
      {
        double ta = std::min(.2, h * .2 / std::hypot(a.x - b.x, a.z - b.z));
        double tc = std::min(.2, h * .2 / std::hypot(c.x - b.x, c.z - b.z));
        next.push_back({b.x * (1 - ta) + a.x * ta, 0, b.z * (1 - ta) + a.z * ta});
        next.push_back({b.x * (1 - tc) + c.x * tc, 0, b.z * (1 - tc) + c.z * tc});
      }
      else
      {
        next.push_back(b);
      }
    }
    out = std::move(next);
  }
  return out.size() >= 3 && area(out) > 0;
}

} // namespace procedural::world::rural
