#include "procedural/geom3/Prism.hpp"

#include "procedural/geom2/Offset.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::geom3 {

namespace {

using geom2::areaMetres;
using geom2::counterClockwise;
using geom2::orientation;
using geom2::polygonFault;
using geom2::toMetres;

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

bool insideTriangle(const Point &a, const Point &b, const Point &c, const Point &p)
{
  const int d1 = orientation(a, b, p);
  const int d2 = orientation(b, c, p);
  const int d3 = orientation(c, a, p);

  return d1 >= 0 && d2 >= 0 && d3 >= 0;
}

// Joins every hole into the outer ring with a pair of coincident bridge edges,
// so the whole polygon becomes one simple ring an ear clipper can consume.
//
// The bridge runs from the hole's rightmost vertex to a visible outer vertex.
// Taking the rightmost one guarantees a vertex to its right on the outer ring,
// which is what makes a visible partner exist at all.
bool bridgeHoles(const Polygon &polygon, std::vector<Point> &ring, std::string *error)
{
  ring = polygon.outer;

  std::vector<Ring> holes = polygon.holes;

  // Rightmost first, so a bridge never has to cross a hole that is still
  // waiting to be joined.
  std::sort(holes.begin(), holes.end(), [](const Ring &l, const Ring &r) {
    const auto rightmost = [](const Ring &ring) {
      Point best = ring.front();

      for (const auto &p : ring)
      {
        if (best < p)
        {
          best = p;
        }
      }

      return best;
    };

    return rightmost(r) < rightmost(l);
  });

  for (const auto &hole : holes)
  {
    size_t holeIndex = 0;

    for (size_t i = 1; i < hole.size(); ++i)
    {
      if (hole[holeIndex] < hole[i])
      {
        holeIndex = i;
      }
    }

    // The visible partner: among the ring vertices to the right of the hole's
    // rightmost point, the one whose bridge crosses nothing.
    bool found = false;
    size_t best = 0;
    double bestDistance = 0;

    for (size_t k = 0; k < ring.size(); ++k)
    {
      if (ring[k].x < hole[holeIndex].x)
      {
        continue;
      }

      bool blocked = false;

      for (size_t e = 0; e < ring.size() && !blocked; ++e)
      {
        const Point &a = ring[e];
        const Point &b = ring[(e + 1) % ring.size()];

        if (a == ring[k] || b == ring[k] || a == hole[holeIndex] || b == hole[holeIndex])
        {
          continue;
        }

        blocked = geom2::segmentsProperlyCross(hole[holeIndex], ring[k], a, b);
      }

      for (const auto &other : polygon.holes)
      {
        for (size_t e = 0; e < other.size() && !blocked; ++e)
        {
          const Point &a = other[e];
          const Point &b = other[(e + 1) % other.size()];

          if (a == hole[holeIndex] || b == hole[holeIndex])
          {
            continue;
          }

          blocked = geom2::segmentsProperlyCross(hole[holeIndex], ring[k], a, b);
        }
      }

      if (blocked)
      {
        continue;
      }

      const double distance = std::hypot(toMetres(ring[k].x - hole[holeIndex].x), toMetres(ring[k].y - hole[holeIndex].y));

      if (!found || distance < bestDistance)
      {
        found = true;
        best = k;
        bestDistance = distance;
      }
    }

    if (!found)
    {
      return fail(error, "could not bridge a hole into the outer ring");
    }

    std::vector<Point> merged(ring.begin(), ring.begin() + long(best) + 1);

    for (size_t i = 0; i <= hole.size(); ++i)
    {
      merged.push_back(hole[(holeIndex + i) % hole.size()]);
    }

    merged.push_back(ring[best]);
    merged.insert(merged.end(), ring.begin() + long(best) + 1, ring.end());

    ring = std::move(merged);
  }

  return true;
}

} // namespace

bool triangulate(const Polygon &polygon, std::vector<std::array<uint32_t, 3>> &triangles, std::vector<Point> &vertices, std::string *error)
{
  triangles.clear();
  vertices.clear();

  const std::string fault = polygonFault(polygon);

  if (!fault.empty())
  {
    return fail(error, "triangulation subject is not a valid polygon");
  }

  if (!bridgeHoles(polygon, vertices, error))
  {
    return false;
  }

  std::vector<uint32_t> remaining(vertices.size());

  for (uint32_t i = 0; i < vertices.size(); ++i)
  {
    remaining[i] = i;
  }

  // Ear clipping. A vertex is an ear when its corner turns the same way as the
  // ring and no other remaining vertex lies inside its triangle. Bridged
  // vertices appear twice, which is exactly what lets the clipper walk into a
  // hole and back out again.
  size_t guard = 0;

  while (remaining.size() > 3)
  {
    if (++guard > vertices.size() * vertices.size() + 16)
    {
      return fail(error, "ear clipping did not terminate");
    }

    bool clipped = false;

    for (size_t i = 0; i < remaining.size(); ++i)
    {
      const uint32_t ia = remaining[(i + remaining.size() - 1) % remaining.size()];
      const uint32_t ib = remaining[i];
      const uint32_t ic = remaining[(i + 1) % remaining.size()];

      const Point &a = vertices[ia];
      const Point &b = vertices[ib];
      const Point &c = vertices[ic];

      if (orientation(a, b, c) <= 0)
      {
        continue;
      }

      bool contains = false;

      for (const uint32_t other : remaining)
      {
        if (other == ia || other == ib || other == ic)
        {
          continue;
        }

        if (vertices[other] == a || vertices[other] == b || vertices[other] == c)
        {
          continue;
        }

        if (insideTriangle(a, b, c, vertices[other]))
        {
          contains = true;

          break;
        }
      }

      if (contains)
      {
        continue;
      }

      triangles.push_back({ia, ib, ic});
      remaining.erase(remaining.begin() + long(i));
      clipped = true;

      break;
    }

    if (!clipped)
    {
      return fail(error, "ear clipping found no ear");
    }
  }

  if (remaining.size() == 3)
  {
    triangles.push_back({remaining[0], remaining[1], remaining[2]});
  }

  return !triangles.empty();
}

bool extrude(const Prism &prism, Mesh &out, std::string *error)
{
  out = {};

  if (!std::isfinite(prism.baseHeight) || !std::isfinite(prism.topHeight))
  {
    return fail(error, "prism heights are not finite");
  }

  const size_t outerCount = prism.footprint.outer.size();

  if (!prism.baseProfile.empty() && prism.baseProfile.size() != outerCount)
  {
    return fail(error, "base profile does not match the outer ring");
  }

  if (!prism.topProfile.empty() && prism.topProfile.size() != outerCount)
  {
    return fail(error, "top profile does not match the outer ring");
  }

  std::vector<std::array<uint32_t, 3>> capTriangles;
  std::vector<Point> capVertices;

  if (!triangulate(prism.footprint, capTriangles, capVertices, error))
  {
    return false;
  }

  // Heights on the bridged cap vertices: a bridged vertex is a copy of an
  // outer or hole vertex, so its height comes from whichever outer vertex it
  // matches, and from the flat height otherwise.
  const auto heightAt = [&](const Point &p, const std::vector<double> &profile, const double flat) {
    if (profile.empty())
    {
      return flat;
    }

    for (size_t i = 0; i < outerCount; ++i)
    {
      if (prism.footprint.outer[i] == p)
      {
        return profile[i];
      }
    }

    return flat;
  };

  const uint32_t base = uint32_t(capVertices.size());

  for (const auto &p : capVertices)
  {
    out.vertices.push_back({toMetres(p.x), heightAt(p, prism.baseProfile, prism.baseHeight), toMetres(p.y)});
  }

  for (const auto &p : capVertices)
  {
    out.vertices.push_back({toMetres(p.x), heightAt(p, prism.topProfile, prism.topHeight), toMetres(p.y)});
  }

  for (const auto &t : capTriangles)
  {
    // The bottom faces down, so its winding is reversed against the top.
    out.triangles.push_back({t[0], t[2], t[1]});
    out.parts.push_back(Mesh::Part::Bottom);

    out.triangles.push_back({base + t[0], base + t[1], base + t[2]});
    out.parts.push_back(Mesh::Part::Top);
  }

  // Walls, one quad per ring edge. Holes included: the inside of a courtyard
  // is as much a wall as the outside of the building.
  const auto wall = [&](const Ring &ring, const std::vector<double> &baseProfile, const std::vector<double> &topProfile) {
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const Point &a = ring[i];
      const Point &b = ring[(i + 1) % ring.size()];

      const double aBase = heightAt(a, baseProfile, prism.baseHeight);
      const double bBase = heightAt(b, baseProfile, prism.baseHeight);
      const double aTop = heightAt(a, topProfile, prism.topHeight);
      const double bTop = heightAt(b, topProfile, prism.topHeight);

      const uint32_t index = uint32_t(out.vertices.size());

      out.vertices.push_back({toMetres(a.x), aBase, toMetres(a.y)});
      out.vertices.push_back({toMetres(b.x), bBase, toMetres(b.y)});
      out.vertices.push_back({toMetres(b.x), bTop, toMetres(b.y)});
      out.vertices.push_back({toMetres(a.x), aTop, toMetres(a.y)});

      out.triangles.push_back({index, index + 1, index + 2});
      out.parts.push_back(Mesh::Part::Wall);

      out.triangles.push_back({index, index + 2, index + 3});
      out.parts.push_back(Mesh::Part::Wall);
    }
  };

  wall(prism.footprint.outer, prism.baseProfile, prism.topProfile);

  for (const auto &hole : prism.footprint.holes)
  {
    wall(hole, {}, {});
  }

  return true;
}

bool thicken(const std::vector<Point> &path, const double halfWidthMetres, const double baseHeight, const double topHeight, Mesh &out, std::string *error)
{
  out = {};

  std::vector<Polygon> ribbon;

  if (!geom2::offsetPolyline(path, halfWidthMetres, {}, ribbon, error))
  {
    return false;
  }

  for (const auto &polygon : ribbon)
  {
    Prism prism;
    prism.footprint = polygon;
    prism.baseHeight = baseHeight;
    prism.topHeight = topHeight;

    Mesh piece;

    if (!extrude(prism, piece, error))
    {
      return false;
    }

    const uint32_t offset = uint32_t(out.vertices.size());

    out.vertices.insert(out.vertices.end(), piece.vertices.begin(), piece.vertices.end());

    for (size_t i = 0; i < piece.triangles.size(); ++i)
    {
      out.triangles.push_back({piece.triangles[i][0] + offset, piece.triangles[i][1] + offset, piece.triangles[i][2] + offset});
      out.parts.push_back(piece.parts[i]);
    }
  }

  return !out.triangles.empty();
}

} // namespace procedural::geom3
