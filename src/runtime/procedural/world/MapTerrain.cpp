#include "procedural/world/MapTerrain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <vector>

namespace procedural::world {

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

double distanceBetween(const MapNode &a, const MapNode &b)
{
  return std::hypot(a.site.x - b.site.x, a.site.y - b.site.y);
}

double smoothStep(const double value)
{
  const double t = std::clamp(value, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

bool planarType(const CellType type)
{
  return type == CellType::Plain || type == CellType::Plateau;
}

struct QueueEntry
{
  double distance = 0.0;
  uint32_t node = kInvalidNode;
  uint32_t source = kInvalidNode;

  bool operator<(const QueueEntry &other) const
  {
    if (distance != other.distance)
    {
      return distance > other.distance;
    }
    if (node != other.node)
    {
      return node > other.node;
    }
    return source > other.source;
  }
};

} // namespace

bool conditionTerrain(const TerrainConditioningSettings &settings, const RiverNetwork &rivers, MapGraph &graph, TerrainConditioningReport *report, std::string *error)
{
  const auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }
    return false;
  };

  if (report != nullptr)
  {
    *report = TerrainConditioningReport{};
  }
  if (graph.nodeCount() == 0u)
  {
    return fail("cannot condition an empty terrain graph");
  }
  if (rivers.nodes.size() != graph.nodeCount())
  {
    return fail("terrain conditioning requires a matching preliminary river network");
  }
  if (!std::isfinite(settings.planarStrength) || !std::isfinite(settings.floodplainStrength) || settings.planarStrength < 0.0 || settings.planarStrength > 1.0 || settings.floodplainStrength < 0.0 ||
      settings.floodplainStrength > 1.0)
  {
    return fail("terrain conditioning strengths must be within [0, 1]");
  }
  if (!std::isfinite(settings.maximumPlanarChangeMetres) || !std::isfinite(settings.floodplainWidthMetres) || !std::isfinite(settings.floodplainBankGradient) || !std::isfinite(settings.maximumFloodplainChangeMetres) ||
      !std::isfinite(settings.majorRiverDischargeFraction) || !(settings.maximumPlanarChangeMetres >= 0.0) || !(settings.floodplainWidthMetres > 0.0) || !(settings.floodplainBankGradient >= 0.0) ||
      !(settings.maximumFloodplainChangeMetres >= 0.0) || settings.majorRiverDischargeFraction < 0.0 || settings.majorRiverDischargeFraction > 1.0)
  {
    return fail("terrain conditioning dimensions and fractions are invalid");
  }

  TerrainConditioningReport result{};
  std::vector<double> elevation(graph.nodeCount(), 0.0);
  std::vector<double> next(graph.nodeCount(), 0.0);
  for (const MapNode &node : graph.nodes())
  {
    elevation[node.id] = node.intent.elevationMetres;
  }

  // A same-landform Laplacian removes cell-scale corrugation but does not
  // cross a plateau rim, mountain front, coast, or river-cut valley. Limiting
  // each pass prevents a small polygon next to one outlier from collapsing.
  std::vector<uint8_t> planarTouched(graph.nodeCount(), 0u);
  for (uint32_t pass = 0u; pass < settings.planarPasses; ++pass)
  {
    next = elevation;
    for (const MapNode &node : graph.nodes())
    {
      if (node.refined() || !node.land || node.coastal || !planarType(node.type))
      {
        continue;
      }
      double weighted = 0.0;
      double totalWeight = 0.0;
      for (const uint32_t neighbourId : node.neighbours)
      {
        const MapNode &neighbour = graph.node(neighbourId);
        if (neighbour.refined() || !neighbour.land || neighbour.type != node.type)
        {
          continue;
        }
        const double weight = 1.0 / std::max(1.0, distanceBetween(node, neighbour));
        weighted += elevation[neighbourId] * weight;
        totalWeight += weight;
      }
      if (totalWeight <= 0.0)
      {
        continue;
      }
      const double wanted = weighted / totalWeight;
      const double delta = std::clamp((wanted - elevation[node.id]) * settings.planarStrength, -settings.maximumPlanarChangeMetres, settings.maximumPlanarChangeMetres);
      next[node.id] += delta;
      if (std::abs(delta) > 1.0e-9)
      {
        planarTouched[node.id] = 1u;
      }
    }
    elevation.swap(next);
  }

  // Metric distance to the nearest major channel. Discharge rather than order
  // works even on a graph too coarse to resolve order-two tributaries.
  std::vector<double> toRiver(graph.nodeCount(), kInfinity);
  std::vector<uint32_t> nearestRiver(graph.nodeCount(), kInvalidNode);
  std::priority_queue<QueueEntry> queue;
  const double dischargeThreshold = rivers.largestDischargeCumecs * settings.majorRiverDischargeFraction;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || !node.land || !rivers.nodes[node.id].river || rivers.nodes[node.id].dischargeCumecs < dischargeThreshold)
    {
      continue;
    }
    toRiver[node.id] = 0.0;
    nearestRiver[node.id] = node.id;
    queue.push(QueueEntry{0.0, node.id, node.id});
  }
  while (!queue.empty())
  {
    const QueueEntry entry = queue.top();
    queue.pop();
    if (entry.distance > toRiver[entry.node] || entry.distance > settings.floodplainWidthMetres)
    {
      continue;
    }
    const MapNode &node = graph.node(entry.node);
    for (const uint32_t neighbourId : node.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined() || !neighbour.land)
      {
        continue;
      }
      const double candidate = entry.distance + distanceBetween(node, neighbour);
      if (candidate > settings.floodplainWidthMetres || candidate >= toRiver[neighbourId])
      {
        continue;
      }
      toRiver[neighbourId] = candidate;
      nearestRiver[neighbourId] = entry.source;
      queue.push(QueueEntry{candidate, neighbourId, entry.source});
    }
  }

  std::vector<uint8_t> floodplainTouched(graph.nodeCount(), 0u);
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || !node.land || node.coastal || nearestRiver[node.id] == kInvalidNode)
    {
      continue;
    }
    const uint32_t source = nearestRiver[node.id];
    const double falloff = smoothStep(1.0 - toRiver[node.id] / settings.floodplainWidthMetres);
    const double strength = settings.floodplainStrength * falloff;
    // The filled channel surface is a monotone downstream grade. Banks climb
    // gently away from it, creating a buildable cross-section without making
    // the entire river longitudinally level.
    const double channel = rivers.nodes[source].filledElevationMetres;
    const double wanted = channel + settings.floodplainBankGradient * toRiver[node.id];
    const double delta = std::clamp(wanted - elevation[node.id], -settings.maximumFloodplainChangeMetres, settings.maximumFloodplainChangeMetres);
    elevation[node.id] += delta * strength;
    if (strength > 1.0e-6)
    {
      floodplainTouched[node.id] = 1u;
    }
  }

  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
  {
    MapNode &node = graph.node(id);
    if (node.refined() || !node.land)
    {
      continue;
    }
    const double conditioned = std::max(graph.brief().seaLevelMetres + 0.01, elevation[node.id]);
    result.totalElevationChangeMetres += std::abs(conditioned - node.intent.elevationMetres);
    node.intent.elevationMetres = conditioned;
    if (planarTouched[node.id] != 0u)
    {
      ++result.planarCells;
      node.intent.reliefAmplitude *= 0.72;
      node.intent.roughness *= 0.78;
    }
    if (floodplainTouched[node.id] != 0u)
    {
      ++result.floodplainCells;
      node.intent.reliefAmplitude *= 0.45;
      node.intent.roughness *= 0.55;
    }
  }

  if (report != nullptr)
  {
    *report = result;
  }
  return true;
}

} // namespace procedural::world
