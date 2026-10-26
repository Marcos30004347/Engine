#include "procedural/world/MapEdges.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace procedural::world
{

namespace
{

uint64_t hashOf(uint64_t value)
{
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;
  return value;
}

double latticeUnit(const uint64_t seed, const int64_t x, const int64_t y)
{
  const uint64_t value = hashOf(seed ^ (static_cast<uint64_t>(x) * 0x9e3779b97f4a7c15ull) ^ (static_cast<uint64_t>(y) * 0xbf58476d1ce4e5b9ull));
  return static_cast<double>(value >> 11u) / 9007199254740992.0;
}

double smoothCurve(const double t)
{
  const double c = std::clamp(t, 0.0, 1.0);
  return c * c * (3.0 - 2.0 * c);
}

double fieldAt(const uint64_t seed, const Point2 &point, const double frequency)
{
  const double x = point.x * frequency;
  const double y = point.y * frequency;
  const double fx = std::floor(x);
  const double fy = std::floor(y);
  const int64_t ix = static_cast<int64_t>(fx);
  const int64_t iy = static_cast<int64_t>(fy);
  const double tx = smoothCurve(x - fx);
  const double ty = smoothCurve(y - fy);
  const double v00 = latticeUnit(seed, ix, iy);
  const double v10 = latticeUnit(seed, ix + 1, iy);
  const double v01 = latticeUnit(seed, ix, iy + 1);
  const double v11 = latticeUnit(seed, ix + 1, iy + 1);
  const double top = v00 + (v10 - v00) * tx;
  const double bottom = v01 + (v11 - v01) * tx;
  return top + (bottom - top) * ty;
}

// Fractal sum of the field, in [-1, 1].
double fractalField(const uint64_t seed, const Point2 &point, const double frequency, const uint32_t octaves, const double persistence)
{
  double sum = 0.0;
  double total = 0.0;
  double weight = 1.0;
  double scale = frequency;
  for (uint32_t octave = 0u; octave < std::max(1u, octaves); ++octave)
  {
    sum += (fieldAt(seed ^ (octave * 7919u), point, scale) - 0.5) * 2.0 * weight;
    total += weight;
    weight *= persistence;
    scale *= 2.0;
  }
  return total > 0.0 ? sum / total : 0.0;
}

int64_t quantise(const double value, const double tolerance)
{
  return static_cast<int64_t>(std::llround(value / tolerance));
}

} // namespace

MapEdgeSet buildEdgeSet(const MapGraph &graph, const double weldToleranceMetres)
{
  MapEdgeSet set{};
  const double tolerance = weldToleranceMetres > 0.0 ? weldToleranceMetres : 1.0;
  set.loops.assign(graph.nodeCount(), {});

  // Weld vertices. Two cells' copies of a shared corner differ by rounding, so
  // they are the same vertex and have to be recognised as one - everything else
  // here depends on it.
  std::unordered_map<uint64_t, uint32_t> vertexOf;
  const auto vertexIndex = [&](const Point2 &point)
  {
    const int64_t x = quantise(point.x, tolerance);
    const int64_t y = quantise(point.y, tolerance);
    const uint64_t key = (static_cast<uint64_t>(x) << 32u) ^ (static_cast<uint64_t>(y) & 0xffffffffull);
    const auto found = vertexOf.find(key);
    if (found != vertexOf.end())
      return found->second;
    const uint32_t index = static_cast<uint32_t>(set.vertices.size());
    set.vertices.push_back(MapBoundaryVertex{point});
    vertexOf.emplace(key, index);
    return index;
  };

  std::unordered_map<uint64_t, uint32_t> edgeOf;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || node.area.size() < 3u)
      continue;
    std::vector<int32_t> &loop = set.loops[node.id];
    for (size_t corner = 0u; corner < node.area.size(); ++corner)
    {
      const uint32_t from = vertexIndex(node.area[corner]);
      const uint32_t to = vertexIndex(node.area[(corner + 1u) % node.area.size()]);
      if (from == to)
        continue;
      const uint64_t low = std::min(from, to);
      const uint64_t high = std::max(from, to);
      const uint64_t key = (low << 32u) ^ high;

      const auto found = edgeOf.find(key);
      if (found == edgeOf.end())
      {
        const uint32_t index = static_cast<uint32_t>(set.edges.size());
        MapBoundaryEdge edge{};
        edge.from = from;
        edge.to = to;
        edge.cellA = node.id;
        const double dx = set.vertices[to].position.x - set.vertices[from].position.x;
        const double dy = set.vertices[to].position.y - set.vertices[from].position.y;
        edge.lengthMetres = std::sqrt(dx * dx + dy * dy);
        set.edges.push_back(std::move(edge));
        edgeOf.emplace(key, index);
        loop.push_back(static_cast<int32_t>(index) + 1);
        continue;
      }
      MapBoundaryEdge &edge = set.edges[found->second];
      if (edge.cellB == kInvalidNode && edge.cellA != node.id)
        edge.cellB = node.id;
      // Negative means this cell walks the edge the other way, which is what
      // keeps its loop wound consistently.
      loop.push_back(edge.from == from ? static_cast<int32_t>(found->second) + 1 : -(static_cast<int32_t>(found->second) + 1));
    }
  }

  // Properties. Assigned once, here, so the post-process and the renderer read
  // the same answers rather than each working them out again.
  for (MapBoundaryEdge &edge : set.edges)
  {
    edge.worldBorder = edge.cellB == kInvalidNode;
    if (edge.worldBorder)
      continue;
    const MapNode &a = graph.node(edge.cellA);
    const MapNode &b = graph.node(edge.cellB);
    edge.coast = a.land != b.land;
    edge.regionBorder = a.region != b.region;
    edge.biomeBorder = a.biome != b.biome;
    edge.contentBorder = a.content != b.content;
    edge.landformBorder = a.type != b.type;
    edge.classificationBorder = edge.regionBorder || edge.biomeBorder ||
        edge.contentBorder || edge.landformBorder;
  }
  for (const MapBoundaryEdge &edge : set.edges)
  {
    for (const uint32_t vertex : {edge.from, edge.to})
    {
      MapBoundaryVertex &corner = set.vertices[vertex];
      if (corner.cells < 255u)
        ++corner.cells;
      corner.coastal = corner.coastal || edge.coast;
      corner.classificationBoundary =
          corner.classificationBoundary || edge.classificationBorder;
      corner.onWorldBorder = corner.onWorldBorder || edge.worldBorder;
    }
  }
  return set;
}

void relaxEdges(MapEdgeSet &edges, const EdgeRelaxSettings &settings)
{
  if (edges.empty() || !(settings.segmentMetres > 0.0) || !(settings.wavelengthMetres > 0.0))
    return;

  const double frequency = 1.0 / settings.wavelengthMetres;
  // Two fields, offset from each other, give the displacement its two
  // components. A single field would move everything along one diagonal.
  const auto warp = [&](const Point2 &point, const double amplitude)
  {
    const double dx = fractalField(settings.seed ^ 0x51ed270bull, point, frequency, settings.octaves, settings.persistence);
    const double dy = fractalField(settings.seed ^ 0x2c1b3a97ull, point, frequency, settings.octaves, settings.persistence);
    return Point2(point.x + dx * amplitude, point.y + dy * amplitude);
  };

  // Vertices first. Every cell meeting here reads the same displaced position,
  // so the polygons stay closed without anyone having to agree on anything.
  std::vector<Point2> movedVertices;
  movedVertices.reserve(edges.vertices.size());
  for (const MapBoundaryVertex &vertex : edges.vertices)
  {
    // The world's rim stays put: a warped border would leave the map with a
    // ragged edge that is not a coastline.
    const double scale = vertex.coastal
        ? settings.coastScale
        : (vertex.classificationBoundary ? settings.classificationScale : settings.interiorScale);
    const double amplitude = vertex.onWorldBorder ? 0.0 : settings.amplitudeMetres * scale;
    movedVertices.push_back(warp(vertex.position, amplitude));
  }

  for (MapBoundaryEdge &edge : edges.edges)
  {
    const Point2 start = movedVertices[edge.from];
    const Point2 end = movedVertices[edge.to];
    edge.path.clear();
    edge.path.push_back(start);

    if (!edge.worldBorder)
    {
      const uint32_t steps = static_cast<uint32_t>(std::clamp(std::ceil(edge.lengthMetres / settings.segmentMetres), 1.0, 64.0));
      const double scale = edge.coast
          ? settings.coastScale
          : (edge.classificationBorder ? settings.classificationScale : settings.interiorScale);
      const double amplitude = settings.amplitudeMetres * scale;
      const Point2 from = edges.vertices[edge.from].position;
      const Point2 to = edges.vertices[edge.to].position;
      for (uint32_t step = 1u; step < steps; ++step)
      {
        const double t = static_cast<double>(step) / static_cast<double>(steps);
        // Warped from the *undisplaced* position, so the two cells sharing this
        // edge produce identical points whichever direction they walk it.
        const Point2 base(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
        edge.path.push_back(warp(base, amplitude));
      }
    }
    edge.path.push_back(end);
  }

  // The vertices are the authoritative junctions for every later boundary
  // operation. Update them only after the edge interiors have sampled their
  // undisplaced positions; doing it before the loop applies the warp twice.
  // Keeping only the displaced copies in edge.path made overlays snap back to
  // the original Voronoi corners and made smoothChains() curve pre-warp geometry.
  for (size_t index = 0u; index < edges.vertices.size(); ++index)
    edges.vertices[index].position = movedVertices[index];
}

void smoothClassificationJunctions(MapEdgeSet &edges, const uint32_t passes, const double strength)
{
  if (edges.empty() || passes == 0u || !(strength > 0.0))
    return;
  const double blend = std::clamp(strength, 0.0, 1.0);
  std::vector<std::vector<uint32_t>> incident(edges.vertices.size());
  for (const MapBoundaryEdge &edge : edges.edges)
    if (edge.classificationBorder && !edge.coast && !edge.worldBorder)
    {
      incident[edge.from].push_back(edge.to);
      incident[edge.to].push_back(edge.from);
    }

  for (uint32_t pass = 0u; pass < passes; ++pass)
  {
    std::vector<Point2> moved;
    moved.reserve(edges.vertices.size());
    for (size_t vertex = 0u; vertex < edges.vertices.size(); ++vertex)
    {
      const MapBoundaryVertex &current = edges.vertices[vertex];
      if (current.coastal || current.onWorldBorder || incident[vertex].size() != 2u)
      {
        moved.push_back(current.position);
        continue;
      }
      const Point2 &before = edges.vertices[incident[vertex][0]].position;
      const Point2 &after = edges.vertices[incident[vertex][1]].position;
      const Point2 average((before.x + after.x) * 0.5, (before.y + after.y) * 0.5);
      moved.push_back(Point2(
          current.position.x + (average.x - current.position.x) * blend,
          current.position.y + (average.y - current.position.y) * blend));
    }
    for (size_t vertex = 0u; vertex < edges.vertices.size(); ++vertex)
      edges.vertices[vertex].position = moved[vertex];
  }

  // Every edge, visible or not, must meet the newly smoothed authoritative
  // vertex because cell fills consume all of them.
  for (MapBoundaryEdge &edge : edges.edges)
  {
    if (edge.path.size() < 2u)
      edge.path = {edges.vertices[edge.from].position, edges.vertices[edge.to].position};
    else
    {
      edge.path.front() = edges.vertices[edge.from].position;
      edge.path.back() = edges.vertices[edge.to].position;
    }
  }
}

namespace
{

Point2 cubicBezier(const Point2 &p0, const Point2 &p1, const Point2 &p2,
    const Point2 &p3, const double t, const double tension)
{
  const auto distance = [](const Point2 &a, const Point2 &b)
  {
    return std::hypot(b.x - a.x, b.y - a.y);
  };
  const double chord = distance(p1, p2);
  if (!(chord > 0.0))
    return p1;
  const auto direction = [](const Point2 &from, const Point2 &to, const Point2 &fallback)
  {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double length = std::hypot(dx, dy);
    if (!(length > 0.0))
      return fallback;
    return Point2(dx / length, dy / length);
  };
  const Point2 chordDirection(
      (p2.x - p1.x) / chord, (p2.y - p1.y) / chord);
  const Point2 tangent1 = direction(p0, p2, chordDirection);
  const Point2 tangent2 = direction(p1, p3, chordDirection);
  const double handleFraction = 0.15 + 0.35 * std::clamp(tension, 0.0, 1.0);
  const double previous = distance(p0, p1);
  const double following = distance(p2, p3);
  const double handle1 = std::min(
      chord * handleFraction,
      previous > 0.0 ? (previous + chord) * 0.25 : chord * handleFraction);
  const double handle2 = std::min(
      chord * handleFraction,
      following > 0.0 ? (chord + following) * 0.25 : chord * handleFraction);
  const Point2 control1(
      p1.x + tangent1.x * handle1, p1.y + tangent1.y * handle1);
  const Point2 control2(
      p2.x - tangent2.x * handle2, p2.y - tangent2.y * handle2);
  const double u = 1.0 - t;
  const double a = u * u * u;
  const double b = 3.0 * u * u * t;
  const double c = 3.0 * u * t * t;
  const double d = t * t * t;
  if (!std::isfinite(a + b + c + d))
    return Point2(p1.x + (p2.x - p1.x) * t, p1.y + (p2.y - p1.y) * t);
  return Point2(
      p1.x * a + control1.x * b + control2.x * c + p2.x * d,
      p1.y * a + control1.y * b + control2.y * c + p2.y * d);
}

} // namespace

void smoothChains(MapEdgeSet &edges, const uint32_t samplesPerEdge, const double tension)
{
  if (edges.empty() || samplesPerEdge < 2u)
    return;

  // Visible edges at each vertex. A vertex with exactly two is inside a chain
  // and is a bend; anything else is where chains meet and stays a corner.
  std::vector<std::vector<uint32_t>> incident(edges.vertices.size());
  for (size_t index = 0u; index < edges.edges.size(); ++index)
    if (edges.edges[index].visible)
    {
      incident[edges.edges[index].from].push_back(static_cast<uint32_t>(index));
      incident[edges.edges[index].to].push_back(static_cast<uint32_t>(index));
    }

  const auto positionOf = [&edges](const uint32_t vertex)
  {
    return edges.vertices[vertex].position;
  };

  std::vector<uint8_t> walked(edges.edges.size(), 0u);
  for (size_t start = 0u; start < edges.edges.size(); ++start)
  {
    if (!edges.edges[start].visible || walked[start] != 0u)
      continue;

    // Walk out both ways to the ends of the chain, collecting the vertices it
    // passes through and the edges between them.
    std::vector<uint32_t> chainVertices{edges.edges[start].from, edges.edges[start].to};
    std::vector<uint32_t> chainEdges{static_cast<uint32_t>(start)};
    walked[start] = 1u;
    for (int32_t direction = 0; direction < 2; ++direction)
    {
      uint32_t vertex = direction == 0 ? edges.edges[start].to : edges.edges[start].from;
      uint32_t previous = static_cast<uint32_t>(start);
      while (incident[vertex].size() == 2u)
      {
        const uint32_t next = incident[vertex][0] == previous ? incident[vertex][1] : incident[vertex][0];
        if (walked[next] != 0u)
          break;
        walked[next] = 1u;
        const uint32_t far = edges.edges[next].from == vertex ? edges.edges[next].to : edges.edges[next].from;
        if (direction == 0)
        {
          chainVertices.push_back(far);
          chainEdges.push_back(next);
        }
        else
        {
          chainVertices.insert(chainVertices.begin(), far);
          chainEdges.insert(chainEdges.begin(), next);
        }
        previous = next;
        vertex = far;
      }
    }
    if (chainVertices.size() < 2u)
      continue;

    const bool closed = chainVertices.size() > 2u && chainVertices.front() == chainVertices.back();
    const size_t count = chainVertices.size();
    const auto at = [&](const int64_t index)
    {
      if (closed)
      {
        const int64_t span = static_cast<int64_t>(count) - 1;
        int64_t wrapped = index % span;
        if (wrapped < 0)
          wrapped += span;
        return positionOf(chainVertices[static_cast<size_t>(wrapped)]);
      }
      return positionOf(chainVertices[static_cast<size_t>(std::clamp<int64_t>(index, 0, static_cast<int64_t>(count) - 1))]);
    };

    // One curve segment per edge of the chain, sampled and written back onto
    // that edge - so the cells either side both read the smoothed geometry and
    // nothing has to be reassembled.
    for (size_t step = 0u; step < chainEdges.size(); ++step)
    {
      MapBoundaryEdge &edge = edges.edges[chainEdges[step]];
      const Point2 p0 = at(static_cast<int64_t>(step) - 1);
      const Point2 p1 = at(static_cast<int64_t>(step));
      const Point2 p2 = at(static_cast<int64_t>(step) + 1);
      const Point2 p3 = at(static_cast<int64_t>(step) + 2);
      const bool forward = edge.from == chainVertices[step];

      std::vector<Point2> path;
      path.reserve(samplesPerEdge);
      for (uint32_t sample = 0u; sample < samplesPerEdge; ++sample)
      {
        const double t = static_cast<double>(sample) / static_cast<double>(samplesPerEdge - 1u);
        path.push_back(cubicBezier(p0, p1, p2, p3, t, tension));
      }
      // The endpoints are the shared vertices, exactly: an interpolating curve
      // is only worth using if it actually interpolates.
      path.front() = p1;
      path.back() = p2;
      if (!forward)
        std::reverse(path.begin(), path.end());
      edge.path = std::move(path);
    }
  }
}

std::vector<Point2> cellBoundary(const MapEdgeSet &edges, const uint32_t node)
{
  std::vector<Point2> boundary;
  if (node >= edges.loops.size())
    return boundary;
  for (const int32_t entry : edges.loops[node])
  {
    if (entry == 0)
      continue;
    const uint32_t index = static_cast<uint32_t>(std::abs(entry) - 1);
    if (index >= edges.edges.size())
      continue;
    const std::vector<Point2> &path = edges.edges[index].path;
    if (path.size() < 2u)
      continue;
    // The last point of each edge is the first of the next, so it is dropped.
    if (entry > 0)
      boundary.insert(boundary.end(), path.begin(), path.end() - 1);
    else
      boundary.insert(boundary.end(), path.rbegin(), path.rend() - 1);
  }
  return boundary;
}

} // namespace procedural::world
