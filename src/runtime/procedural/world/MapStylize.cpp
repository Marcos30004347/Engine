#include "procedural/world/MapStylize.hpp"

#include "procedural/world/MapEdges.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace procedural::world {

namespace {

double smoothStep(const double t)
{
  const double clamped = std::clamp(t, 0.0, 1.0);
  return clamped * clamped * (3.0 - 2.0 * clamped);
}

} // namespace

bool stylizeRelief(const StylizeSettings &settings, MapGraph &graph, PlateauField &plateaus, StylizeReport *report, std::string *error)
{
  StylizeReport local{};
  double highest = settings.seaLevelMetres;
  double lowest = settings.seaLevelMetres;
  bool any = false;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || !node.land)
    {
      continue;
    }
    highest = any ? std::max(highest, node.intent.elevationMetres) : node.intent.elevationMetres;
    lowest = any ? std::min(lowest, node.intent.elevationMetres) : node.intent.elevationMetres;
    any = true;
  }
  if (!any || !(highest > settings.seaLevelMetres))
  {
    if (error != nullptr)
    {
      *error = "the world has no land relief to stylize";
    }
    return false;
  }
  const double relief = highest - settings.seaLevelMetres;
  local.reliefBefore = relief;

  const double low = settings.seaLevelMetres + relief * settings.lowFraction;
  const double high = settings.seaLevelMetres + relief * settings.highFraction;
  const double span = std::max(1.0e-6, high - low);

  // f(h) = h + relief * gain * smoothstep((h - low) / span).
  //
  // f'(h) = 1 + relief * gain * s'(t) / span, and smoothstep's derivative peaks
  // at 1.5, so f' > 0 everywhere as long as gain < span / (1.5 * relief). The
  // bound is what makes the claim "no descent relation can reverse" a theorem
  // rather than a hope.
  local.maximumGain = span / (1.5 * relief);
  local.gainApplied = std::min(settings.gain, local.maximumGain * 0.95);

  const auto remap = [&](const double height) {
    if (height <= settings.seaLevelMetres)
    {
      return height;
    }
    return height + relief * local.gainApplied * smoothStep((height - low) / span);
  };

  // Monotonicity, checked rather than asserted: sample the range densely and
  // require the mapping to be strictly increasing.
  double previous = remap(settings.seaLevelMetres);
  for (uint32_t sample = 1u; sample <= 4096u; ++sample)
  {
    const double height = settings.seaLevelMetres + relief * static_cast<double>(sample) / 4096.0;
    const double mapped = remap(height);
    if (!(mapped > previous))
    {
      local.monotone = false;
    }
    previous = mapped;
  }
  if (!local.monotone)
  {
    if (error != nullptr)
    {
      *error = "the stylization gain is not monotone; lower it";
    }
    return false;
  }

  for (MapNode &node : const_cast<std::vector<MapNode> &>(graph.nodes()))
  {
    if (!node.land)
    {
      continue;
    }
    node.intent.elevationMetres = remap(node.intent.elevationMetres);
    node.intent.reliefAmplitude *= 1.0 + local.gainApplied;
  }

  // Plateau blocks were snapped to a ladder before drainage, and a remap moves
  // them off it. Re-snap so a brink is still exactly a contour, which is the
  // whole reason the ladder and the contour interval are one number.
  if (!plateaus.blocks.empty() && plateaus.ladderIntervalMetres > 0.0)
  {
    double stylizedHighest = settings.seaLevelMetres;
    for (const MapNode &node : graph.nodes())
    {
      if (!node.refined() && node.land)
      {
        stylizedHighest = std::max(stylizedHighest, node.intent.elevationMetres);
      }
    }
    const double scale = (stylizedHighest - settings.seaLevelMetres) / std::max(1.0e-6, highest - settings.seaLevelMetres);
    plateaus.ladderIntervalMetres *= std::max(1.0, scale);
    const double interval = plateaus.ladderIntervalMetres;
    for (PlateauBlock &block : plateaus.blocks)
    {
      block.elevationMetres = std::round(remap(block.elevationMetres) / interval) * interval;
      block.rung = static_cast<int32_t>(std::llround(block.elevationMetres / interval));
      for (const uint32_t cell : block.cells)
      {
        graph.node(cell).intent.elevationMetres = block.elevationMetres;
      }
    }
  }

  double after = settings.seaLevelMetres;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined() && node.land)
    {
      after = std::max(after, node.intent.elevationMetres);
    }
  }
  local.reliefAfter = after - settings.seaLevelMetres;
  if (report != nullptr)
  {
    *report = local;
  }
  return true;
}

std::vector<RidgeLine> extractRidges(const StylizeSettings &settings, const MapGraph &graph, const RiverNetwork &rivers)
{
  std::vector<RidgeLine> ridges;
  if (rivers.nodes.size() != graph.nodeCount())
  {
    return ridges;
  }

  // Basin per cell: follow the drainage to its sink. The network already
  // guarantees this terminates - depression filling gives every cell a strictly
  // lower neighbour, so the descent cannot cycle.
  std::vector<uint32_t> basin(graph.nodeCount(), kInvalidNode);
  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
  {
    if (graph.node(id).refined() || !graph.node(id).land)
    {
      continue;
    }
    uint32_t at = id;
    uint32_t guard = 0u;
    while (rivers.nodes[at].downstream != kInvalidNode && guard++ < graph.nodeCount())
    {
      if (basin[at] != kInvalidNode)
      {
        break;
      }
      at = rivers.nodes[at].downstream;
    }
    basin[id] = basin[at] != kInvalidNode ? basin[at] : at;
  }

  double highest = 0.0;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined() && node.land)
    {
      highest = std::max(highest, node.intent.elevationMetres);
    }
  }
  const double prominenceFloor = highest * settings.ridgeProminenceFraction;

  const MapEdgeSet edges = buildEdgeSet(graph, 1.0e-4);
  for (const MapBoundaryEdge &edge : edges.edges)
  {
    if (edge.worldBorder || edge.cellA == kInvalidNode || edge.cellB == kInvalidNode)
    {
      continue;
    }
    const MapNode &a = graph.node(edge.cellA);
    const MapNode &b = graph.node(edge.cellB);
    if (a.refined() || b.refined() || !a.land || !b.land)
    {
      continue;
    }
    if (basin[edge.cellA] == basin[edge.cellB])
    {
      continue;
    }
    // A divide worth an edge in the mesh stands meaningfully above the ground
    // it separates; every other cell boundary is one too, and drawing them all
    // would be a net over the whole world.
    const double crest = std::min(a.intent.elevationMetres, b.intent.elevationMetres);
    if (crest < prominenceFloor)
    {
      continue;
    }
    RidgeLine line{};
    line.prominenceMetres = crest;
    line.points = {edges.vertices[edge.from].position, edges.vertices[edge.to].position};
    ridges.push_back(std::move(line));
  }
  return ridges;
}

std::vector<Point2> extractPeaks(const MapGraph &graph)
{
  std::vector<Point2> peaks;
  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
  {
    const MapNode &node = graph.node(id);
    if (node.refined() || !node.land || node.neighbours.empty())
    {
      continue;
    }
    bool maximum = true;
    for (const uint32_t neighbourId : node.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined())
      {
        continue;
      }
      if (neighbour.intent.elevationMetres >= node.intent.elevationMetres)
      {
        maximum = false;
        break;
      }
    }
    if (maximum)
    {
      peaks.push_back(node.site);
    }
  }
  return peaks;
}

} // namespace procedural::world
