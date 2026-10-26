#include "WorldWatersheds.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace procedural::world::rewrite
{
namespace
{
using Ring = std::vector<Position>;
double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
double area(const Ring &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    auto b = p[i], c = p[(i + 1) % p.size()];
    a += b.x * c.z - c.x * b.z;
  }
  return a * .5;
}
Ring clip(const Ring &input, const std::function<double(Position)> &side)
{
  Ring out;
  if (input.empty())
    return out;
  auto a = input.back();
  double sa = side(a);
  for (auto b : input)
  {
    double sb = side(b);
    if ((sa >= 0) != (sb >= 0))
    {
      double t = sa / (sa - sb);
      out.push_back({a.x + t * (b.x - a.x), 0, a.z + t * (b.z - a.z)});
    }
    if (sb >= 0)
      out.push_back(b);
    a = b;
    sa = sb;
  }
  return out;
}
double distance(Position p, Position a, Position b)
{
  double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz, t = l2 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / l2, 0.0, 1.0) : 0;
  return std::hypot(p.x - a.x - t * dx, p.z - a.z - t * dz);
}
bool intersects(Position a, Position b, Position c, Position d)
{
  auto o = [](Position a, Position b, Position c)
  {
    return orient2dExact({a.x, a.z}, {b.x, b.z}, {c.x, c.z});
  };
  double u = o(a, b, c), v = o(a, b, d), w = o(c, d, a), x = o(c, d, b);
  auto opposite = [](double a, double b)
  {
    return (a <= 0 && b >= 0) || (a >= 0 && b <= 0);
  };
  return opposite(u, v) && opposite(w, x) && std::max(std::min(a.x, b.x), std::min(c.x, d.x)) <= std::min(std::max(a.x, b.x), std::max(c.x, d.x)) &&
         std::max(std::min(a.z, b.z), std::min(c.z, d.z)) <= std::min(std::max(a.z, b.z), std::max(c.z, d.z));
}
struct Segment
{
  Position a, b;
};
} // namespace
bool buildWatersheds(const LandProduct &land, const RiverGrowthProduct &rivers, const ReliefControls &relief, bool allowTruncated, WatershedProduct &output, std::string *error)
{
  auto fail = [&](const char *s)
  {
    if (error)
      *error = s;
    return false;
  };
  if (!land.spec.validate(error))
    return false;
  if (land.spec.extentMetres > 1e9)
    return fail("Watershed extent exceeds reference precision budget");
  const auto &nodes = rivers.nodes;
  if (nodes.empty() || nodes.size() > 4096 || (!allowTruncated && rivers.budgetReached))
    return fail("Invalid watershed site budget or truncated drainage input");
  const double e = land.spec.extentMetres, tol = std::max(1e-7, e * 1e-9);
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    auto n = nodes[i];
    if (!std::isfinite(n.position.x) || !std::isfinite(n.position.y) || !std::isfinite(n.position.z) || n.position.x < 0 || n.position.z < 0 || n.position.x > e || n.position.z > e ||
        (n.receiver != UINT32_MAX && (n.receiver >= i || n.position.y <= nodes[n.receiver].position.y)))
      return fail("Invalid river node or receiver");
    for (size_t j = 0; j < i; ++j)
      if (std::hypot(n.position.x - nodes[j].position.x, n.position.z - nodes[j].position.z) <= tol)
        return fail("Coincident watershed sites");
  }
  std::vector<Ring> triangles;
  std::vector<Segment> coast;
  for (auto &c : land.cells)
    if (c.land)
    {
      PatchMesh mesh;
      if (!compilePatchMesh(
              c.patch,
              land.boundaries,
              {},
              [&](double, double)
              {
                return land.spec.seaLevelMetres;
              },
              mesh,
              error))
        return false;
      for (auto t : mesh.triangles)
      {
        Ring r{mesh.vertices[t[0]].position, mesh.vertices[t[1]].position, mesh.vertices[t[2]].position};
        if (area(r) < 0)
          std::reverse(r.begin(), r.end());
        triangles.push_back(r);
      }
    }
  if (triangles.empty() || triangles.size() > 10000)
    return fail("Invalid watershed land triangle budget");
  for (auto &loop : land.coasts)
    for (auto use : loop.edges)
    {
      auto edge = land.boundaries.find(use.edgeId);
      if (!edge || edge->vertices.size() < 2)
        return fail("Missing coast boundary");
      for (size_t i = 1; i < edge->vertices.size(); ++i)
        coast.push_back({edge->vertices[i - 1].position, edge->vertices[i].position});
    }
  if (coast.empty())
    return fail("Missing coastline");
  auto onCoast = [&](Position p)
  {
    for (auto s : coast)
      if (distance(p, s.a, s.b) <= tol)
        return true;
    return false;
  };
  auto inLand = [&](Position p)
  {
    for (auto &t : triangles)
      if (cross(t[0], t[1], p) >= -tol && cross(t[1], t[2], p) >= -tol && cross(t[2], t[0], p) >= -tol)
        return true;
    return false;
  };
  for (auto n : nodes)
    if (!inLand(n.position) && !onCoast(n.position))
      return fail("River node outside physical land");
  struct Bounds
  {
    double minX, minZ, maxX, maxZ;
  };
  auto bounds = [](const Ring &ring)
  {
    Bounds box{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (auto p : ring)
    {
      box.minX = std::min(box.minX, p.x);
      box.minZ = std::min(box.minZ, p.z);
      box.maxX = std::max(box.maxX, p.x);
      box.maxZ = std::max(box.maxZ, p.z);
    }
    return box;
  };
  auto overlap = [](Bounds a, Bounds b)
  {
    return a.maxX >= b.minX && a.minX <= b.maxX && a.maxZ >= b.minZ && a.minZ <= b.maxZ;
  };
  std::vector<Bounds> triangleBounds;
  for (auto &triangle : triangles)
    triangleBounds.push_back(bounds(triangle));
  std::vector<Ring> voronoi;
  WatershedProduct next;
  next.sourceGraphTruncated = rivers.budgetReached;
  for (uint32_t i = 0; i < nodes.size(); ++i)
  {
    Ring polygon{{0, 0, 0}, {e, 0, 0}, {e, 0, e}, {0, 0, e}};
    auto a = nodes[i].position;
    for (uint32_t j = 0; j < nodes.size() && !polygon.empty(); ++j)
      if (i != j)
      {
        auto b = nodes[j].position;
        double dx = b.x - a.x, dz = b.z - a.z, mx = (a.x + b.x) * .5, mz = (a.z + b.z) * .5;
        polygon = clip(
            polygon,
            [&](Position p)
            {
              return (mx - p.x) * dx + (mz - p.z) * dz;
            });
      }
    WatershedCell cell;
    cell.riverNode = i;
    auto polygonBounds = bounds(polygon);
    for (size_t ti = 0; ti < triangles.size(); ++ti)
    {
      if (!overlap(polygonBounds, triangleBounds[ti]))
        continue;
      const auto &t = triangles[ti];
      Ring fragment = polygon;
      for (int side = 0; side < 3 && !fragment.empty(); ++side)
        fragment = clip(
            fragment,
            [&](Position p)
            {
              return cross(t[side], t[(side + 1) % 3], p);
            });
      double size = area(fragment);
      if (size > tol * tol)
      {
        cell.areaMetres2 += size;
        cell.fragments.push_back(std::move(fragment));
      }
    }
    if (cell.areaMetres2 <= 0)
      return fail("River site has no land watershed");
    next.landAreaMetres2 += cell.areaMetres2;
    next.cells.push_back(std::move(cell));
    voronoi.push_back(std::move(polygon));
  }
  if (std::abs(next.landAreaMetres2 - land.landAreaMetres2) > std::max(1e-4, land.landAreaMetres2 * 1e-8))
    return fail("Watershed fragments do not cover physical land");
  std::map<std::pair<int64_t, int64_t>, uint32_t> vertexIds;
  bool invalidVertex = false;
  auto vertex = [&](Position p)
  {
    auto key = std::make_pair(std::llround(p.x / tol), std::llround(p.z / tol));
    auto found = vertexIds.find(key);
    if (found != vertexIds.end())
      return found->second;
    CrestVertex v;
    v.position = p;
    v.coastal = onCoast(p);
    double nearest = std::numeric_limits<double>::infinity(), high = -nearest;
    for (auto n : nodes)
      nearest = std::min(nearest, std::hypot(p.x - n.position.x, p.z - n.position.z));
    for (uint32_t i = 0; i < nodes.size(); ++i)
      if (std::abs(std::hypot(p.x - nodes[i].position.x, p.z - nodes[i].position.z) - nearest) <= tol * 4)
      {
        v.equidistantSites.push_back(i);
        high = std::max(high, nodes[i].position.y);
      }
    if (v.coastal)
      v.position.y = land.spec.seaLevelMetres;
    else
    {
      if (v.equidistantSites.size() < 3)
        invalidVertex = true;
      v.position.y = high + relief.sample(p.x, p.z).terrainLambda * nearest;
    }
    uint32_t id = uint32_t(next.vertices.size());
    vertexIds.emplace(key, id);
    next.vertices.push_back(std::move(v));
    return id;
  };
  for (uint32_t i = 0; i < voronoi.size(); ++i)
  {
    auto &ring = voronoi[i];
    for (size_t k = 0; k < ring.size(); ++k)
    {
      auto a = ring[k], b = ring[(k + 1) % ring.size()];
      double length = std::hypot(b.x - a.x, b.z - a.z);
      if (length <= tol)
        continue;
      Position mid{(a.x + b.x) * .5, 0, (a.z + b.z) * .5};
      double d = std::hypot(mid.x - nodes[i].position.x, mid.z - nodes[i].position.z);
      uint32_t neighbour = UINT32_MAX;
      for (uint32_t j = 0; j < nodes.size(); ++j)
        if (i != j && std::abs(std::hypot(mid.x - nodes[j].position.x, mid.z - nodes[j].position.z) - d) <= tol * 4)
        {
          neighbour = j;
          break;
        }
      if (neighbour == UINT32_MAX || neighbour < i)
        continue;
      std::vector<std::pair<double, double>> spans;
      auto edgeBounds = bounds({a, b});
      for (size_t ti = 0; ti < triangles.size(); ++ti)
      {
        if (!overlap(edgeBounds, triangleBounds[ti]))
          continue;
        const auto &t = triangles[ti];
        double lo = 0, hi = 1;
        bool valid = true;
        for (int side = 0; side < 3; ++side)
        {
          double sa = cross(t[side], t[(side + 1) % 3], a), sb = cross(t[side], t[(side + 1) % 3], b);
          if (sa < 0 && sb < 0)
          {
            valid = false;
            break;
          }
          if ((sa < 0) != (sb < 0))
          {
            double u = sa / (sa - sb);
            if (sa < 0)
              lo = std::max(lo, u);
            else
              hi = std::min(hi, u);
          }
        }
        if (valid && hi - lo > tol / length)
          spans.push_back({lo, hi});
      }
      std::sort(spans.begin(), spans.end());
      std::vector<std::pair<double, double>> merged;
      for (auto span : spans)
      {
        if (merged.empty() || span.first > merged.back().second + tol / length)
          merged.push_back(span);
        else
          merged.back().second = std::max(merged.back().second, span.second);
      }
      for (auto span : merged)
      {
        auto at = [&](double t)
        {
          return Position{a.x + t * (b.x - a.x), 0, a.z + t * (b.z - a.z)};
        };
        auto p = at(span.first), q = at(span.second);
        bool crossing = false;
        for (auto n : nodes)
          if (n.receiver != UINT32_MAX && intersects(p, q, n.position, nodes[n.receiver].position))
          {
            crossing = true;
            break;
          }
        auto va = vertex(p), vb = vertex(q);
        if (va == vb)
          return fail("Crest edge collapses at canonical precision");
        next.edges.push_back({va, vb, i, neighbour, crossing});
      }
    }
  }
  if (invalidVertex)
    return fail("Interior crest endpoint is not a Voronoi tie of at least three sites");
  output = std::move(next);
  if (error)
    error->clear();
  return true;
}
} // namespace procedural::world::rewrite
