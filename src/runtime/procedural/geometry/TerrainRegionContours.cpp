#include "procedural/geometry/TerrainRegionContours.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <stdexcept>

namespace procedural::geometry
{
namespace
{
// Douglas-Peucker on a closed ring. Marching squares emits one vertex per cell
// crossing; the shape carries far fewer than that, and every vertex kept is a
// constraint the triangulator has to honour.
void simplifyInto(const std::vector<Point2> &points, const size_t first, const size_t last,
    const double tolerance, std::vector<uint8_t> &keep)
{
  if (last <= first + 1u) return;
  const Point2 &a = points[first], &b = points[last];
  const double dx = b.x - a.x, dy = b.y - a.y;
  const double span = std::hypot(dx, dy);
  double worst = -1; size_t at = first;
  for (size_t i = first + 1u; i < last; ++i)
  {
    double distance;
    if (span < 1e-9) distance = std::hypot(points[i].x - a.x, points[i].y - a.y);
    else distance = std::abs((points[i].x - a.x) * dy - (points[i].y - a.y) * dx) / span;
    if (distance > worst) { worst = distance; at = i; }
  }
  if (worst <= tolerance) return;
  keep[at] = 1u;
  simplifyInto(points, first, at, tolerance, keep);
  simplifyInto(points, at, last, tolerance, keep);
}
std::vector<Point2> simplifyRing(const std::vector<Point2> &ring, const double tolerance)
{
  if (ring.size() < 4u || !(tolerance > 0)) return ring;
  std::vector<uint8_t> keep(ring.size(), 0u);
  keep.front() = keep.back() = 1u;
  // Split at the point farthest from the first, so a closed ring is not
  // collapsed by having its two ends coincide.
  size_t opposite = 0; double furthest = -1;
  for (size_t i = 1; i < ring.size(); ++i)
  {
    const double d = std::hypot(ring[i].x - ring[0].x, ring[i].y - ring[0].y);
    if (d > furthest) { furthest = d; opposite = i; }
  }
  keep[opposite] = 1u;
  simplifyInto(ring, 0, opposite, tolerance, keep);
  simplifyInto(ring, opposite, ring.size() - 1u, tolerance, keep);
  std::vector<Point2> out;
  for (size_t i = 0; i < ring.size(); ++i) if (keep[i]) out.push_back(ring[i]);
  return out.size() >= 3u ? out : ring;
}
// Chord smoothing. A marching-squares ring is axis-aligned steps; without this
// a "coastline" is a staircase whatever its resolution.
void smoothRing(std::vector<Point2> &ring, const uint32_t rounds, const double amount)
{
  if (ring.size() < 4u || !(amount > 0)) return;
  for (uint32_t round = 0; round < rounds; ++round)
  {
    std::vector<Point2> next = ring;
    for (size_t i = 0, n = ring.size(); i < n; ++i)
    {
      const Point2 &a = ring[(i + n - 1u) % n], &b = ring[(i + 1u) % n];
      next[i] = {ring[i].x + ((a.x + b.x) * .5 - ring[i].x) * amount,
          ring[i].y + ((a.y + b.y) * .5 - ring[i].y) * amount};
    }
    ring.swap(next);
  }
}
double perimeterOf(const std::vector<Point2> &ring)
{
  double total = 0;
  for (size_t i = 0, n = ring.size(); i < n; ++i)
    total += std::hypot(ring[(i + 1u) % n].x - ring[i].x, ring[(i + 1u) % n].y - ring[i].y);
  return total;
}

// Marching squares, with the crossing interpolated along the grid edge.
//
// The crossing on an edge between corner values `va` and `vb` sits at
// `(level - va) / (vb - va)` along it. Both cells that share the edge compute it
// from the same two corner values, so the result is bit-identical from either
// side - which is why keying the chain by *edge identity* rather than by
// position needs no tolerance at all.
//
// Without this the crossings sit at fixed edge midpoints and the contour is
// quantised to a half-cell lattice: a coastline made of 62 m steps that the
// smoothing pass then has to disguise. With it the contour follows the actual
// iso-value and the smoothing only has to take the corners off.
//
// A categorical field - a biome index - has no meaningful interpolation, so it
// passes `interpolate = false` and falls back to midpoints, which is correct
// there: the boundary genuinely is "somewhere between these two cells".
struct EdgeKey
{
  int64_t x, y; uint8_t axis;
  bool operator<(const EdgeKey &o) const
  { return std::tie(x, y, axis) < std::tie(o.x, o.y, o.axis); }
  bool operator==(const EdgeKey &o) const { return x == o.x && y == o.y && axis == o.axis; }
};
std::vector<std::vector<Point2>> marchSquares(const std::vector<double> &field, const double level,
    const uint32_t side, const double cell, const Point2 origin, const bool interpolate)
{
  const auto value = [&](const int64_t x, const int64_t y)
  {
    if (x < 0 || y < 0 || x >= int64_t(side) || y >= int64_t(side))
      return level - 1.0;                       // outside is always "out"
    return field[size_t(y) * side + size_t(x)];
  };
  const auto inside = [&](const int64_t x, const int64_t y) { return value(x, y) >= level; };
  const auto centre = [&](const int64_t x, const int64_t y)
  { return Point2(origin.x + (double(x) + .5) * cell, origin.y + (double(y) + .5) * cell); };
  // The crossing on the edge joining two grid vertices, and its identity.
  const auto crossing = [&](const int64_t ax, const int64_t ay, const int64_t bx, const int64_t by)
  {
    const Point2 a = centre(ax, ay), b = centre(bx, by);
    double t = .5;
    if (interpolate)
    {
      const double va = value(ax, ay), vb = value(bx, by);
      if (std::abs(vb - va) > 1e-12) t = std::clamp((level - va) / (vb - va), 0.0, 1.0);
    }
    return Point2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
  };
  const auto keyOf = [&](const int64_t ax, const int64_t ay, const int64_t bx, const int64_t by)
  { return EdgeKey{std::min(ax, bx), std::min(ay, by), uint8_t(ax == bx ? 1u : 0u)}; };

  std::map<EdgeKey, std::vector<EdgeKey>> outgoing;
  std::map<EdgeKey, Point2> where;
  std::vector<std::pair<EdgeKey, EdgeKey>> segments;
  for (int64_t y = -1; y < int64_t(side); ++y)
    for (int64_t x = -1; x < int64_t(side); ++x)
    {
      const bool a = inside(x, y), b = inside(x + 1, y), c = inside(x + 1, y + 1), d = inside(x, y + 1);
      const uint32_t mask = uint32_t(a) | (uint32_t(b) << 1) | (uint32_t(c) << 2) | (uint32_t(d) << 3);
      if (mask == 0u || mask == 15u) continue;
      const EdgeKey lowerKey = keyOf(x, y, x + 1, y);
      const EdgeKey rightKey = keyOf(x + 1, y, x + 1, y + 1);
      const EdgeKey upperKey = keyOf(x, y + 1, x + 1, y + 1);
      const EdgeKey leftKey = keyOf(x, y, x, y + 1);
      where[lowerKey] = crossing(x, y, x + 1, y);
      where[rightKey] = crossing(x + 1, y, x + 1, y + 1);
      where[upperKey] = crossing(x, y + 1, x + 1, y + 1);
      where[leftKey] = crossing(x, y, x, y + 1);
      const auto emit = [&](const EdgeKey &from, const EdgeKey &to) { segments.emplace_back(from, to); };
      switch (mask)
      {
      case 1:  emit(leftKey, lowerKey); break;
      case 2:  emit(lowerKey, rightKey); break;
      case 3:  emit(leftKey, rightKey); break;
      case 4:  emit(rightKey, upperKey); break;
      case 5:  emit(leftKey, upperKey); emit(rightKey, lowerKey); break;   // saddle
      case 6:  emit(lowerKey, upperKey); break;
      case 7:  emit(leftKey, upperKey); break;
      case 8:  emit(upperKey, leftKey); break;
      case 9:  emit(upperKey, lowerKey); break;
      case 10: emit(upperKey, rightKey); emit(lowerKey, leftKey); break;   // saddle
      case 11: emit(upperKey, rightKey); break;
      case 12: emit(rightKey, leftKey); break;
      case 13: emit(rightKey, lowerKey); break;
      case 14: emit(lowerKey, leftKey); break;
      default: break;
      }
    }
  for (const auto &[from, to] : segments) outgoing[from].push_back(to);
  std::map<EdgeKey, size_t> used;
  std::vector<std::vector<Point2>> rings;
  for (const auto &[from, to] : segments)
  {
    if (used[from] >= outgoing[from].size()) continue;
    std::vector<Point2> ring;
    EdgeKey walk = from;
    for (size_t guard = 0; guard <= segments.size(); ++guard)
    {
      const auto found = outgoing.find(walk);
      if (found == outgoing.end() || used[walk] >= found->second.size()) break;
      ring.push_back(where[walk]);
      const EdgeKey next = found->second[used[walk]++];
      if (next == from) break;
      walk = next;
    }
    if (ring.size() >= 4u) rings.push_back(std::move(ring));
  }
  return rings;
}
}

std::vector<std::vector<Point2>> extractRegionContours(const double extentMetres,
    const std::function<bool(Point2)> &inside, const RegionContourSettings &settings)
{
  if (!(extentMetres > 0)) throw std::invalid_argument("invalid contour domain");
  if (!inside) throw std::invalid_argument("contour extraction needs a predicate");
  const uint32_t side = std::max(4u, settings.resolution);
  const double cell = extentMetres / side;
  std::vector<double> mask(size_t(side) * side, 0.0);
  for (uint32_t y = 0; y < side; ++y)
    for (uint32_t x = 0; x < side; ++x)
      mask[size_t(y) * side + x] = inside({(x + .5) * cell, (y + .5) * cell}) ? 1.0 : 0.0;
  auto rings = marchSquares(mask, .5, side, cell, {0, 0}, false);
  std::vector<std::vector<Point2>> out;
  for (auto &ring : rings)
  {
    ring = simplifyRing(ring, settings.simplifyMetres);
    smoothRing(ring, settings.smoothingRounds, settings.smoothing);
    if (ring.size() >= 3u && perimeterOf(ring) >= settings.minimumPerimeterMetres)
      out.push_back(std::move(ring));
  }
  return out;
}

std::vector<std::vector<Point2>> extractIsoContours(const double extentMetres,
    const std::function<double(Point2)> &field, const double level,
    const RegionContourSettings &settings)
{
  if (!(extentMetres > 0)) throw std::invalid_argument("invalid contour domain");
  if (!field) throw std::invalid_argument("iso contours need a field");
  const uint32_t side = std::max(4u, settings.resolution);
  const double cell = extentMetres / side;
  std::vector<double> samples(size_t(side) * side, 0.0);
  for (uint32_t y = 0; y < side; ++y)
    for (uint32_t x = 0; x < side; ++x)
      samples[size_t(y) * side + x] = field({(x + .5) * cell, (y + .5) * cell});
  std::vector<std::vector<Point2>> out;
  for (auto &ring : marchSquares(samples, level, side, cell, {0, 0}, true))
  {
    ring = simplifyRing(ring, settings.simplifyMetres);
    smoothRing(ring, settings.smoothingRounds, settings.smoothing);
    if (ring.size() >= 3u && perimeterOf(ring) >= settings.minimumPerimeterMetres)
      out.push_back(std::move(ring));
  }
  return out;
}

std::vector<std::vector<Point2>> extractLabelContours(const double extentMetres,
    const std::function<uint8_t(Point2)> &label, const RegionContourSettings &settings)
{
  if (!label) throw std::invalid_argument("contour extraction needs a label function");
  const uint32_t side = std::max(4u, settings.resolution);
  const double cell = extentMetres / side;
  std::vector<uint8_t> labels(size_t(side) * side, 0u);
  for (uint32_t y = 0; y < side; ++y)
    for (uint32_t x = 0; x < side; ++x)
      labels[size_t(y) * side + x] = label({(x + .5) * cell, (y + .5) * cell});
  // One pass per distinct value, taking that value's own region boundary. The
  // shared edge between two regions is then produced twice, identically, which
  // the triangulator deduplicates - and which is what guarantees the two sides
  // agree rather than nearly agreeing.
  std::vector<uint8_t> present;
  for (const uint8_t v : labels)
    if (std::find(present.begin(), present.end(), v) == present.end()) present.push_back(v);
  std::sort(present.begin(), present.end());
  std::vector<std::vector<Point2>> out;
  std::vector<double> mask(labels.size(), 0.0);
  for (const uint8_t value : present)
  {
    for (size_t i = 0; i < labels.size(); ++i) mask[i] = labels[i] == value ? 1.0 : 0.0;
    // Categorical: no interpolation, because "half a biome" means nothing.
    for (auto &ring : marchSquares(mask, .5, side, cell, {0, 0}, false))
    {
      ring = simplifyRing(ring, settings.simplifyMetres);
      smoothRing(ring, settings.smoothingRounds, settings.smoothing);
      if (ring.size() >= 3u && perimeterOf(ring) >= settings.minimumPerimeterMetres)
        out.push_back(std::move(ring));
    }
  }
  return out;
}
}
