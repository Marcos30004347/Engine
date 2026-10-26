#pragma once

#include "procedural/world/WorldCityLayout.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace procedural::world::citylayout {

using rewrite::CityStreet;
using rewrite::CityWallTower;
using rewrite::Position;

using Poly = std::vector<Position>;

// The three geometric primitives the city layout is written in.
//
// They lived in WorldCityLayout.cpp's anonymous namespace, which meant the
// housing strategies could not be moved out of that file without taking a copy
// of them. A shared header is the alternative to a fourth copy of `dist`.

// Planar distance. The y coordinate is height and never participates: two
// points at the same place on the map are the same place, whatever the terrace
// does between them.
inline double dist(const Position a, const Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}

// Linear interpolation on the map plane, with the height dropped. Callers put
// the height back from whichever surface they belong to - the terrace, or the
// untreated ground - and mixing two heights here would silently pick one.
inline Position mix(const Position a, const Position b, const double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}

// The ground a street occupies: its centreline swept to its half width.
//
// This is the widthed-edge idea in its original form - a street is not a line
// through the city, it is a strip of ground nothing else may stand on.
inline Poly ribbon(const CityStreet &s)
{
  const double l = dist(s.a, s.b);
  const double x = -(s.b.z - s.a.z) / l * s.halfWidth;
  const double z = (s.b.x - s.a.x) / l * s.halfWidth;

  return {{s.a.x - x, 0, s.a.z - z}, {s.b.x - x, 0, s.b.z - z}, {s.b.x + x, 0, s.b.z + z}, {s.a.x + x, 0, s.a.z + z}};
}

// The square a wall tower keeps clear around itself, at the wall's setback.
// Axis aligned rather than fitted: the clearance is a rule about how close a
// building may stand, not a description of the tower's shape.
inline Poly towerClearance(const CityWallTower &tower, const double setback)
{
  double x0 = INFINITY, z0 = INFINITY, x1 = -INFINITY, z1 = -INFINITY;

  for (const auto p : tower.footprint)
  {
    x0 = std::min(x0, p.x);
    z0 = std::min(z0, p.z);
    x1 = std::max(x1, p.x);
    z1 = std::max(z1, p.z);
  }

  return {{x0 - setback, 0, z0 - setback}, {x1 + setback, 0, z0 - setback}, {x1 + setback, 0, z1 + setback}, {x0 - setback, 0, z1 + setback}};
}

// The planar predicates the layout is built on, moved here verbatim from
// WorldCityLayout.cpp's anonymous namespace rather than re-expressed through
// geom2. geom2 has its own tolerances and its own definition of validity;
// swapping them in would change which lots exist, and that belongs in its own
// change with its own evidence. A move is a move.
//
// Every tolerance below is the one the city layout has always used.

// Twice the signed area of the triangle (a, b, c): positive turning left.
inline double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

// Signed area of a ring by the shoelace sum. Positive counter-clockwise.
inline double area(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }
  return a * .5;
}

// Sutherland-Hodgman against one half plane, dropping the result when it
// collapses to a sliver. Convex in, convex out.
inline Poly clip(const Poly &p, double nx, double nz, double c)
{
  Poly r;
  if (p.empty())
  {
    return r;
  }
  for (size_t i = 0; i < p.size(); ++i)
  {
    auto a = p[i], b = p[(i + 1) % p.size()];
    double da = nx * a.x + nz * a.z - c, db = nx * b.x + nz * b.z - c;
    if (da <= 1e-8)
    {
      r.push_back(a);
    }
    if ((da < -1e-8 && db > 1e-8) || (db < -1e-8 && da > 1e-8))
    {
      r.push_back(mix(a, b, da / (da - db)));
    }
  }
  Poly q;
  for (auto v : r)
  {
    if (q.empty() || dist(q.back(), v) > 1e-7)
    {
      q.push_back(v);
    }
  }
  if (q.size() > 1 && dist(q.front(), q.back()) < 1e-7)
  {
    q.pop_back();
  }
  if (q.size() < 3 || area(q) < 1e-7)
  {
    q.clear();
  }
  return q;
}

// Whether q is inside a convex ring, boundary included within the tolerance.
inline bool inside(const Poly &p, Position q)
{
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (cross(p[i], p[(i + 1) % p.size()], q) < -1e-6)
    {
      return false;
    }
  }
  return !p.empty();
}

// Convex intersection, by clipping p against every edge of q in turn.
inline Poly intersection(Poly p, const Poly &q)
{
  for (size_t i = 0; i < q.size(); ++i)
  {
    auto a = q[i], b = q[(i + 1) % q.size()];
    p = clip(p, b.z - a.z, a.x - b.x, (b.z - a.z) * a.x + (a.x - b.x) * a.z);
  }
  return p;
}

// Whether the segment ab lies entirely within the union of convex pieces.
// Each piece contributes the interval of ab it covers; the segment is covered
// when those intervals join up without a gap.
inline bool coveredSegment(Position a, Position b, const std::vector<Poly> &pieces)
{
  std::vector<std::pair<double, double>> intervals;
  for (const auto &p : pieces)
  {
    double lo = 0, hi = 1;
    for (size_t k = 0; k < p.size(); ++k)
    {
      double da = cross(p[k], p[(k + 1) % p.size()], a), db = cross(p[k], p[(k + 1) % p.size()], b);
      if (da < -1e-8 && db < -1e-8)
      {
        hi = -1;
        break;
      }
      if (da < -1e-8)
      {
        lo = std::max(lo, da / (da - db));
      }
      if (db < -1e-8)
      {
        hi = std::min(hi, da / (da - db));
      }
    }
    if (hi >= lo)
    {
      intervals.emplace_back(lo, hi);
    }
  }
  std::sort(intervals.begin(), intervals.end());
  double end = 0;
  for (auto [lo, hi] : intervals)
  {
    if (lo > end + 1e-8)
    {
      break;
    }
    end = std::max(end, hi);
  }
  return end >= 1 - 1e-8;
}

// Distance from p to the segment ab, on the map plane.
inline double segmentDistance(Position p, Position a, Position b)
{
  double d = dist(a, b);
  if (d < 1e-9)
  {
    return dist(p, a);
  }
  return dist(p, mix(a, b, std::clamp(((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / (d * d), 0., 1.)));
}

// Whether two segments touch or cross, within the endpoint tolerance.
inline bool segmentsMeet(Position a, Position b, Position c, Position d)
{
  if (std::min({segmentDistance(a, c, d), segmentDistance(b, c, d), segmentDistance(c, a, b), segmentDistance(d, a, b)}) < 1e-5)
  {
    return true;
  }
  return cross(a, b, c) * cross(a, b, d) < 0 && cross(c, d, a) * cross(c, d, b) < 0;
}

// Convex hull of a point set, by gift wrapping on the map plane.
inline Poly hull(Poly p)
{
  std::sort(p.begin(), p.end(), [](auto a, auto b) {
    return a.x == b.x ? a.z < b.z : a.x < b.x;
  });
  Poly h;
  for (auto v : p)
  {
    while (h.size() > 1 && cross(h[h.size() - 2], h.back(), v) <= 1e-8)
    {
      h.pop_back();
    }
    h.push_back(v);
  }
  size_t k = h.size();
  for (auto it = p.rbegin() + 1; it != p.rend(); ++it)
  {
    while (h.size() > k && cross(h[h.size() - 2], h.back(), *it) <= 1e-8)
    {
      h.pop_back();
    }
    h.push_back(*it);
  }
  if (h.size() > 1)
  {
    h.pop_back();
  }
  return h;
}

// Merges any two pieces whose union is still convex, repeatedly. Convex
// decomposition leaves seams that are an artefact of how a shape was cut, not
// boundaries anything should see; this is what dissolves them again.
inline void merge(std::vector<Poly> &p)
{
  bool changed = true;
  while (changed)
  {
    changed = false;
    for (size_t i = 0; i < p.size() && !changed; ++i)
    {
      for (size_t j = i + 1; j < p.size(); ++j)
      {
        Poly all = p[i];
        all.insert(all.end(), p[j].begin(), p[j].end());
        auto h = hull(all);
        if (std::abs(area(h) - area(p[i]) - area(p[j])) < 1e-5)
        {
          p[i] = h;
          p.erase(p.begin() + j);
          changed = true;
          break;
        }
      }
    }
  }
}

// Whether a ring turns the same way at every corner, within the tolerance.
inline bool convex(const Poly &p)
{
  if (p.size() < 3 || p.size() > 128 || area(p) <= 1e-6)
  {
    return false;
  }
  for (size_t i = 0; i < p.size(); ++i)
  {
    if (!std::isfinite(p[i].x) || !std::isfinite(p[i].z) || std::max(std::abs(p[i].x), std::abs(p[i].z)) > 1e6 || cross(p[i], p[(i + 1) % p.size()], p[(i + 2) % p.size()]) < -1e-7)
    {
      return false;
    }
  }
  return true;
}

// p minus q, as convex pieces: one piece per edge of q, each the part of p
// on that edge's outer side. The pieces tile the difference exactly.
inline std::vector<Poly> subtract(Poly p, const Poly &q)
{
  std::vector<Poly> r;
  for (size_t i = 0; i < q.size() && !p.empty(); ++i)
  {
    auto a = q[i], b = q[(i + 1) % q.size()];
    double nx = b.z - a.z, nz = a.x - b.x, c = nx * a.x + nz * a.z;
    auto out = clip(p, -nx, -nz, -c);
    if (!out.empty())
    {
      r.push_back(out);
    }
    p = clip(p, nx, nz, c);
  }
  return r;
}

} // namespace procedural::world::citylayout
