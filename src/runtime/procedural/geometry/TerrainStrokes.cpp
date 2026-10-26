#include "procedural/Geometry.hpp"
#include "procedural/geometry/TerrainMaterial.hpp"
#include "procedural/geometry/TerrainMeshBuild.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace procedural::geometry
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

uint64_t mix(uint64_t value)
{
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;
  return value;
}

double unitNoise(const uint64_t seed, const uint64_t id)
{
  return static_cast<double>(mix(seed ^ (id * 0x9e3779b9ull)) >> 11u) /
      static_cast<double>(1ull << 53u);
}

} // namespace

double StrokeSettings::widthMetres(const double dischargeCumecs) const
{
  // Downstream hydraulic geometry, Leopold & Maddock. The three exponents sum
  // to one, which is the check that the coefficients are mutually consistent.
  return widthCoefficient * std::pow(std::max(0.0, dischargeCumecs), 0.5);
}

double StrokeSettings::depthMetres(const double dischargeCumecs) const
{
  return depthCoefficient * std::pow(std::max(0.0, dischargeCumecs), 0.4);
}

ChannelNetwork resolveChannelNetwork(
    const world::GeneratedWorld &world, const StrokeSettings &settings)
{
  ChannelNetwork network{};
  if (world.rivers.segments.empty())
    return network;

  const size_t cells = world.rivers.nodes.size();
  std::vector<uint32_t> downstreamOf(cells, world::kInvalidNode);
  std::vector<uint32_t> inflow(cells, 0u);
  for (uint32_t id = 0u; id < cells; ++id)
  {
    const world::RiverNode &node = world.rivers.nodes[id];
    if (!node.river)
      continue;
    downstreamOf[id] = node.downstream;
    if (node.downstream != world::kInvalidNode && node.downstream < cells)
      ++inflow[node.downstream];
  }

  // Junction nodes first, so a reach can name the node it ends on while it is
  // being built rather than being stitched to one afterwards.
  std::vector<uint32_t> nodeOfCell(cells, kInvalidChannelNode);
  for (uint32_t id = 0u; id < cells; ++id)
  {
    if (!world.rivers.nodes[id].river || inflow[id] < 2u)
      continue;
    ChannelNode node{};
    node.position = world.graph.node(id).site;
    const double flow = world.rivers.nodes[id].dischargeCumecs;
    node.surfaceMetres = world.surface.sample(node.position).elevationMetres;
    node.widthMetres =
        std::max(settings.minimumWidthMetres, settings.widthMetres(flow));
    node.bedMetres = node.surfaceMetres -
        std::max(settings.minimumDepthMetres, settings.depthMetres(flow));
    nodeOfCell[id] = static_cast<uint32_t>(network.nodes.size());
    network.nodes.push_back(std::move(node));
  }

  std::vector<uint32_t> starts;
  for (uint32_t id = 0u; id < cells; ++id)
    if (world.rivers.nodes[id].river && inflow[id] != 1u)
      starts.push_back(id);

  std::vector<uint8_t> visited(cells, 0u);
  for (const uint32_t id : starts)
  {
    // A reach runs from a headwater or a confluence down to the next confluence
    // or the mouth, stopping *on* the junction so the corridors meet there.
    std::vector<uint32_t> course;
    for (uint32_t at = id; at != world::kInvalidNode && at < cells;)
    {
      course.push_back(at);
      if (!world.rivers.nodes[at].river)
        break;
      const uint32_t next = downstreamOf[at];
      if (next == world::kInvalidNode || next >= cells)
        break;
      if (inflow[next] >= 2u)
      {
        course.push_back(next);
        break;
      }
      at = next;
      if (course.size() > cells)
        break;
    }
    if (course.size() < 2u)
      continue;

    ChannelReach reach{};
    reach.upstreamNode = nodeOfCell[course.front()];
    reach.downstreamNode = nodeOfCell[course.back()];
    reach.mapCellCount = static_cast<uint32_t>(course.size());

    // Centreline with meander. The map's course runs cell to cell and is far
    // too straight; real channels wander at ten to fourteen channel widths.
    for (size_t index = 0u; index + 1u < course.size(); ++index)
    {
      const Point2 a = world.graph.node(course[index]).site;
      const Point2 b = world.graph.node(course[index + 1u]).site;
      const double flowA = world.rivers.nodes[course[index]].dischargeCumecs;
      const double flowB = world.rivers.nodes[course[index + 1u]].dischargeCumecs;
      const double span = distance(a, b);
      const auto steps = static_cast<uint32_t>(std::max<int64_t>(
          1, std::llround(span / std::max(1.0, settings.spacingMetres))));
      for (uint32_t step = 0u; step < steps; ++step)
      {
        const double t = static_cast<double>(step) / static_cast<double>(steps);
        const double flow = flowA + (flowB - flowA) * t;
        Point2 point(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
        if (settings.meanderScale > 0.0)
        {
          const double width = settings.widthMetres(flow);
          const double wavelength = std::max(60.0, width * 12.0);
          const double amplitude =
              std::min(span * 0.35, width * settings.meanderScale);
          double tx = b.x - a.x;
          double tz = b.y - a.y;
          const double length = std::max(1.0e-9, std::hypot(tx, tz));
          tx /= length;
          tz /= length;
          const double phase = unitNoise(settings.seed, course[index]) * kPi * 2.0;
          const double offset =
              std::sin(span * t / wavelength * kPi * 2.0 + phase) * amplitude;
          point = Point2(point.x + tz * offset, point.y - tx * offset);
        }
        ChannelVertex vertex{};
        vertex.position = point;
        vertex.dischargeCumecs = flow;
        reach.vertices.push_back(vertex);
      }
    }
    ChannelVertex last{};
    last.position = world.graph.node(course.back()).site;
    last.dischargeCumecs = world.rivers.nodes[course.back()].dischargeCumecs;
    reach.vertices.push_back(last);
    if (reach.vertices.size() < 2u)
      continue;

    // Water surface: sampled, forced monotone, then quantised into flat
    // reaches. Monotone before quantising, so a pool cannot be lifted above
    // the one upstream of it.
    for (ChannelVertex &vertex : reach.vertices)
      vertex.surfaceMetres =
          world.surface.sample(vertex.position).elevationMetres;
    for (size_t index = 1u; index < reach.vertices.size(); ++index)
      reach.vertices[index].surfaceMetres = std::min(
          reach.vertices[index].surfaceMetres,
          reach.vertices[index - 1u].surfaceMetres);
    if (settings.reachIntervalMetres > 0.0)
      for (ChannelVertex &vertex : reach.vertices)
        vertex.surfaceMetres =
            std::floor(vertex.surfaceMetres / settings.reachIntervalMetres) *
            settings.reachIntervalMetres;

    for (ChannelVertex &vertex : reach.vertices)
    {
      vertex.widthMetres = std::max(settings.minimumWidthMetres,
          settings.widthMetres(vertex.dischargeCumecs));
      vertex.depthMetres = std::max(settings.minimumDepthMetres,
          settings.depthMetres(vertex.dischargeCumecs));
      vertex.bedMetres = vertex.surfaceMetres - vertex.depthMetres;
    }

    const auto reachIndex = static_cast<uint32_t>(network.reaches.size());
    if (reach.downstreamNode != kInvalidChannelNode)
      network.nodes[reach.downstreamNode].incoming.push_back(reachIndex);
    if (reach.upstreamNode != kInvalidChannelNode)
      network.nodes[reach.upstreamNode].outgoing = reachIndex;

    for (const uint32_t cell : course)
    {
      if (cell >= cells || !world.rivers.nodes[cell].river)
        continue;
      ++network.carvedCells;
      if (visited[cell] != 0u && inflow[cell] < 2u)
        ++network.duplicateCells;
      visited[cell] = 1u;
    }
    network.reaches.push_back(std::move(reach));
  }

  // Junction surfaces, resolved once every reach exists.
  //
  // The junction is downstream of everything arriving at it, so its water
  // surface is the *lowest* of what arrives - not an independent sample. Taking
  // a sample instead lets a junction sit above the reach feeding it, and the
  // reach then climbs into it. Lowering an arriving end can never break the
  // monotonicity already established; raising a departing start can, so the
  // outgoing reach is re-clamped after.
  for (ChannelNode &node : network.nodes)
  {
    double surface = node.surfaceMetres;
    for (const uint32_t incoming : node.incoming)
      if (incoming < network.reaches.size() &&
          !network.reaches[incoming].vertices.empty())
        surface = std::min(
            surface, network.reaches[incoming].vertices.back().surfaceMetres);
    if (settings.reachIntervalMetres > 0.0)
      surface = std::floor(surface / settings.reachIntervalMetres) *
          settings.reachIntervalMetres;
    node.surfaceMetres = surface;
    node.bedMetres = surface -
        std::max(settings.minimumDepthMetres,
            settings.depthMetres(
                node.incoming.empty()
                    ? 0.0
                    : network.reaches[node.incoming.front()].vertices.back()
                          .dischargeCumecs));

    for (const uint32_t incoming : node.incoming)
      if (incoming < network.reaches.size() &&
          !network.reaches[incoming].vertices.empty())
        network.reaches[incoming].vertices.back().surfaceMetres = surface;
    if (node.outgoing != kInvalidChannelNode &&
        node.outgoing < network.reaches.size() &&
        !network.reaches[node.outgoing].vertices.empty())
    {
      ChannelReach &out = network.reaches[node.outgoing];
      out.vertices.front().surfaceMetres = surface;
      for (size_t index = 1u; index < out.vertices.size(); ++index)
        out.vertices[index].surfaceMetres = std::min(
            out.vertices[index].surfaceMetres,
            out.vertices[index - 1u].surfaceMetres);
    }
  }

  // Bed follows the final surface everywhere.
  for (ChannelReach &reach : network.reaches)
    for (ChannelVertex &vertex : reach.vertices)
      vertex.bedMetres = vertex.surfaceMetres - vertex.depthMetres;

  return network;
}

StrokeGeometry buildStrokeGeometry(
    const ChannelNetwork &network,
    const world::GeneratedWorld &world,
    const StrokeSettings &settings)
{
  StrokeGround ground{};
  const world::GeneratedWorld *source = &world;
  ground.height = [source](const Point2 point)
  { return source->surface.sample(point).elevationMetres; };
  ground.cover = [source](const Point2 point)
  {
    const uint32_t node = source->graph.nodeAt(point);
    return node == world::kInvalidNode
        ? world::Biome::Grassland
        : world::biomeOf(source->graph.node(node));
  };
  return buildStrokeGeometry(network, ground, settings);
}

StrokeGeometry buildStrokeGeometry(
    const ChannelNetwork &network,
    const StrokeGround &ground,
    const StrokeSettings &settings)
{
  StrokeGeometry geometry{};
  geometry.carvedCells = network.carvedCells;
  geometry.duplicateCells = network.duplicateCells;
  geometry.junctionCount = static_cast<uint32_t>(network.nodes.size());

  for (const ChannelReach &reach : network.reaches)
  {
    const size_t count = reach.vertices.size();
    std::vector<Point2> centre(count);
    std::vector<Point2> leftBank(count);
    std::vector<Point2> rightBank(count);
    std::vector<double> surface(count);
    std::vector<double> bed(count);
    for (size_t index = 0u; index < count; ++index)
    {
      const ChannelVertex &vertex = reach.vertices[index];
      const Point2 &previous = reach.vertices[index == 0u ? 0u : index - 1u].position;
      const Point2 &next =
          reach.vertices[std::min(count - 1u, index + 1u)].position;
      double tx = next.x - previous.x;
      double tz = next.y - previous.y;
      const double length = std::max(1.0e-9, std::hypot(tx, tz));
      tx /= length;
      tz /= length;
      const double half = vertex.widthMetres * 0.5;
      // Along the *horizontal* normal, never the surface normal: that is what
      // keeps both banks of a section at one elevation.
      centre[index] = vertex.position;
      leftBank[index] =
          Point2(vertex.position.x - tz * half, vertex.position.y + tx * half);
      rightBank[index] =
          Point2(vertex.position.x + tz * half, vertex.position.y - tx * half);
      surface[index] = vertex.surfaceMetres;
      bed[index] = vertex.bedMetres;
    }

    const auto pushChain = [&geometry](std::vector<Point2> points,
                               std::vector<double> heights)
    {
      if (points.size() < 2u)
        return;
      Chain chain{};
      chain.kind = ChainKind::Stroke;
      chain.points = std::move(points);
      chain.elevations = std::move(heights);
      chain.elevationMetres =
          chain.elevations.empty() ? 0.0 : chain.elevations.front();
      chain.hasElevation = true;
      geometry.chains.push_back(std::move(chain));
    };
    pushChain(leftBank, surface);
    pushChain(rightBank, surface);
    pushChain(centre, bed);

    const world::Biome cover =
        ground.cover ? ground.cover(centre.front()) : world::Biome::Grassland;
    for (size_t index = 0u; index + 1u < count; ++index)
    {
      const double groundA = ground.height(leftBank[index]);
      const double groundB = ground.height(leftBank[index + 1u]);
      if (groundA - surface[index] < settings.minimumBankMetres &&
          groundB - surface[index + 1u] < settings.minimumBankMetres)
        continue;
      StepQuad left{};
      left.brinkA = leftBank[index];
      left.brinkB = leftBank[index + 1u];
      left.toeA = centre[index];
      left.toeB = centre[index + 1u];
      left.brinkAHeight = std::max(groundA, surface[index]);
      left.brinkBHeight = std::max(groundB, surface[index + 1u]);
      left.toeAHeight = bed[index];
      left.toeBHeight = bed[index + 1u];
      left.material = settings.bankMaterial;
      left.role = SurfaceRole::RiverBank;
      left.cover = cover;
      geometry.quads.push_back(left);

      // Brink is always the upper edge of a step; emitting the right bank the
      // other way round inverts the convention and the channel reads as a ridge.
      StepQuad right{};
      right.brinkA = rightBank[index];
      right.brinkB = rightBank[index + 1u];
      right.toeA = centre[index];
      right.toeB = centre[index + 1u];
      right.brinkAHeight =
          std::max(ground.height(rightBank[index]), surface[index]);
      right.brinkBHeight =
          std::max(ground.height(rightBank[index + 1u]), surface[index + 1u]);
      right.toeAHeight = bed[index];
      right.toeBHeight = bed[index + 1u];
      right.material = settings.bankMaterial;
      right.role = SurfaceRole::RiverBank;
      right.cover = cover;
      geometry.quads.push_back(right);
      geometry.channelLengthMetres += distance(centre[index], centre[index + 1u]);
    }
    ++geometry.courseCount;
    geometry.maximumWidthMetres = std::max(
        geometry.maximumWidthMetres, reach.vertices.back().widthMetres);
    geometry.maximumDepthMetres = std::max(
        geometry.maximumDepthMetres, reach.vertices.back().depthMetres);
  }

  // Junction polygons. The node knows every reach incident on it and how wide
  // each arrives, so the merged area is the hull of those corridor ends rather
  // than a blind disc: the three channels open into one floor at one bed
  // elevation, which is what merging means.
  for (const ChannelNode &node : network.nodes)
  {
    double radius = node.widthMetres * 0.5;
    for (const uint32_t incoming : node.incoming)
      if (incoming < network.reaches.size() &&
          !network.reaches[incoming].vertices.empty())
        radius = std::max(radius,
            network.reaches[incoming].vertices.back().widthMetres * 0.5);
    radius *= settings.junctionScale;

    Chain disc{};
    disc.kind = ChainKind::Stroke;
    disc.closed = true;
    disc.hasElevation = true;
    disc.elevationMetres = node.bedMetres;
    const uint32_t sides = 10u;
    for (uint32_t corner = 0u; corner < sides; ++corner)
    {
      const double angle =
          static_cast<double>(corner) / static_cast<double>(sides) * kPi * 2.0;
      disc.points.push_back(Point2(node.position.x + std::cos(angle) * radius,
          node.position.y + std::sin(angle) * radius));
      disc.elevations.push_back(node.bedMetres);
    }
    geometry.chains.push_back(std::move(disc));
  }
  return geometry;
}

StrokeGeometry buildStrokes(
    const world::GeneratedWorld &world, const StrokeSettings &settings)
{
  return buildStrokeGeometry(
      resolveChannelNetwork(world, settings), world, settings);
}

} // namespace procedural::geometry
