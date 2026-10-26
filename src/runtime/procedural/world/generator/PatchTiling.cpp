#include "PatchTiling.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace procedural::world::generator {

double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

double signedArea(const ConvexRing &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }
  return a * .5;
}

double length(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}

Position centroid(const ConvexRing &p)
{
  Position c;
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    auto u = p[i], v = p[(i + 1) % p.size()];
    double w = u.x * v.z - v.x * u.z;
    a += w;
    c.x += (u.x + v.x) * w;
    c.z += (u.z + v.z) * w;
  }
  if (std::abs(a) < 1e-12)
  {
    return p.empty() ? Position{} : p.front();
  }
  return {c.x / (3 * a), 0, c.z / (3 * a)};
}

bool insideEvenOdd(const ConvexRing &p, Position q)
{
  bool in = false;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
  {
    if ((p[i].z > q.z) != (p[j].z > q.z))
    {
      double t = (q.z - p[i].z) / (p[j].z - p[i].z);
      if (q.x < p[i].x + t * (p[j].x - p[i].x))
      {
        in = !in;
      }
    }
  }
  return in;
}

double segmentDistance(Position p, Position a, Position b)
{
  double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
  double t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / l2, 0., 1.) : 0;
  return std::hypot(p.x - (a.x + dx * t), p.z - (a.z + dz * t));
}

double polygonDistance(Position p, const ConvexRing &poly)
{
  if (insideEvenOdd(poly, p))
  {
    return 0;
  }
  double d = INFINITY;
  for (size_t i = 0; i < poly.size(); ++i)
  {
    d = std::min(d, segmentDistance(p, poly[i], poly[(i + 1) % poly.size()]));
  }
  return d;
}

ConvexRing clipToRect(ConvexRing p, double minX, double minZ, double maxX, double maxZ)
{
  // Sutherland-Hodgman against the four window half-planes; convex in, convex out.
  for (int side = 0; side < 4 && !p.empty(); ++side)
  {
    ConvexRing out;
    auto keep = [&](Position q) {
      switch (side)
      {
      case 0:
        return q.x >= minX;
      case 1:
        return q.x <= maxX;
      case 2:
        return q.z >= minZ;
      default:
        return q.z <= maxZ;
      }
    };
    auto cut = [&](Position u, Position v) {
      double t = 0;
      Position r;
      switch (side)
      {
      case 0:
        t = (minX - u.x) / (v.x - u.x);
        r = {minX, 0, u.z + (v.z - u.z) * t};
        break;
      case 1:
        t = (maxX - u.x) / (v.x - u.x);
        r = {maxX, 0, u.z + (v.z - u.z) * t};
        break;
      case 2:
        t = (minZ - u.z) / (v.z - u.z);
        r = {u.x + (v.x - u.x) * t, 0, minZ};
        break;
      default:
        t = (maxZ - u.z) / (v.z - u.z);
        r = {u.x + (v.x - u.x) * t, 0, maxZ};
        break;
      }
      return r;
    };
    for (size_t i = 0; i < p.size(); ++i)
    {
      auto u = p[i], v = p[(i + 1) % p.size()];
      bool ku = keep(u), kv = keep(v);
      if (ku)
      {
        out.push_back(u);
      }
      if (ku != kv)
      {
        out.push_back(cut(u, v));
      }
    }
    p = std::move(out);
  }
  ConvexRing clean;
  for (auto q : p)
  {
    if (clean.empty() || length(clean.back(), q) > 1e-4)
    {
      clean.push_back(q);
    }
  }
  while (clean.size() > 1 && length(clean.front(), clean.back()) <= 1e-4)
  {
    clean.pop_back();
  }
  // Window intersections can leave nearly-collinear points a few ulps on the
  // wrong side of an otherwise convex contour. Runtime arrangement quite
  // correctly rejects a reflex turn, so canonicalise the clipped point set
  // with its monotone hull. For a valid convex input this changes only those
  // numerical duplicates/near-collinear turns; it never expands beyond the
  // points already produced by clipping.
  bool reflex = false;
  for (size_t i=0; i<clean.size() && clean.size()>=3; ++i)
  {
    const auto a=clean[i],b=clean[(i+1)%clean.size()],c=clean[(i+2)%clean.size()];
    reflex |= (b.x-a.x)*(c.z-b.z)-(b.z-a.z)*(c.x-b.x) < -1e-8;
  }
  if (reflex)
  {
    std::sort(clean.begin(),clean.end(),[](Position a,Position b){ return a.x==b.x ? a.z<b.z : a.x<b.x; });
    ConvexRing hull;
    auto turn=[](Position a,Position b,Position c){ return (b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x); };
    for (auto p:clean) { while (hull.size()>=2 && turn(hull[hull.size()-2],hull.back(),p)<=1e-10) hull.pop_back(); hull.push_back(p); }
    const size_t lower=hull.size();
    for (size_t i=clean.size(); i-->0;) { auto p=clean[i]; while (hull.size()>lower && turn(hull[hull.size()-2],hull.back(),p)<=1e-10) hull.pop_back(); hull.push_back(p); }
    if (!hull.empty()) hull.pop_back();
    clean=std::move(hull);
  }
  return clean.size() < 3 || signedArea(clean) < 1e-3 ? ConvexRing{} : clean;
}

// Shared boundaries between cells refined under different parents meet at
// T-junctions whose vertices sit a few nanometres off the neighbour's edge.
// Snap those vertices onto the edge and insert them, so every tiling test
// downstream sees exactly shared vertices instead of micron-sized overlaps.
void weldTiling(std::vector<ConvexRing> &polys)
{
  struct Hit
  {
    double t;
    Position p;
  };

  for (size_t pass = 0; pass < 2; ++pass)
  {
    for (size_t i = 0; i < polys.size(); ++i)
    {
      std::vector<ConvexRing> rebuilt;
      ConvexRing out;
      for (size_t k = 0; k < polys[i].size(); ++k)
      {
        auto a = polys[i][k], b = polys[i][(k + 1) % polys[i].size()];
        double len = length(a, b);
        out.push_back(a);
        if (len < 1e-6)
        {
          continue;
        }
        std::vector<Hit> hits;
        for (size_t j = 0; j < polys.size(); ++j)
        {
          if (j == i)
          {
            continue;
          }
          for (auto &q : polys[j])
          {
            double t = ((q.x - a.x) * (b.x - a.x) + (q.z - a.z) * (b.z - a.z)) / (len * len);
            if (t <= 1e-4 / len || t >= 1 - 1e-4 / len)
            {
              continue;
            }
            Position on{a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
            if (length(q, on) > 1e-5)
            {
              continue;
            }
            if (pass == 0)
            {
              q = on; // Snap the neighbour's vertex exactly onto this edge.
            }
            else
            {
              hits.push_back({t, q});
            }
          }
        }
        std::sort(hits.begin(), hits.end(), [](const Hit &x, const Hit &y) {
          return x.t < y.t;
        });
        for (auto &h : hits)
        {
          if (length(out.back(), h.p) > 1e-5 && length(h.p, b) > 1e-5)
          {
            out.push_back(h.p);
          }
        }
      }
      if (pass == 1)
      {
        polys[i] = std::move(out);
      }
    }
  }
}

// The rural and master-plan generators test convexity, duplicates and overlap
// with fixed absolute tolerances. Judge every offered patch with the same
// predicates so a rejected patch is omitted here with its reason rather than
// failing the whole territory downstream.
std::string tilingFault(const ConvexRing &p, double minX, double minZ, double maxX, double maxZ, double crossTolerance, double vertexTolerance)
{
  if (p.size() < 3 || p.size() > 32)
  {
    return "vertex count " + std::to_string(p.size());
  }
  if (signedArea(p) <= 1e-4)
  {
    return "degenerate area";
  }
  for (size_t k = 0; k < p.size(); ++k)
  {
    auto a = p[k], b = p[(k + 1) % p.size()];
    if (!std::isfinite(a.x) || !std::isfinite(a.z) || a.x < minX || a.x > maxX || a.z < minZ || a.z > maxZ)
    {
      return "vertex outside the domain";
    }
    if (length(a, b) < vertexTolerance)
    {
      return "degenerate edge";
    }
    for (size_t j = k + 1; j < p.size(); ++j)
    {
      if (length(a, p[j]) < vertexTolerance)
      {
        return "duplicate vertex";
      }
    }
    for (auto q : p)
    {
      if (cross(a, b, q) < -crossTolerance)
      {
        return "not convex within tolerance";
      }
    }
  }
  return "";
}

bool tilesOverlap(const ConvexRing &a, const ConvexRing &b, double tolerance)
{
  for (auto *p : {&a, &b})
  {
    for (size_t i = 0; i < p->size(); ++i)
    {
      bool separated = true;
      for (auto q : p == &a ? b : a)
      {
        if (cross((*p)[i], (*p)[(i + 1) % p->size()], q) > tolerance)
        {
          separated = false;
          break;
        }
      }
      if (separated)
      {
        return false;
      }
    }
  }
  return true;
}

Position vertexMean(const ConvexRing &p)
{
  Position c;
  for (auto q : p)
  {
    c.x += q.x / p.size();
    c.z += q.z / p.size();
  }
  return c;
}

bool onRing(const ConvexRing &ring, Position p, double tolerance)
{
  for (size_t i = 0; i < ring.size(); ++i)
  {
    if (segmentDistance(p, ring[i], ring[(i + 1) % ring.size()]) <= tolerance)
    {
      return true;
    }
  }
  return false;
}

bool strictlyInsideConvex(const ConvexRing &p, Position q)
{
  for (size_t k = 0; k < p.size(); ++k)
  {
    if (cross(p[k], p[(k + 1) % p.size()], q) <= 1e-6)
    {
      return false;
    }
  }
  return !p.empty();
}

TilingKey tilingKey(Position p)
{
  return {int64_t(std::llround(p.x * 1e6)), int64_t(std::llround(p.z * 1e6))};
}

// Union of exactly tiling convex cells: shared directed edges cancel, the
// remaining edges chain into rings. At a pinch vertex the rightmost turn is
// taken so every ring stays simple. Positive rings are outer boundaries.
bool unionTiles(const std::vector<ConvexRing> &tiles, std::vector<ConvexRing> &outers, std::vector<ConvexRing> &holes)
{
  std::map<TilingKey, Position> positions;
  std::map<std::pair<TilingKey, TilingKey>, int> edges;
  for (const auto &tile : tiles)
  {
    ConvexRing t = tile;
    if (signedArea(t) < 0)
    {
      std::reverse(t.begin(), t.end());
    }
    for (size_t i = 0; i < t.size(); ++i)
    {
      auto a = tilingKey(t[i]), b = tilingKey(t[(i + 1) % t.size()]);
      if (a == b)
      {
        continue;
      }
      positions.emplace(a, t[i]);
      positions.emplace(b, t[(i + 1) % t.size()]);
      auto reverse = edges.find({b, a});
      if (reverse != edges.end())
      {
        if (--reverse->second == 0)
        {
          edges.erase(reverse);
        }
      }
      else
      {
        ++edges[{a, b}];
      }
    }
  }
  std::map<TilingKey, std::vector<TilingKey>> out;
  for (auto &[edge, count] : edges)
  {
    if (count > 0)
    {
      out[edge.first].push_back(edge.second);
    }
  }
  std::set<std::pair<TilingKey, TilingKey>> used;
  for (auto &[edge, count] : edges)
  {
    if (count <= 0 || used.count(edge))
    {
      continue;
    }
    ConvexRing ring{positions[edge.first]};
    TilingKey start = edge.first, previous = edge.first, current = edge.second;
    used.insert(edge);
    size_t guard = 0;
    while (current != start && guard++ < 100000)
    {
      ring.push_back(positions[current]);
      auto d = positions[current], back = positions[previous];
      Position incoming{d.x - back.x, 0, d.z - back.z};
      TilingKey best{};
      double bestTurn = INFINITY;
      bool found = false;
      for (auto next : out[current])
      {
        if (used.count({current, next}))
        {
          continue;
        }
        auto n = positions[next];
        Position e{n.x - d.x, 0, n.z - d.z};
        double turn = std::atan2(incoming.x * e.z - incoming.z * e.x, incoming.x * e.x + incoming.z * e.z);
        if (turn < bestTurn)
        {
          bestTurn = turn;
          best = next;
          found = true;
        }
      }
      if (!found)
      {
        if (std::getenv("CITY_DEBUG"))
        {
          std::fprintf(stderr, "union: walk stuck at (%.3f,%.3f) after %zu vertices, %zu outgoing unmatched\n", d.x, d.z, ring.size(), out[current].size());
        }
        return false;
      }
      used.insert({current, best});
      previous = current;
      current = best;
    }
    if (current != start)
    {
      return false;
    }
    (signedArea(ring) > 0 ? outers : holes).push_back(std::move(ring));
  }
  return !outers.empty();
}

} // namespace procedural::world::generator
