#include "procedural/world/MapContours.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace procedural::world {

namespace {

struct Segment
{
  Point2 a;
  Point2 b;
  uint64_t keyA = 0u;
  uint64_t keyB = 0u;
};

struct ScalarVertex
{
  Point2 position;
  double elevationMetres = 0.0;
  uint64_t id = 0u;
};

uint64_t edgeKey(const uint64_t a, const uint64_t b)
{
  return (std::min(a, b) << 32u) | std::max(a, b);
}

double comparisonElevation(const ScalarVertex &vertex, const double intervalMetres)
{
  // A contour exactly through a graph vertex is topologically ambiguous. A
  // stable, microscopic symbolic perturbation makes every triangle choose the
  // same side for that vertex without visibly moving the intersection.
  const double sign = (vertex.id & 1u) == 0u ? -1.0 : 1.0;
  return vertex.elevationMetres + sign * intervalMetres * 1.0e-10;
}

Point2 crossing(const ScalarVertex &first, const ScalarVertex &second, const double elevation)
{
  // Always interpolate in vertex-id order. Adjacent triangles then compute the
  // shared-edge crossing with identical floating-point operations.
  const ScalarVertex *a = &first;
  const ScalarVertex *b = &second;
  if (a->id > b->id)
  {
    std::swap(a, b);
  }
  const double span = b->elevationMetres - a->elevationMetres;
  const double t = span == 0.0 ? 0.5 : std::clamp((elevation - a->elevationMetres) / span, 0.0, 1.0);
  return Point2(a->position.x + (b->position.x - a->position.x) * t, a->position.y + (b->position.y - a->position.y) * t);
}

void contourTriangle(const std::array<ScalarVertex, 3> &triangle, const double elevation, const double interval, const double tolerance, std::vector<Segment> &segments)
{
  std::array<Point2, 3> points{};
  std::array<uint64_t, 3> keys{};
  uint32_t count = 0u;
  for (uint32_t edge = 0u; edge < 3u; ++edge)
  {
    const ScalarVertex &a = triangle[edge];
    const ScalarVertex &b = triangle[(edge + 1u) % 3u];
    const double ea = comparisonElevation(a, interval);
    const double eb = comparisonElevation(b, interval);
    if ((ea < elevation) == (eb < elevation))
    {
      continue;
    }
    if (count < points.size())
    {
      points[count] = crossing(a, b, elevation);
      keys[count] = edgeKey(a.id, b.id);
      ++count;
    }
  }
  if (count != 2u)
  {
    return;
  }
  const double dx = points[1].x - points[0].x;
  const double dy = points[1].y - points[0].y;
  if (dx * dx + dy * dy <= tolerance * tolerance)
  {
    return;
  }
  segments.push_back(Segment{points[0], points[1], keys[0], keys[1]});
}

std::vector<ContourPolyline> joinSegments(const std::vector<Segment> &segments, const double elevation, const bool major)
{
  struct WeldedSegment
  {
    uint32_t a = 0u;
    uint32_t b = 0u;
  };

  // Join by the identity of the triangulation edge a crossing lies on, not by
  // rounded coordinates. This matters when a contour passes exactly through a
  // scalar vertex: several different edges then meet at the same position, but
  // the symbolic perturbation chooses distinct topological connections. A
  // positional weld collapses them back into a degree-4 branch and opens loops.
  std::vector<Point2> vertices;
  std::unordered_map<uint64_t, uint32_t> vertexOfEdge;
  const auto vertexFor = [&](const uint64_t key, const Point2 &point) {
    const auto found = vertexOfEdge.find(key);
    if (found != vertexOfEdge.end())
    {
      return found->second;
    }
    const uint32_t vertex = static_cast<uint32_t>(vertices.size());
    vertices.push_back(point);
    vertexOfEdge.emplace(key, vertex);
    return vertex;
  };

  std::vector<WeldedSegment> welded;
  welded.reserve(segments.size());
  std::unordered_set<uint64_t> segmentKeys;
  for (const Segment &segment : segments)
  {
    const uint32_t a = vertexFor(segment.keyA, segment.a);
    const uint32_t b = vertexFor(segment.keyB, segment.b);
    if (a == b)
    {
      continue;
    }
    const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32u) | std::max(a, b);
    if (segmentKeys.insert(key).second)
    {
      welded.push_back(WeldedSegment{a, b});
    }
  }

  std::vector<std::vector<uint32_t>> incident(vertices.size());
  for (uint32_t index = 0u; index < welded.size(); ++index)
  {
    incident[welded[index].a].push_back(index);
    incident[welded[index].b].push_back(index);
  }

  std::vector<uint8_t> used(welded.size(), 0u);
  std::vector<ContourPolyline> result;
  const auto walk = [&](const uint32_t seed, const uint32_t start, std::vector<uint8_t> &walked) {
    ContourPolyline line{};
    line.elevationMetres = elevation;
    line.major = major;
    uint32_t current = seed;
    uint32_t at = start;
    for (;;)
    {
      if (walked[current] != 0u)
      {
        break;
      }
      walked[current] = 1u;
      const WeldedSegment &segment = welded[current];
      const bool forward = segment.a == at;
      if (line.points.empty())
      {
        line.points.push_back(vertices[at]);
      }
      const uint32_t nextVertex = forward ? segment.b : segment.a;
      line.points.push_back(vertices[nextVertex]);
      if (nextVertex == start)
      {
        line.closed = true;
        // Closed is metadata; keeping a duplicate endpoint makes downstream
        // line renderers draw a zero-length final segment.
        line.points.pop_back();
        break;
      }
      uint32_t next = static_cast<uint32_t>(welded.size());
      for (const uint32_t candidate : incident[nextVertex])
      {
        if (walked[candidate] == 0u)
        {
          next = candidate;
          break;
        }
      }
      if (next == welded.size())
      {
        break;
      }
      at = nextVertex;
      current = next;
    }
    return line;
  };

  // Open contours first. This prevents a chain starting in its middle merely
  // because its segment happened to be emitted first.
  for (uint32_t index = 0u; index < welded.size(); ++index)
  {
    if (used[index] != 0u)
    {
      continue;
    }
    const WeldedSegment &segment = welded[index];
    const size_t degreeA = incident[segment.a].size();
    const size_t degreeB = incident[segment.b].size();
    if (degreeA == 2u && degreeB == 2u)
    {
      continue;
    }
    const uint32_t start = degreeA != 2u ? segment.a : segment.b;
    ContourPolyline line = walk(index, start, used);
    if (line.points.size() >= 2u)
    {
      result.push_back(std::move(line));
    }
  }
  // What remains consists of closed loops (apart from pathological saddles,
  // which the symbolic perturbation above removes).
  for (uint32_t index = 0u; index < welded.size(); ++index)
  {
    if (used[index] == 0u)
    {
      ContourPolyline line = walk(index, welded[index].a, used);
      if (line.points.size() >= 2u)
      {
        result.push_back(std::move(line));
      }
    }
  }
  return result;
}

Point2 catmullRom(const Point2 &p0, const Point2 &p1, const Point2 &p2, const Point2 &p3, const double t, const double tension)
{
  // Centripetal parameterisation avoids the loops and cusps the uniform form
  // creates where marching-triangle segments have very different lengths.
  const auto knot = [tension](const double previous, const Point2 &a, const Point2 &b) {
    return previous + std::pow(std::hypot(b.x - a.x, b.y - a.y), tension);
  };
  const double t0 = 0.0;
  const double t1 = knot(t0, p0, p1);
  const double t2 = knot(t1, p1, p2);
  const double t3 = knot(t2, p2, p3);
  if (!(t0 < t1) || !(t1 < t2) || !(t2 < t3))
  {
    return Point2(p1.x + (p2.x - p1.x) * t, p1.y + (p2.y - p1.y) * t);
  }

  const double at = t1 + (t2 - t1) * t;
  const auto mix = [](const Point2 &a, const Point2 &b, const double from, const double to, const double value) {
    const double span = to - from;
    if (!(span > 0.0))
    {
      return a;
    }
    const double first = (to - value) / span;
    const double second = (value - from) / span;
    return Point2(a.x * first + b.x * second, a.y * first + b.y * second);
  };
  const Point2 a1 = mix(p0, p1, t0, t1, at);
  const Point2 a2 = mix(p1, p2, t1, t2, at);
  const Point2 a3 = mix(p2, p3, t2, t3, at);
  const Point2 b1 = mix(a1, a2, t0, t2, at);
  const Point2 b2 = mix(a2, a3, t1, t3, at);
  return mix(b1, b2, t1, t2, at);
}

void smoothContour(ContourPolyline &line, const uint32_t samplesPerSpan, const double tension)
{
  if (samplesPerSpan <= 1u || line.points.size() < 3u)
  {
    return;
  }
  const std::vector<Point2> source = line.points;
  const int64_t count = static_cast<int64_t>(source.size());
  const auto pointAt = [&](const int64_t index) {
    if (line.closed)
    {
      const int64_t wrapped = (index % count + count) % count;
      return source[static_cast<size_t>(wrapped)];
    }
    if (index < 0)
    {
      return Point2(source[0].x * 2.0 - source[1].x, source[0].y * 2.0 - source[1].y);
    }
    if (index >= count)
    {
      return Point2(source.back().x * 2.0 - source[source.size() - 2u].x, source.back().y * 2.0 - source[source.size() - 2u].y);
    }
    return source[static_cast<size_t>(index)];
  };

  const size_t spans = line.closed ? source.size() : source.size() - 1u;
  std::vector<Point2> curved;
  curved.reserve(spans * samplesPerSpan + (line.closed ? 0u : 1u));
  for (size_t span = 0u; span < spans; ++span)
  {
    const int64_t point = static_cast<int64_t>(span);
    const Point2 p0 = pointAt(point - 1);
    const Point2 p1 = pointAt(point);
    const Point2 p2 = pointAt(point + 1);
    const Point2 p3 = pointAt(point + 2);
    for (uint32_t sample = 0u; sample < samplesPerSpan; ++sample)
    {
      curved.push_back(catmullRom(p0, p1, p2, p3, static_cast<double>(sample) / static_cast<double>(samplesPerSpan), tension));
    }
  }
  if (!line.closed)
  {
    curved.push_back(source.back());
  }
  line.points = std::move(curved);
}

} // namespace

size_t ContourSet::pointCount() const
{
  size_t count = 0u;
  for (const ContourPolyline &line : lines)
  {
    count += line.points.size();
  }
  return count;
}

bool extractElevationContours(const MapGraph &graph, const ContourSettings &settings, ContourSet &contours, std::string *error)
{
  SiteIndex index;
  std::string localError;
  if (!index.build(graph, 0u, graph.brief().extentMetres, &localError))
  {
    if (error != nullptr)
    {
      *error = localError;
    }
    contours = {};
    return false;
  }
  MapSurfaceField surface;
  if (!surface.build(graph, index, BlendSettings{}, MapSurfaceSettings{}, &localError))
  {
    if (error != nullptr)
    {
      *error = localError;
    }
    contours = {};
    return false;
  }
  return extractElevationContours(surface, settings, contours, error);
}

bool extractElevationContours(const MapSurfaceField &surface, const ContourSettings &settings, ContourSet &contours, std::string *error)
{
  contours = {};
  const auto fail = [error](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }
    return false;
  };
  if (!(settings.intervalMetres > 0.0) || !std::isfinite(settings.intervalMetres))
  {
    return fail("contour interval must be finite and positive");
  }
  if (!std::isfinite(settings.minimumElevationMetres))
  {
    return fail("contour minimum must be finite");
  }
  if (settings.maximumElevationMetres < settings.minimumElevationMetres)
  {
    return fail("contour maximum must not be below its minimum");
  }
  if (!std::isfinite(settings.curveTension) || settings.curveTension < 0.0 || settings.curveTension > 1.0)
  {
    return fail("contour curve tension must be between zero and one");
  }
  contours.intervalMetres = settings.intervalMetres;
  if (surface.empty())
  {
    return true;
  }

  double lowest = std::numeric_limits<double>::infinity();
  double highest = -std::numeric_limits<double>::infinity();
  for (uint32_t y = 0u; y < surface.gridSide(); ++y)
  {
    for (uint32_t x = 0u; x < surface.gridSide(); ++x)
    {
      const double elevation = surface.at(x, y).elevationMetres;
      lowest = std::min(lowest, elevation);
      highest = std::max(highest, elevation);
    }
  }
  if (!std::isfinite(lowest) || !std::isfinite(highest))
  {
    return fail("graph has no finite leaf elevations");
  }

  const double extent = surface.extentMetres();
  const double tolerance = settings.minimumSegmentMetres > 0.0 ? settings.minimumSegmentMetres : std::clamp(extent * 1.0e-6, 1.0e-3, 10.0);
  // The shared surface is already a conforming triangle grid. March those exact
  // triangles: no hierarchy T-junctions, no second interpolation scheme, and
  // no renderer/contour disagreement about the scalar under a crossing.
  std::vector<ScalarVertex> scalarVertices;
  const uint32_t side = surface.gridSide();
  scalarVertices.reserve(static_cast<size_t>(side) * side);
  const double boundaryElevation = std::min(lowest, settings.minimumElevationMetres) - settings.intervalMetres;
  for (uint32_t y = 0u; y < side; ++y)
  {
    for (uint32_t x = 0u; x < side; ++x)
    {
      const uint64_t id = static_cast<uint64_t>(y) * side + x;
      const bool boundary = x == 0u || y == 0u || x + 1u == side || y + 1u == side;
      scalarVertices.push_back(
          ScalarVertex{Point2(static_cast<double>(x) * surface.spacingMetres(), static_cast<double>(y) * surface.spacingMetres()), boundary ? boundaryElevation : surface.at(x, y).elevationMetres, id});
    }
  }
  const bool boundedDomain = std::isfinite(extent) && extent > 0.0;
  std::vector<std::array<ScalarVertex, 3>> triangles;
  triangles.reserve(static_cast<size_t>(side - 1u) * (side - 1u) * 2u);
  for (uint32_t y = 0u; y + 1u < side; ++y)
  {
    for (uint32_t x = 0u; x + 1u < side; ++x)
    {
      const size_t a = static_cast<size_t>(y) * side + x;
      const size_t b = a + 1u;
      const size_t d = static_cast<size_t>(y + 1u) * side + x;
      const size_t c = d + 1u;
      triangles.push_back({scalarVertices[a], scalarVertices[b], scalarVertices[c]});
      triangles.push_back({scalarVertices[a], scalarVertices[c], scalarVertices[d]});
    }
  }

  double first = std::ceil(std::max(lowest, settings.minimumElevationMetres) / settings.intervalMetres) * settings.intervalMetres;
  if (!settings.includeMinimum && first <= settings.minimumElevationMetres + settings.intervalMetres * 1e-9)
  {
    first += settings.intervalMetres;
  }
  const double last = std::min(highest, settings.maximumElevationMetres);
  for (double elevation = first; elevation <= last + settings.intervalMetres * 1e-9; elevation += settings.intervalMetres)
  {
    std::vector<Segment> segments;
    for (const std::array<ScalarVertex, 3> &triangle : triangles)
    {
      contourTriangle(triangle, elevation, settings.intervalMetres, tolerance, segments);
    }
    const int64_t ordinal = static_cast<int64_t>(std::llround(elevation / settings.intervalMetres));
    const bool major = settings.majorEvery > 0u && std::abs(ordinal) % static_cast<int64_t>(settings.majorEvery) == 0;
    std::vector<ContourPolyline> lines = joinSegments(segments, elevation, major);
    if (boundedDomain)
    {
      lines.erase(
          std::remove_if(
              lines.begin(),
              lines.end(),
              [](const ContourPolyline &line) {
                return line.points.size() < 3u;
              }),
          lines.end());
    }
    for (ContourPolyline &line : lines)
    {
      // The four below-range corner samples define a closed scalar domain, so
      // an isoline cannot legitimately leave it. A few hull triangles can be
      // omitted by the incremental triangulator when sites are nearly
      // collinear with the rectangular boundary; the resulting chain still
      // has exactly two ends. Seal that missing hull span before smoothing so
      // the public contour graph preserves the domain's closed-loop topology.
      if (boundedDomain && !line.closed)
      {
        line.closed = true;
      }
      smoothContour(line, settings.curveSamplesPerSpan, settings.curveTension);
    }
    contours.lines.insert(contours.lines.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
  }
  return true;
}

} // namespace procedural::world
