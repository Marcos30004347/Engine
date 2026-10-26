#include "procedural/world/MapCoast.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace procedural::world {

namespace {

enum class Side : uint8_t
{
  Left = 0u,
  Right,
  Bottom,
  Top,
};

bool inside(const Point2 &point, const Side side, const double extent)
{
  switch (side)
  {
  case Side::Left:
    return point.x >= 0.0;
  case Side::Right:
    return point.x <= extent;
  case Side::Bottom:
    return point.y >= 0.0;
  default:
    return point.y <= extent;
  }
}

Point2 intersect(const Point2 &a, const Point2 &b, const Side side, const double extent)
{
  // Parameter along a->b where the edge meets the clip line. The caller only
  // calls this when the two endpoints straddle the line, so the denominators
  // below cannot be zero.
  double t = 0.0;
  switch (side)
  {
  case Side::Left:
    t = (0.0 - a.x) / (b.x - a.x);
    break;
  case Side::Right:
    t = (extent - a.x) / (b.x - a.x);
    break;
  case Side::Bottom:
    t = (0.0 - a.y) / (b.y - a.y);
    break;
  default:
    t = (extent - a.y) / (b.y - a.y);
    break;
  }
  Point2 point(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);

  // Snap the clipped coordinate exactly onto the boundary rather than trusting
  // the interpolation to land there. Algebraically a + (b - a) * t is exactly
  // the boundary value; in floating point it lands a rounding step to either
  // side, and a result a hair *outside* is re-clipped on a second pass, so the
  // operation is not idempotent and the world changes every time it runs.
  // Caught by testClipIsIdempotent.
  switch (side)
  {
  case Side::Left:
    point.x = 0.0;
    break;
  case Side::Right:
    point.x = extent;
    break;
  case Side::Bottom:
    point.y = 0.0;
    break;
  default:
    point.y = extent;
    break;
  }
  return point;
}

std::vector<Point2> clipToSide(const std::vector<Point2> &polygon, const Side side, const double extent)
{
  std::vector<Point2> result;
  if (polygon.empty())
  {
    return result;
  }
  result.reserve(polygon.size() + 4u);
  for (size_t index = 0u; index < polygon.size(); ++index)
  {
    const Point2 &current = polygon[index];
    const Point2 &previous = polygon[(index + polygon.size() - 1u) % polygon.size()];
    const bool currentIn = inside(current, side, extent);
    const bool previousIn = inside(previous, side, extent);
    if (currentIn)
    {
      if (!previousIn)
      {
        result.push_back(intersect(previous, current, side, extent));
      }
      result.push_back(current);
    }
    else if (previousIn)
    {
      result.push_back(intersect(previous, current, side, extent));
    }
  }
  return result;
}

// Quantized key so two cells' shared edge endpoints, computed independently as
// circumcentres, hash to the same vertex. Without this the chaining below never
// links anything.
struct VertexKey
{
  int64_t x = 0;
  int64_t y = 0;

  bool operator<(const VertexKey &other) const
  {
    return x != other.x ? x < other.x : y < other.y;
  }
};

VertexKey quantize(const Point2 &point, const double tolerance)
{
  return VertexKey{static_cast<int64_t>(std::llround(point.x / tolerance)), static_cast<int64_t>(std::llround(point.y / tolerance))};
}

} // namespace

std::vector<Point2> clipPolygonToConvex(const std::vector<Point2> &subject, const std::vector<Point2> &window)
{
  if (subject.size() < 3u || window.size() < 3u)
  {
    return {};
  }

  std::vector<Point2> output = subject;
  std::vector<Point2> input;
  for (size_t edge = 0u; edge < window.size() && !output.empty(); ++edge)
  {
    const Point2 &a = window[edge];
    const Point2 &b = window[(edge + 1u) % window.size()];
    const double ex = b.x - a.x;
    const double ey = b.y - a.y;
    // Positive is inside for a counter-clockwise window.
    const auto side = [&](const Point2 &point) {
      return ex * (point.y - a.y) - ey * (point.x - a.x);
    };

    input.swap(output);
    output.clear();
    for (size_t index = 0u; index < input.size(); ++index)
    {
      const Point2 &current = input[index];
      const Point2 &next = input[(index + 1u) % input.size()];
      const double currentSide = side(current);
      const double nextSide = side(next);
      const auto crossing = [&]() {
        const double span = currentSide - nextSide;
        const double t = span == 0.0 ? 0.0 : currentSide / span;
        // The crossing is snapped onto the window edge rather than left where
        // the interpolation lands. Algebraically they are the same point; in
        // floating point the interpolated one falls a rounding step to either
        // side, and a vertex a hair outside is re-clipped on the next pass. The
        // same defect, and the same fix, as clipCellsToExtent.
        const double px = current.x + (next.x - current.x) * t;
        const double py = current.y + (next.y - current.y) * t;
        const double length = ex * ex + ey * ey;
        if (length <= 0.0)
        {
          return Point2(px, py);
        }
        const double along = ((px - a.x) * ex + (py - a.y) * ey) / length;
        return Point2(a.x + ex * along, a.y + ey * along);
      };
      if (currentSide >= 0.0)
      {
        output.push_back(current);
        if (nextSide < 0.0)
        {
          output.push_back(crossing());
        }
      }
      else if (nextSide >= 0.0)
      {
        output.push_back(crossing());
      }
    }
  }
  return output.size() >= 3u ? output : std::vector<Point2>{};
}

bool clipCellsToExtent(MapGraph &graph, const double extentMetres, std::string *error)
{
  if (!(extentMetres > 0.0))
  {
    if (error != nullptr)
    {
      *error = "clip extent must be positive";
    }
    return false;
  }

  for (size_t index = 0u; index < graph.nodeCount(); ++index)
  {
    MapNode &node = graph.node(static_cast<uint32_t>(index));
    if (node.area.size() < 3u)
    {
      continue;
    }
    std::vector<Point2> polygon = node.area;
    for (const Side side : {Side::Left, Side::Right, Side::Bottom, Side::Top})
    {
      polygon = clipToSide(polygon, side, extentMetres);
      if (polygon.empty())
      {
        break;
      }
    }
    // A cell entirely outside the world clips to nothing. Keeping the original
    // polygon would reintroduce the very overhang this pass exists to remove,
    // so it is left degenerate and validate() reports it.
    if (polygon.size() >= 3u)
    {
      node.area = std::move(polygon);
    }
  }
  return true;
}

std::vector<CoastPolyline> extractCoastline(const MapGraph &graph, const double toleranceMetres)
{
  std::vector<CoastPolyline> coastlines;
  if (graph.nodeCount() == 0u || !(toleranceMetres > 0.0))
  {
    return coastlines;
  }

  // Collect the segments where a land cell meets a water cell. A shared Voronoi
  // edge appears in both cells' polygons, so the segment is found by matching
  // consecutive vertex pairs between the two.
  std::vector<std::pair<Point2, Point2>> segments;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.land || node.area.size() < 3u)
    {
      continue;
    }
    for (const uint32_t neighbourId : node.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.land || neighbour.area.size() < 3u)
      {
        continue;
      }
      // Only emit each boundary once: the land side owns it.
      std::map<VertexKey, Point2> neighbourVertices;
      for (const Point2 &vertex : neighbour.area)
      {
        neighbourVertices.emplace(quantize(vertex, toleranceMetres), vertex);
      }

      for (size_t index = 0u; index < node.area.size(); ++index)
      {
        const Point2 &a = node.area[index];
        const Point2 &b = node.area[(index + 1u) % node.area.size()];
        if (neighbourVertices.count(quantize(a, toleranceMetres)) != 0u && neighbourVertices.count(quantize(b, toleranceMetres)) != 0u)
        {
          segments.emplace_back(a, b);
        }
      }
    }
  }
  if (segments.empty())
  {
    return coastlines;
  }

  // Chain segments end to end. A vertex may join more than two segments where
  // several cells meet, so the walk takes the first unused continuation rather
  // than assuming a manifold curve.
  std::multimap<VertexKey, size_t> byStart;
  for (size_t index = 0u; index < segments.size(); ++index)
  {
    byStart.emplace(quantize(segments[index].first, toleranceMetres), index);
  }

  std::vector<bool> used(segments.size(), false);
  for (size_t index = 0u; index < segments.size(); ++index)
  {
    if (used[index])
    {
      continue;
    }
    used[index] = true;
    CoastPolyline line;
    line.points.push_back(segments[index].first);
    line.points.push_back(segments[index].second);

    Point2 tail = segments[index].second;
    while (true)
    {
      const VertexKey key = quantize(tail, toleranceMetres);
      auto range = byStart.equal_range(key);
      size_t next = segments.size();
      for (auto iterator = range.first; iterator != range.second; ++iterator)
      {
        if (!used[iterator->second])
        {
          next = iterator->second;
          break;
        }
      }
      if (next == segments.size())
      {
        break;
      }
      used[next] = true;
      tail = segments[next].second;
      line.points.push_back(tail);
      if (quantize(tail, toleranceMetres) < quantize(line.points.front(), toleranceMetres) || quantize(line.points.front(), toleranceMetres) < quantize(tail, toleranceMetres))
      {
        continue;
      }
      line.closed = true;
      break;
    }
    coastlines.push_back(std::move(line));
  }
  return coastlines;
}

} // namespace procedural::world
