#include "procedural/world/MapFluvial.hpp"

#include "procedural/world/MapElevation.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world {

namespace {

// Distance between two cells' sites, floored so a coincident pair cannot make
// the implicit coefficient infinite.
double reachLength(const MapNode &a, const MapNode &b)
{
  const double dx = a.site.x - b.site.x;
  const double dz = a.site.y - b.site.y;
  return std::max(1.0, std::sqrt(dx * dx + dz * dz));
}

} // namespace

bool erodeFluvial(const FluvialSettings &settings, const RiverSettings &riverSettings, MapGraph &graph, FluvialReport *report, std::string *error)
{
  if (graph.nodeCount() == 0u)
  {
    if (error != nullptr)
    {
      *error = "the graph is empty";
    }
    return false;
  }
  if (!(settings.channelResponse > 0.0) || !(settings.iterations > 0u))
  {
    if (error != nullptr)
    {
      *error = "erodibility and iteration count must be positive";
    }
    return false;
  }

  FluvialReport local{};
  const size_t count = graph.nodeCount();
  std::vector<double> before(count, 0.0);

  double reliefBefore = settings.seaLevelMetres;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined() && node.land)
    {
      reliefBefore = std::max(reliefBefore, node.intent.elevationMetres);
    }
  }
  local.reliefBefore = reliefBefore - settings.seaLevelMetres;

  // Discard the analytic relief first, if asked. What follows then builds the
  // ranges rather than eroding ones already drawn.
  if (settings.flattenBeforeErosion > 0.0)
  {
    const double base = settings.seaLevelMetres + local.reliefBefore * settings.flattenTargetFraction;
    const double amount = std::clamp(settings.flattenBeforeErosion, 0.0, 1.0);
    for (uint32_t id = 0u; id < count; ++id)
    {
      MapNode &node = graph.node(id);
      if (node.refined() || !node.land)
      {
        continue;
      }
      node.intent.elevationMetres += (base - node.intent.elevationMetres) * amount;
      node.intent.elevationMetres = std::max(node.intent.elevationMetres, settings.seaLevelMetres + settings.minimumLandElevationMetres);
    }
  }

  // Captured after any flattening: this is the surface the solver starts from,
  // and comparing against the pre-flattened one would credit erosion with the
  // flattening's work.
  for (size_t id = 0u; id < count; ++id)
  {
    before[id] = graph.node(static_cast<uint32_t>(id)).intent.elevationMetres;
  }

  std::vector<uint32_t> receiver(count, kInvalidNode);
  std::vector<double> area(count, 0.0);
  std::vector<uint32_t> order;
  order.reserve(count);

  const auto refreshDrainage = [&]() -> bool {
    RiverNetwork network;
    std::string drainageError;
    MapGraph probe = graph;
    if (!buildRivers(riverSettings, probe, network, {}, &drainageError))
    {
      return false;
    }
    for (size_t id = 0u; id < count; ++id)
    {
      receiver[id] = network.nodes[id].downstream;
      area[id] = network.nodes[id].drainageAreaSquareMetres;
    }

    // Topological order, outlets first. Braun and Willett's stack: updating a
    // cell after its receiver is what makes the implicit stream-power step
    // solvable in one sweep instead of iteratively.
    std::vector<std::vector<uint32_t>> donors(count);
    order.clear();
    for (uint32_t id = 0u; id < count; ++id)
    {
      if (graph.node(id).refined())
      {
        continue;
      }
      if (receiver[id] == kInvalidNode || receiver[id] >= count)
      {
        order.push_back(id);
      }
      else
      {
        donors[receiver[id]].push_back(id);
      }
    }
    for (size_t at = 0u; at < order.size(); ++at)
    {
      for (const uint32_t donor : donors[order[at]])
      {
        order.push_back(donor);
      }
    }
    ++local.drainageRebuilds;
    return true;
  };

  if (!refreshDrainage())
  {
    if (error != nullptr)
    {
      *error = "the fluvial solver could not derive a drainage network";
    }
    return false;
  }

  // Calibrate erodibility against this world rather than against a constant.
  // The median of A^m / L is what a typical channel presents to the solver, so
  // dividing the requested response by it makes "move a median channel 14% of
  // the way to its receiver per step" true at any world size.
  std::vector<double> coefficients;
  coefficients.reserve(count);
  for (uint32_t id = 0u; id < count; ++id)
  {
    const MapNode &node = graph.node(id);
    if (node.refined() || !node.land)
    {
      continue;
    }
    const uint32_t downstream = receiver[id];
    if (downstream == kInvalidNode || downstream >= count)
    {
      continue;
    }
    coefficients.push_back(std::pow(std::max(1.0, area[id]), settings.areaExponent) / reachLength(node, graph.node(downstream)));
  }
  double streamPowerK = 1.0;
  if (!coefficients.empty())
  {
    const size_t middle = coefficients.size() / 2u;
    std::nth_element(coefficients.begin(), coefficients.begin() + middle, coefficients.end());
    const double median = std::max(1.0e-9, coefficients[middle]);
    // C = k * median, and we want C such that h moves `channelResponse` of the
    // way: C / (1 + C) = response.
    const double response = std::clamp(settings.channelResponse, 0.0, 0.95);
    streamPowerK = (response / (1.0 - response)) / median;
  }
  local.resolvedStreamPowerK = streamPowerK;

  // Uplift, normalised the same way: the strongest uplift on the map moves by
  // `upliftScale` metres per step, whatever units the intent happens to be in.
  double peakUplift = 0.0;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined() && node.land)
    {
      peakUplift = std::max(peakUplift, node.intent.upliftRate);
    }
  }
  const double upliftNormalisation = peakUplift > 0.0 ? settings.upliftScale / peakUplift : 0.0;

  for (uint32_t iteration = 0u; iteration < settings.iterations; ++iteration)
  {
    if (iteration != 0u && settings.drainageRefreshInterval != 0u && iteration % settings.drainageRefreshInterval == 0u)
    {
      refreshDrainage();
    }

    // Uplift. This is what the incision works against, and without it the
    // solver simply flattens everything it is given - which is exactly how a
    // range placed by hand disappears under an erosion pass.
    for (uint32_t id = 0u; id < count; ++id)
    {
      MapNode &node = graph.node(id);
      if (node.refined() || !node.land)
      {
        continue;
      }
      node.intent.elevationMetres += node.intent.upliftRate * upliftNormalisation;
    }

    // Implicit stream power, one sweep down the stack. With n = 1,
    //   h_i' = (h_i + C h_r') / (1 + C),   C = k A^m / L,
    // which is unconditionally stable and cannot overshoot past the receiver.
    for (const uint32_t id : order)
    {
      MapNode &node = graph.node(id);
      if (node.refined() || !node.land)
      {
        continue;
      }
      const uint32_t downstream = receiver[id];
      if (downstream == kInvalidNode || downstream >= count)
      {
        continue;
      }
      const MapNode &target = graph.node(downstream);
      const double length = reachLength(node, target);
      const double coefficient = streamPowerK * std::pow(std::max(1.0, area[id]), settings.areaExponent) / length;
      const double receiverHeight = target.intent.elevationMetres;
      double height = (node.intent.elevationMetres + coefficient * receiverHeight) / (1.0 + coefficient);
      // A cell may not fall below what it drains into. The implicit form
      // cannot overshoot, but depression filling and the uplift step can still
      // leave an inversion, and a channel that runs uphill is not a channel.
      if (height < receiverHeight)
      {
        height = receiverHeight;
        ++local.inversionsClamped;
      }
      node.intent.elevationMetres = std::max(height, settings.seaLevelMetres + settings.minimumLandElevationMetres);
    }

    // Hillslope diffusion. Stream power alone leaves knife-edge divides,
    // because nothing acts on a cell that is nobody's receiver.
    if (settings.diffusion > 0.0)
    {
      std::vector<double> smoothed(count, 0.0);
      for (uint32_t id = 0u; id < count; ++id)
      {
        const MapNode &node = graph.node(id);
        smoothed[id] = node.intent.elevationMetres;
        if (node.refined() || !node.land || node.neighbours.empty())
        {
          continue;
        }
        double sum = 0.0;
        uint32_t used = 0u;
        for (const uint32_t neighbourId : node.neighbours)
        {
          const MapNode &neighbour = graph.node(neighbourId);
          if (neighbour.refined() || !neighbour.land)
          {
            continue;
          }
          sum += neighbour.intent.elevationMetres;
          ++used;
        }
        if (used == 0u)
        {
          continue;
        }
        const double mean = sum / static_cast<double>(used);
        smoothed[id] = node.intent.elevationMetres + (mean - node.intent.elevationMetres) * settings.diffusion;
      }
      for (uint32_t id = 0u; id < count; ++id)
      {
        MapNode &node = graph.node(id);
        if (node.refined() || !node.land)
        {
          continue;
        }
        node.intent.elevationMetres = std::max(smoothed[id], settings.seaLevelMetres + settings.minimumLandElevationMetres);
      }
    }
    ++local.iterations;
  }

  // Pearson correlation of uplift against elevation, over land cells.
  const auto correlation = [&graph, &count](const std::vector<double> &heights) {
    double sumU = 0.0;
    double sumH = 0.0;
    uint32_t used = 0u;
    for (uint32_t id = 0u; id < count; ++id)
    {
      const MapNode &node = graph.node(id);
      if (node.refined() || !node.land)
      {
        continue;
      }
      sumU += node.intent.upliftRate;
      sumH += heights[id];
      ++used;
    }
    if (used < 2u)
    {
      return 0.0;
    }
    const double meanU = sumU / used;
    const double meanH = sumH / used;
    double covariance = 0.0;
    double varianceU = 0.0;
    double varianceH = 0.0;
    for (uint32_t id = 0u; id < count; ++id)
    {
      const MapNode &node = graph.node(id);
      if (node.refined() || !node.land)
      {
        continue;
      }
      const double du = node.intent.upliftRate - meanU;
      const double dh = heights[id] - meanH;
      covariance += du * dh;
      varianceU += du * du;
      varianceH += dh * dh;
    }
    const double denominator = std::sqrt(varianceU * varianceH);
    return denominator > 0.0 ? covariance / denominator : 0.0;
  };
  std::vector<double> after(count, 0.0);
  for (uint32_t id = 0u; id < count; ++id)
  {
    after[id] = graph.node(id).intent.elevationMetres;
  }
  local.upliftCorrelationBefore = correlation(before);
  local.upliftCorrelationAfter = correlation(after);

  double reliefAfter = settings.seaLevelMetres;
  double erosionSum = 0.0;
  uint32_t landCells = 0u;
  for (uint32_t id = 0u; id < count; ++id)
  {
    const MapNode &node = graph.node(id);
    if (node.refined() || !node.land)
    {
      continue;
    }
    reliefAfter = std::max(reliefAfter, node.intent.elevationMetres);
    const double moved = before[id] - node.intent.elevationMetres;
    erosionSum += moved;
    local.maximumErosionMetres = std::max(local.maximumErosionMetres, moved);
    ++landCells;
  }
  local.reliefAfter = reliefAfter - settings.seaLevelMetres;
  local.meanErosionMetres = landCells != 0u ? erosionSum / static_cast<double>(landCells) : 0.0;
  if (report != nullptr)
  {
    *report = local;
  }
  return true;
}

} // namespace procedural::world
