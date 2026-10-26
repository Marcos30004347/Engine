#include "WorldSurfaceArrangement.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
constexpr double epsilon = 1e-6;

bool fail(std::string *e, const char *t)
{
  if (e)
  {
    *e = t;
  }
  return false;
}

double side(Position a, Position b, Position p)
{
  return (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
}

double area(const std::vector<Position> &p)
{
  double result = 0;
  for (size_t i = 1; i + 1 < p.size(); ++i)
  {
    result += side(p[0], p[i], p[i + 1]);
  }
  return result * .5;
}

bool road(SurfaceRole r)
{
  return r == SurfaceRole::Road || r == SurfaceRole::Shoulder;
}
} // namespace

bool arrangeSurfaceContours(const std::vector<SurfaceContour> &contours, const SurfaceMapSettings &s, const std::function<double(double, double)> &terrain, uint64_t revision, SurfaceArrangement &out, std::string *error)
{
  if (!terrain || !revision || contours.size() > 4096 || s.maximumFaces == 0 || s.maximumFaces > 40000 || !std::isfinite(s.seamToleranceMetres) || s.seamToleranceMetres <= 0 || s.seamToleranceMetres > .1)
  {
    return fail(error, "invalid arrangement budget or revision");
  }
  for (double v : {s.minX, s.minZ, s.maxX, s.maxZ})
  {
    if (!std::isfinite(v) || std::abs(v) > 1e6)
    {
      return fail(error, "invalid arrangement bounds");
    }
  }
  if (s.maxX <= s.minX || s.maxZ <= s.minZ)
  {
    return fail(error, "empty arrangement bounds");
  }
  if (s.cutsX.size() + s.cutsZ.size() > 128)
  {
    return fail(error, "tile cut budget exceeded");
  }
  for (const auto *cuts : {&s.cutsX, &s.cutsZ})
  {
    for (double v : *cuts)
    {
      if (!std::isfinite(v) || v <= (cuts == &s.cutsX ? s.minX : s.minZ) + epsilon || v >= (cuts == &s.cutsX ? s.maxX : s.maxZ) - epsilon)
      {
        return fail(error, "invalid interior tile cut");
      }
    }
  }
  std::set<uint32_t> layers{0};
  std::set<uint64_t> sourceIds;
  for (const auto &c : contours)
  {
    if (!c.id || !sourceIds.insert(c.id).second || c.layer > 2 || c.loop.size() < 3 || c.loop.size() > 256)
    {
      return fail(error, "invalid contour identity or polygon budget");
    }
    if (!std::isfinite(c.plane.x) || !std::isfinite(c.plane.z) || !std::isfinite(c.plane.constant))
    {
      return fail(error, "invalid contour plane");
    }
    for (size_t i = 0; i < c.loop.size(); ++i)
    {
      auto p = c.loop[i];
      if (!std::isfinite(p.x) || !std::isfinite(p.z) || p.x < s.minX - epsilon || p.x > s.maxX + epsilon || p.z < s.minZ - epsilon || p.z > s.maxZ + epsilon)
      {
        return fail(error, "contour outside map bounds");
      }
      if (side(p, c.loop[(i + 1) % c.loop.size()], c.loop[(i + 2) % c.loop.size()]) < -epsilon)
      {
        return fail(error, "contour must be convex CCW");
      }
    }
    if (area(c.loop) < 1e-9)
    {
      return fail(error, "degenerate contour");
    }
    layers.insert(c.layer);
  }
  SurfaceArrangement result;
  result.settings = s;
  result.revision = revision;
  for (auto layer : layers)
  {
    struct Segment
    {
      Position a, b;
      uint64_t source = 0;
      std::vector<uint32_t> splits;
    };

    std::vector<Segment> segments;
    auto add = [&](Position a, Position b, uint64_t source) {
      if (std::hypot(a.x - b.x, a.z - b.z) > epsilon)
      {
        segments.push_back({a, b, source, {}});
      }
    };
    std::vector<Position> rectangle = {{s.minX, 0, s.minZ}, {s.maxX, 0, s.minZ}, {s.maxX, 0, s.maxZ}, {s.minX, 0, s.maxZ}};
    for (int i = 0; i < 4; ++i)
    {
      add(rectangle[i], rectangle[(i + 1) % 4], 0);
    }
    for (double x : s.cutsX)
    {
      add({x, 0, s.minZ}, {x, 0, s.maxZ}, 0);
    }
    for (double z : s.cutsZ)
    {
      add({s.minX, 0, z}, {s.maxX, 0, z}, 0);
    }
    // Reuse horizontal cuts across overlapping vertical ranges. With explicit
    // tile columns, end each support cut on the enclosing vertical boundaries.
    // Both ends remain connected to the global graph, without extending tiny
    // city subdivisions through distant farmland. No cutsX retains old geometry.
    std::map<int64_t, std::vector<std::pair<double, double>>> scanlines;
    std::vector<std::array<double, 4>> intervals;
    for (const auto &c : contours)
    {
      if (c.layer == layer)
      {
        double lo = 1e30, hi = -1e30, left = 1e30, right = -1e30;
        for (size_t i = 0; i < c.loop.size(); ++i)
        {
          add(c.loop[i], c.loop[(i + 1) % c.loop.size()], c.id);
          lo = std::min(lo, c.loop[i].z);
          hi = std::max(hi, c.loop[i].z);
          left = std::min(left, c.loop[i].x);
          right = std::max(right, c.loop[i].x);
        }
        intervals.push_back({hi, lo, left, right});
      }
    }
    std::sort(intervals.begin(), intervals.end());
    for (auto [hi, lo, left, right] : intervals)
    {
      auto chosen = scanlines.end();
      for (auto it = scanlines.begin(); it != scanlines.end(); ++it)
      {
        if (it->first * epsilon > lo + epsilon && it->first * epsilon < hi - epsilon)
        {
          chosen = it;
          break;
        }
      }
      if (chosen == scanlines.end())
      {
        chosen = scanlines.emplace(std::llround((lo + hi) * .5 / epsilon), std::vector<std::pair<double, double>>{}).first;
      }
      double from = s.minX, to = s.maxX;
      for (double cut : s.cutsX)
      {
        if (cut <= left)
        {
          from = std::max(from, cut);
        }
        if (cut >= right)
        {
          to = std::min(to, cut);
        }
      }
      chosen->second.push_back({from, to});
    }
    for (auto &[z, spans] : scanlines)
    {
      double value = z * epsilon;
      if (value <= s.minZ + epsilon || value >= s.maxZ - epsilon)
      {
        continue;
      }
      std::sort(spans.begin(), spans.end());
      double from = spans.front().first, to = spans.front().second;
      for (size_t i = 1; i < spans.size(); ++i)
      {
        if (spans[i].first <= to + epsilon)
        {
          to = std::max(to, spans[i].second);
        }
        else
        {
          add({from, 0, value}, {to, 0, value}, 0);
          from = spans[i].first;
          to = spans[i].second;
        }
      }
      add({from, 0, value}, {to, 0, value}, 0);
    }
    if (segments.size() > 16384)
    {
      size_t generated = 0;
      for (const auto &segment : segments)
      {
        generated += segment.source == 0;
      }
      std::string detail = "arrangement segment budget exceeded: " + std::to_string(segments.size()) + " segments, " + std::to_string(segments.size() - generated) + " from contours, " + std::to_string(generated) +
                           " generated cuts and supports";
      return fail(error, detail.c_str());
    }
    std::vector<Position> points;
    std::map<std::pair<int64_t, int64_t>, std::vector<uint32_t>> bins;
    auto vertex = [&](Position p) {
      int64_t x = std::llround(p.x / epsilon), z = std::llround(p.z / epsilon);
      uint32_t best = UINT32_MAX;
      for (int dx = -2; dx <= 2; ++dx)
      {
        for (int dz = -2; dz <= 2; ++dz)
        {
          auto it = bins.find({x + dx, z + dz});
          if (it != bins.end())
          {
            for (auto id : it->second)
            {
              if (std::hypot(p.x - points[id].x, p.z - points[id].z) <= epsilon * 2 && id < best)
              {
                best = id;
              }
            }
          }
        }
      }
      if (best != UINT32_MAX)
      {
        return best;
      }
      uint32_t id = points.size();
      points.push_back({p.x, 0, p.z});
      bins[{x, z}].push_back(id);
      return id;
    };
    // Publish authored endpoints first so intersections snap to their identities.
    for (auto &e : segments)
    {
      e.splits.push_back(vertex(e.a));
      e.splits.push_back(vertex(e.b));
      e.a = points[e.splits[0]];
      e.b = points[e.splits[1]];
    }
    uint64_t comparisons = 0;
    for (size_t i = 0; i < segments.size(); ++i)
    {
      for (size_t j = i + 1; j < segments.size(); ++j)
      {
        auto &a = segments[i];
        auto &b = segments[j];
        if (std::max(a.a.x, a.b.x) + epsilon < std::min(b.a.x, b.b.x) || std::max(b.a.x, b.b.x) + epsilon < std::min(a.a.x, a.b.x) || std::max(a.a.z, a.b.z) + epsilon < std::min(b.a.z, b.b.z) ||
            std::max(b.a.z, b.b.z) + epsilon < std::min(a.a.z, a.b.z))
        {
          continue;
        }
        // Charge the intersection budget only after broad-phase rejection.
        // The segment cap still bounds cheap box comparisons; disjoint city
        // contours must not consume the budget for geometric intersection work.
        if (++comparisons > 20000000)
        {
          return fail(error, "arrangement intersection budget exceeded");
        }
        double ax = a.b.x - a.a.x, az = a.b.z - a.a.z, bx = b.b.x - b.a.x, bz = b.b.z - b.a.z, al = std::hypot(ax, az), bl = std::hypot(bx, bz), den = ax * bz - az * bx;
        if (al < epsilon || bl < epsilon)
        {
          continue;
        }
        auto attach = [&](uint32_t id, Segment &e) {
          auto p = points[id];
          double dx = e.b.x - e.a.x, dz = e.b.z - e.a.z, l2 = dx * dx + dz * dz, t = ((p.x - e.a.x) * dx + (p.z - e.a.z) * dz) / l2;
          if (t >= -epsilon / std::sqrt(l2) && t <= 1 + epsilon / std::sqrt(l2) && std::abs(side(e.a, e.b, p)) <= epsilon * std::sqrt(l2) * 2)
          {
            e.splits.push_back(id);
          }
        };
        attach(a.splits[0], b);
        attach(a.splits[1], b);
        attach(b.splits[0], a);
        attach(b.splits[1], a);
        if (std::abs(den) < 1e-12 * al * bl)
        {
          continue;
        }
        double dx = b.a.x - a.a.x, dz = b.a.z - a.a.z, t = (dx * bz - dz * bx) / den, u = (dx * az - dz * ax) / den;
        if (t >= 0 && t <= 1 && u >= 0 && u <= 1)
        {
          auto id = vertex({a.a.x + t * ax, 0, a.a.z + t * az});
          a.splits.push_back(id);
          b.splits.push_back(id);
        }
      }
    }
    if (points.size() > 60000)
    {
      return fail(error, "arrangement vertex budget exceeded");
    }

    struct Edge
    {
      uint32_t a, b;
      std::set<uint64_t> sources;
    };

    std::map<std::pair<uint32_t, uint32_t>, Edge> unique;
    for (auto &e : segments)
    {
      double dx = e.b.x - e.a.x, dz = e.b.z - e.a.z;
      std::sort(e.splits.begin(), e.splits.end(), [&](uint32_t a, uint32_t b) {
        return (points[a].x - e.a.x) * dx + (points[a].z - e.a.z) * dz < (points[b].x - e.a.x) * dx + (points[b].z - e.a.z) * dz;
      });
      e.splits.erase(std::unique(e.splits.begin(), e.splits.end()), e.splits.end());
      for (size_t i = 1; i < e.splits.size(); ++i)
      {
        const std::pair<double, double> key = std::minmax<double>(e.splits[i - 1], e.splits[i]);
        if (key.first == key.second)
        {
          continue;
        }
        auto &edge = unique[key];
        edge.a = key.first;
        edge.b = key.second;
        if (e.source)
        {
          edge.sources.insert(e.source);
        }
      }
    }
    std::vector<std::vector<uint32_t>> neighbors(points.size());
    for (auto &[key, e] : unique)
    {
      neighbors[e.a].push_back(e.b);
      neighbors[e.b].push_back(e.a);
    }
    for (uint32_t i = 0; i < points.size(); ++i)
    {
      std::sort(neighbors[i].begin(), neighbors[i].end(), [&](uint32_t a, uint32_t b) {
        return std::atan2(points[a].z - points[i].z, points[a].x - points[i].x) < std::atan2(points[b].z - points[i].z, points[b].x - points[i].x);
      });
    }
    std::set<std::pair<uint32_t, uint32_t>> visited;
    uint32_t base = result.vertices.size();
    result.vertices.insert(result.vertices.end(), points.begin(), points.end());
    auto contains = [](const std::vector<Position> &polygon, Position p) {
      for (size_t i = 0; i < polygon.size(); ++i)
      {
        if (side(polygon[i], polygon[(i + 1) % polygon.size()], p) < -1e-9)
        {
          return false;
        }
      }
      return true;
    };
    for (auto &[key, e] : unique)
    {
      for (auto directed : std::vector<std::pair<uint32_t, uint32_t>>{{e.a, e.b}, {e.b, e.a}})
      {
        if (visited.count(directed))
        {
          continue;
        }
        auto current = directed;
        std::vector<uint32_t> ring;
        do
        {
          if (!visited.insert(current).second)
          {
            return fail(error, "invalid arrangement face walk");
          }
          ring.push_back(current.first);
          auto &list = neighbors[current.second];
          auto it = std::find(list.begin(), list.end(), current.first);
          size_t index = size_t(it - list.begin());
          current = {current.second, list[(index + list.size() - 1) % list.size()]};
          if (ring.size() > 8192)
          {
            return fail(error, "arrangement face boundary budget exceeded");
          }
        } while (current != directed);
        std::vector<Position> polygon;
        for (auto id : ring)
        {
          polygon.push_back(points[id]);
        }
        double signedArea = area(polygon);
        if (signedArea <= 1e-9)
        {
          continue;
        }
        if (std::set<uint32_t>(ring.begin(), ring.end()).size() != ring.size())
        {
          return fail(error, "pinched arrangement face");
        }
        // Find an interior ear centroid, also valid for concave outer terrain faces.
        Position sample{};
        bool found = false;
        for (size_t i = 0; i < ring.size() && !found; ++i)
        {
          Position a = polygon[(i + ring.size() - 1) % ring.size()], b = polygon[i], c = polygon[(i + 1) % ring.size()];
          if (side(a, b, c) <= 1e-9)
          {
            continue;
          }
          bool empty = true;
          for (size_t j = 0; j < ring.size(); ++j)
          {
            if (j == i || j == (i + 1) % ring.size() || j == (i + ring.size() - 1) % ring.size())
            {
              continue;
            }
            auto p = polygon[j];
            if (side(a, b, p) > 1e-10 && side(b, c, p) > 1e-10 && side(c, a, p) > 1e-10)
            {
              empty = false;
              break;
            }
          }
          if (empty)
          {
            sample = {(a.x + b.x + c.x) / 3, 0, (a.z + b.z + c.z) / 3};
            found = true;
          }
        }
        if (!found)
        {
          return fail(error, "arrangement has no interior face sample");
        }
        SurfaceContour owner;
        owner.layer = layer;
        owner.material = layer ? UINT32_MAX : 0;
        for (const auto &c : contours)
        {
          if (c.layer == layer && contains(c.loop, sample))
          {
            if (road(c.role) && road(owner.role))
            {
              for (auto p : polygon)
              {
                if (std::abs(c.plane.height(p.x, p.z) - owner.plane.height(p.x, p.z)) > s.seamToleranceMetres)
                {
                  return fail(error, "junction requires compatible locked road surfaces");
                }
              }
            }
            owner = c;
          }
        }
        if (owner.material == UINT32_MAX)
        {
          continue;
        }
        SurfaceMapFace face;
        face.id = result.faces.size() + 1;
        face.sourceContour = owner.id;
        face.material = owner.material;
        face.layer = layer;
        face.role = owner.role;
        face.terrainSurface = owner.terrainSurface;
        face.plane = owner.plane;
        for (auto id : ring)
        {
          face.vertices.push_back(base + id);
        }
        result.faces.push_back(std::move(face));
        if (result.faces.size() > s.maximumFaces)
        {
          return fail(error, "arrangement face budget exceeded");
        }
      }
    }
    std::map<std::pair<uint32_t, uint32_t>, size_t> edgeIds;
    for (const auto &f : result.faces)
    {
      if (f.layer == layer)
      {
        for (size_t i = 0; i < f.vertices.size(); ++i)
        {
          auto a = f.vertices[i], b = f.vertices[(i + 1) % f.vertices.size()];
          const std::pair<uint32_t, uint32_t> key = std::minmax<uint32_t>(a, b);
          auto it = edgeIds.find(key);
          if (it == edgeIds.end())
          {
            SurfaceMapEdge edge;
            edge.id = result.edges.size() + 1;
            edge.a = key.first;
            edge.b = key.second;
            edge.layer = layer;
            auto raw = unique.find({edge.a - base, edge.b - base});
            if (raw != unique.end())
            {
              edge.sourceContours.assign(raw->second.sources.begin(), raw->second.sources.end());
            }
            edgeIds[key] = result.edges.size();
            result.edges.push_back(edge);
            it = edgeIds.find(key);
          }
          auto &edge = result.edges[it->second];
          auto &owner = a < b ? edge.leftFace : edge.rightFace;
          if (owner)
          {
            return fail(error, "nonmanifold arrangement edge");
          }
          owner = f.id;
        }
      }
    }
  }
  std::vector<bool> assigned(result.vertices.size(), false);
  for (const auto &f : result.faces)
  {
    for (auto id : f.vertices)
    {
      auto &p = result.vertices[id];
      double y = f.terrainSurface ? terrain(p.x, p.z) : f.plane.height(p.x, p.z);
      if (!std::isfinite(y))
      {
        return fail(error, "unsupported arrangement surface");
      }
      if (assigned[id] && std::abs(p.y - y) > s.seamToleranceMetres)
      {
        return fail(error, "surface seam height mismatch");
      }
      if (!assigned[id] || f.terrainSurface)
      {
        p.y = y;
      }
      assigned[id] = true;
    }
  }
  for (auto &e : result.edges)
  {
    if (e.leftFace && e.rightFace)
    {
      const auto &a = result.faces[e.leftFace - 1];
      const auto &b = result.faces[e.rightFace - 1];
      e.materialBoundary = a.material != b.material;
      e.surfaceBreak = a.role != b.role || a.plane.x != b.plane.x || a.plane.z != b.plane.z;
    }
    else
    {
      e.materialBoundary = e.surfaceBreak = true;
    }
  }
  out = std::move(result);
  return true;
}

bool meshSurfaceArrangement(const SurfaceArrangement &map, const std::function<double(double, double)> &terrain, const SurfaceRefinement &s, SurfaceArrangementMesh &out, std::string *error)
{
  if (!terrain || !map.revision || map.faces.empty() || !std::isfinite(s.groundSpacingMetres) || !std::isfinite(s.modifiedSpacingMetres) || s.groundSpacingMetres < .5 || s.modifiedSpacingMetres < .5 ||
      s.maximumInteriorPoints > 100000)
  {
    return fail(error, "invalid arrangement mesh settings");
  }
  for (auto &e : map.edges)
  {
    if (!e.id || e.a >= map.vertices.size() || e.b >= map.vertices.size() || e.a == e.b)
    {
      return fail(error, "invalid map edge indices");
    }
  }
  for (size_t i = 0; i < map.faces.size(); ++i)
  {
    auto &f = map.faces[i];
    if (f.id != i + 1 || f.vertices.size() < 3 || f.vertices.size() > 8192)
    {
      return fail(error, "invalid map face identity");
    }
    for (auto id : f.vertices)
    {
      if (id >= map.vertices.size())
      {
        return fail(error, "invalid map face vertex");
      }
    }
  }
  BoundaryRegistry registry;
  std::vector<BoundaryInput> boundaries;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> edgeIds;
  for (auto &e : map.edges)
  {
    boundaries.push_back({e.id, {e.a + 1, map.vertices[e.a]}, {e.b + 1, map.vertices[e.b]}, {}});
    edgeIds[{e.a, e.b}] = e.id;
  }
  if (!registry.commit(boundaries, error))
  {
    return false;
  }
  SurfaceArrangementMesh mesh;
  mesh.vertices = map.vertices;
  uint32_t totalSamples = 0;
  for (auto &f : map.faces)
  {
    PatchDomain domain;
    domain.id = f.id;
    for (size_t j = 0; j < f.vertices.size(); ++j)
    {
      auto a = f.vertices[j], b = f.vertices[(j + 1) % f.vertices.size()];
      auto edge = edgeIds.find(std::minmax(a, b));
      if (edge == edgeIds.end())
      {
        return fail(error, "face references missing canonical edge");
      }
      domain.outer.push_back({edge->second, a > b});
    }
    auto height = [&](double x, double z) {
      return f.terrainSurface ? terrain(x, z) : f.plane.height(x, z);
    };
    double minX = 1e30, maxX = -1e30, minZ = 1e30, maxZ = -1e30;
    for (auto id : f.vertices)
    {
      auto p = map.vertices[id];
      minX = std::min(minX, p.x);
      maxX = std::max(maxX, p.x);
      minZ = std::min(minZ, p.z);
      maxZ = std::max(maxZ, p.z);
    }
    double spacing = f.role == SurfaceRole::Ground ? s.groundSpacingMetres : s.modifiedSpacingMetres;
    if ((maxX - minX) / spacing * (maxZ - minZ) / spacing > 200000)
    {
      return fail(error, "face refinement candidate budget exceeded");
    }
    std::vector<Position> samples;
    uint64_t candidate = 0;
    for (double z = minZ + spacing * .53; z < maxZ; z += spacing)
    {
      for (double x = minX + spacing * .47; x < maxX; x += spacing)
      {
        // Bounded deterministic jitter avoids cocircular rows that can stall
        // constraint restoration. Only interior samples move; map edges do not.
        auto jitter = [&](uint64_t purpose) {
          return (double(featureSeed(s.samplingSeed ? s.samplingSeed : map.revision, 11, f.id, purpose, candidate) >> 11) * 0x1.0p-53 - .5) * spacing * .2;
        };
        Position p{x + jitter(1), 0, z + jitter(2)};
        ++candidate;
        bool inside = false, boundary = false;
        for (size_t j = 0; j < f.vertices.size(); ++j)
        {
          auto a = map.vertices[f.vertices[j]], b = map.vertices[f.vertices[(j + 1) % f.vertices.size()]];
          double dx = b.x - a.x, dz = b.z - a.z, t = ((p.x - a.x) * dx + (p.z - a.z) * dz) / (dx * dx + dz * dz);
          if (t >= 0 && t <= 1 && std::abs(side(a, b, p)) < 1e-6 * std::hypot(dx, dz))
          {
            boundary = true;
            break;
          }
          if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
          {
            inside = !inside;
          }
        }
        inside = inside && !boundary;
        if (inside)
        {
          if (++totalSamples > s.maximumInteriorPoints)
          {
            return fail(error, "interior refinement budget exceeded");
          }
          samples.push_back(p);
        }
      }
    }
    PatchMesh patch;
    if (!compilePatchMesh(domain, registry, samples, height, patch, error))
    {
      if (error)
      {
        *error += " face=" + std::to_string(f.id);
      }
      return false;
    }
    std::vector<uint32_t> indices;
    for (const auto &v : patch.vertices)
    {
      if (v.owner == 0 && v.index > 0 && v.index <= map.vertices.size())
      {
        indices.push_back(uint32_t(v.index - 1));
      }
      else
      {
        indices.push_back(mesh.vertices.size());
        mesh.vertices.push_back(v.position);
      }
    }
    for (auto t : patch.triangles)
    {
      SurfaceTriangle triangle{{indices[t[0]], indices[t[1]], indices[t[2]]}, f.id, f.material, f.layer, f.role};
      if (side(mesh.vertices[triangle.vertices[0]], mesh.vertices[triangle.vertices[1]], mesh.vertices[triangle.vertices[2]]) > 0)
      {
        std::swap(triangle.vertices[1], triangle.vertices[2]);
      }
      mesh.triangles.push_back(triangle);
    }
    if (f.layer == 0)
    {
      mesh.groundArea += patch.triangleArea;
    }
    mesh.patches.push_back(std::move(patch));
  }
  double expected = (map.settings.maxX - map.settings.minX) * (map.settings.maxZ - map.settings.minZ);
  if (std::abs(mesh.groundArea - expected) > expected * 1e-7)
  {
    return fail(error, "ground arrangement coverage mismatch");
  }
  out = std::move(mesh);
  return true;
}
} // namespace procedural::world::rewrite
