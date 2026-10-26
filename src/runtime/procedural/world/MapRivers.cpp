#include "procedural/world/MapRivers.hpp"

#include "procedural/world/MapElevation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>

namespace procedural::world
{

namespace
{

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kSecondsPerYear = 365.25 * 24.0 * 60.0 * 60.0;

struct FloodEntry
{
  double elevation = 0.0;
  uint32_t node = kInvalidNode;

  // Cheapest first out of a max-heap, ties on node id so the flood order - and
  // so every filled elevation - is reproducible.
  bool operator<(const FloodEntry &other) const
  {
    if (elevation != other.elevation)
      return elevation > other.elevation;
    return node > other.node;
  }
};

double distanceBetween(const MapNode &a, const MapNode &b)
{
  const double dx = a.site.x - b.site.x;
  const double dy = a.site.y - b.site.y;
  return std::sqrt(dx * dx + dy * dy);
}

} // namespace

uint32_t fillLakes(
    const RiverNetwork &network,
    const LakeSettings &settings,
    const std::vector<double> &moisture,
    MapGraph &graph)
{
  if (network.nodes.size() != graph.nodeCount() ||
      !(settings.minimumDepthMetres > 0.0) ||
      !(settings.minimumMoisture >= 0.0) ||
      !(settings.minimumInflowFraction >= 0.0) ||
      !(settings.maximumAreaSquareMetres >= 0.0) ||
      !(settings.maximumCatchmentFraction >= 0.0))
    return 0u;

  const bool testMoisture = moisture.size() == graph.nodeCount();
  const double inflowThreshold =
      network.largestDischargeCumecs * settings.minimumInflowFraction;

  // Find whole filled basins before deciding which cells contain permanent
  // water. The old cell-by-cell gate flooded every qualifying shelf of a broad
  // depression, producing oversized block-shaped lakes.
  std::vector<uint8_t> candidate(graph.nodeCount(), 0u);
  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
  {
    const MapNode &node = graph.node(id);
    if (node.refined() || !node.land)
      continue;
    const double depth = network.nodes[id].filledElevationMetres -
        node.intent.elevationMetres;
    if (depth < settings.minimumDepthMetres)
      continue;
    if (testMoisture && moisture[id] < settings.minimumMoisture)
      continue;
    candidate[id] = 1u;
  }

  uint32_t flooded = 0u;
  std::vector<uint8_t> visited(graph.nodeCount(), 0u);
  for (uint32_t seed = 0u; seed < graph.nodeCount(); ++seed)
  {
    if (candidate[seed] == 0u || visited[seed] != 0u)
      continue;

    std::vector<uint32_t> basin;
    std::vector<uint32_t> stack{seed};
    visited[seed] = 1u;
    double basinInflow = 0.0;
    double catchmentArea = 0.0;
    while (!stack.empty())
    {
      const uint32_t current = stack.back();
      stack.pop_back();
      basin.push_back(current);
      if (network.nodes[current].dischargeCumecs > basinInflow)
      {
        basinInflow = network.nodes[current].dischargeCumecs;
        catchmentArea = network.nodes[current].drainageAreaSquareMetres;
      }
      for (const uint32_t neighbour : graph.node(current).neighbours)
        if (candidate[neighbour] != 0u && visited[neighbour] == 0u)
        {
          visited[neighbour] = 1u;
          stack.push_back(neighbour);
        }
    }
    if (basinInflow < inflowThreshold)
      continue;

    double areaLimit = kInfinity;
    if (settings.maximumAreaSquareMetres > 0.0)
      areaLimit = settings.maximumAreaSquareMetres;
    if (settings.maximumCatchmentFraction > 0.0 && catchmentArea > 0.0)
      areaLimit = std::min(areaLimit,
          catchmentArea * settings.maximumCatchmentFraction);
    // A graph cell is indivisible. Always allow the deepest cell so a valid
    // basin does not disappear merely because the preview resolution is coarse.
    const auto deeper = [&network, &graph](const uint32_t lhs, const uint32_t rhs)
    {
      const double leftDepth = network.nodes[lhs].filledElevationMetres -
          graph.node(lhs).intent.elevationMetres;
      const double rightDepth = network.nodes[rhs].filledElevationMetres -
          graph.node(rhs).intent.elevationMetres;
      if (leftDepth != rightDepth)
        return leftDepth < rightDepth;
      return lhs > rhs;
    };
    const uint32_t deepest =
        *std::max_element(basin.begin(), basin.end(), deeper);

    std::priority_queue<uint32_t, std::vector<uint32_t>, decltype(deeper)> frontier(deeper);
    std::vector<uint8_t> queued(graph.nodeCount(), 0u);
    frontier.push(deepest);
    queued[deepest] = 1u;
    double lakeArea = 0.0;
    while (!frontier.empty())
    {
      const uint32_t id = frontier.top();
      frontier.pop();
      const double area = cellArea(graph.node(id));
      if (lakeArea > 0.0 && lakeArea + area > areaLimit)
        continue;
      MapNode &node = graph.node(id);
      node.land = false;
      node.type = CellType::Lake;
      node.content = CellType::Lake;
      node.intent.elevationMetres = network.nodes[id].filledElevationMetres;
      lakeArea += area;
      ++flooded;
      for (const uint32_t neighbour : node.neighbours)
        if (candidate[neighbour] != 0u && visited[neighbour] != 0u &&
            queued[neighbour] == 0u)
        {
          queued[neighbour] = 1u;
          frontier.push(neighbour);
        }
    }
  }
  if (flooded == 0u)
    return 0u;

  // Coastal and shallow are derived, and a new lake changes both for everything
  // around it.
  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
  {
    MapNode &node = graph.node(id);
    if (node.refined())
      continue;
    node.coastal = false;
    node.shallow = false;
    for (const uint32_t neighbourId : node.neighbours)
    {
      if (graph.node(neighbourId).refined())
        continue;
      const bool neighbourLand = graph.node(neighbourId).land;
      if (node.land && !neighbourLand)
        node.coastal = true;
      if (!node.land && neighbourLand)
        node.shallow = true;
    }
  }
  return flooded;
}

bool buildRivers(
    const RiverSettings &settings,
    MapGraph &graph,
    RiverNetwork &network,
    const std::vector<double> &runoffScale,
    std::string *error)
{
  const auto fail = [&](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };

  network = RiverNetwork{};
  if (graph.nodeCount() == 0u)
    return fail("cannot build rivers on an empty graph");
  if (!(settings.runoffMetresPerYear >= 0.0))
    return fail("runoff must not be negative");
  if (!(settings.riverThresholdCells > 0.0))
    return fail("river threshold must be positive");
  if (!(settings.channelReferenceGradient > 0.0))
    return fail("channel reference gradient must be positive");
  if (!(settings.channelSlopeExponent >= 0.0))
    return fail("channel slope exponent must not be negative");
  if (settings.minimumRiverSystemCells == 0u)
    return fail("minimum river system length must be positive");
  if (settings.waterfallSeparationCells == 0u)
    return fail("waterfall separation must be positive");
  if (!(settings.fillEpsilonMetres > 0.0))
    return fail("fill epsilon must be positive");
  if (!std::isfinite(settings.canyonMinimumBankRiseFraction) ||
      settings.canyonMinimumBankRiseFraction < 0.0 ||
      settings.canyonMinimumBankRiseFraction > 1.0)
    return fail("canyon minimum bank rise fraction must be within [0, 1]");
  if (!std::isfinite(settings.canyonMinimumBankGradient) ||
      settings.canyonMinimumBankGradient < 0.0)
    return fail("canyon minimum bank gradient must not be negative");

  const size_t count = graph.nodeCount();
  network.nodes.assign(count, RiverNode{});

  // --- depression filling -------------------------------------------------
  //
  // Every water cell is an outlet and holds its own elevation. Land is flooded
  // inward from them, each cell taking the higher of its own elevation and one
  // epsilon above whatever reached it. The result has no interior minimum, so
  // descent from anywhere terminates at water.
  std::vector<uint8_t> settled(count, 0u);
  std::priority_queue<FloodEntry> queue;
  uint32_t outlets = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || node.land)
      continue;
    network.nodes[node.id].filledElevationMetres = node.intent.elevationMetres;
    settled[node.id] = 1u;
    ++outlets;
    queue.push(FloodEntry{node.intent.elevationMetres, node.id});
  }
  if (outlets == 0u)
  {
    // A world with no water at all - landlocked at a land fraction of one. The
    // world border stands in for the sea, so the drainage still terminates
    // somewhere rather than the whole map being one endorheic basin.
    double lowest = kInfinity;
    uint32_t lowestNode = kInvalidNode;
    for (const MapNode &node : graph.nodes())
      if (node.intent.elevationMetres < lowest ||
          (node.intent.elevationMetres == lowest && node.id < lowestNode))
      {
        lowest = node.intent.elevationMetres;
        lowestNode = node.id;
      }
    if (lowestNode == kInvalidNode)
      return fail("no outlet could be found for the drainage network");
    network.nodes[lowestNode].filledElevationMetres = lowest;
    settled[lowestNode] = 1u;
    queue.push(FloodEntry{lowest, lowestNode});
  }

  while (!queue.empty())
  {
    const FloodEntry entry = queue.top();
    queue.pop();
    for (const uint32_t neighbourId : graph.node(entry.node).neighbours)
    {
      if (settled[neighbourId] != 0u || graph.node(neighbourId).refined())
        continue;
      const double own = graph.node(neighbourId).intent.elevationMetres;
      const double filled =
          std::max(own, entry.elevation + settings.fillEpsilonMetres);
      if (filled > own)
        ++network.filledCells;
      network.nodes[neighbourId].filledElevationMetres = filled;
      settled[neighbourId] = 1u;
      queue.push(FloodEntry{filled, neighbourId});
    }
  }

  // A cell the flood never reached is an island of the neighbour graph with no
  // path to any outlet. Leaving it silently would produce a cell whose water
  // goes nowhere, so it is treated as its own sink rather than pretended about.
  for (size_t id = 0u; id < count; ++id)
    if (settled[id] == 0u)
      network.nodes[id].filledElevationMetres =
          graph.node(static_cast<uint32_t>(id)).intent.elevationMetres;

  // --- flow directions ----------------------------------------------------
  //
  // Steepest descent on the filled surface. The filling guarantees a land cell
  // has a strictly lower neighbour, so this always finds one; every step
  // strictly decreases the filled elevation, so a course cannot cycle.
  std::vector<Waterfall> waterfallCandidates;
  for (uint32_t id = 0u; id < count; ++id)
  {
    const MapNode &node = graph.node(id);
    if (node.refined() || !node.land)
      continue;
    double steepest = 0.0;
    uint32_t best = kInvalidNode;
    for (const uint32_t neighbourId : node.neighbours)
    {
      if (graph.node(neighbourId).refined())
        continue;
      const double drop = network.nodes[id].filledElevationMetres -
          network.nodes[neighbourId].filledElevationMetres;
      if (drop <= 0.0)
        continue;
      const double span =
          std::max(1.0, distanceBetween(node, graph.node(neighbourId)));
      const double gradient = drop / span;
      // Ties on node id, so the network does not depend on neighbour order.
      if (gradient > steepest || (gradient == steepest && neighbourId < best))
      {
        steepest = gradient;
        best = neighbourId;
      }
    }
    network.nodes[id].downstream = best;
  }

  // --- accumulation -------------------------------------------------------
  //
  // Processed from the highest filled elevation down, so a cell's own drainage
  // is complete before it is passed on. This is a topological order of the
  // flow tree for free: downstream is always strictly lower.
  std::vector<uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(),
      [&network](const uint32_t lhs, const uint32_t rhs)
      {
        if (network.nodes[lhs].filledElevationMetres !=
            network.nodes[rhs].filledElevationMetres)
          return network.nodes[lhs].filledElevationMetres >
              network.nodes[rhs].filledElevationMetres;
        return lhs < rhs;
      });

  const double runoffPerSecond =
      settings.runoffMetresPerYear / kSecondsPerYear;
  const bool scaled = runoffScale.size() == count;
  double totalLandArea = 0.0;
  uint32_t landCells = 0u;
  for (const MapNode &node : graph.nodes())
    if (!node.refined() && node.land)
    {
      const double area = cellArea(node);
      network.nodes[node.id].drainageAreaSquareMetres = area;
      // Water yielded here, rather than area alone. A desert cell yields almost
      // none, so a catchment made of desert never reaches the channel threshold
      // and the map has no river through it - which is the whole difference
      // between a dry-looking desert and a real one.
      const double wetness = scaled
          ? std::clamp(runoffScale[node.id], 0.0, 1.0)
          : 1.0;
      const double factor = settings.aridRunoffFraction +
          (1.0 - settings.aridRunoffFraction) * wetness;
      network.nodes[node.id].dischargeCumecs = area * runoffPerSecond * factor;
      totalLandArea += area;
      ++landCells;
    }
  const double meanCellArea =
      landCells == 0u ? 0.0 : totalLandArea / static_cast<double>(landCells);
  const double riverThreshold =
      meanCellArea * settings.riverThresholdCells;

  for (const uint32_t id : order)
  {
    const uint32_t downstream = network.nodes[id].downstream;
    if (downstream == kInvalidNode)
      continue;
    network.nodes[downstream].drainageAreaSquareMetres +=
        network.nodes[id].drainageAreaSquareMetres;
    network.nodes[downstream].dischargeCumecs +=
        network.nodes[id].dischargeCumecs;
  }
  // The threshold is on a stream-power proxy, not area alone. On area a desert
  // basin the size of a wet one grows the same river; on discharge alone every
  // broad low-gradient collector becomes a channel at the same scale as a
  // steep headwater. Discharge times a modest slope term is a stable channel-
  // initiation test while keeping the accumulated discharge itself physical.
  const double dischargeThreshold = riverThreshold * runoffPerSecond;
  std::vector<uint8_t> channelHead(count, 0u);
  for (uint32_t id = 0u; id < count; ++id)
  {
    const MapNode &node = graph.node(id);
    if (node.refined() || !node.land)
      continue;
    const uint32_t downstream = network.nodes[id].downstream;
    if (downstream == kInvalidNode)
      continue;
    const double drop = network.nodes[id].filledElevationMetres -
        network.nodes[downstream].filledElevationMetres;
    const double span = std::max(1.0, distanceBetween(node, graph.node(downstream)));
    const double gradient = std::max(0.0, drop / span);
    const double slopeFactor = std::pow(
        std::clamp(gradient / settings.channelReferenceGradient, 0.15, 4.0),
        settings.channelSlopeExponent);
    channelHead[id] = network.nodes[id].dischargeCumecs * slopeFactor >=
        dischargeThreshold;
  }

  // A channel, once initiated, remains a channel downstream even where it
  // crosses a flat floodplain. This also makes the visible river graph
  // contiguous by construction.
  for (uint32_t id = 0u; id < count; ++id)
    if (channelHead[id] != 0u)
    {
      uint32_t current = id;
      while (current != kInvalidNode && graph.node(current).land)
      {
        if (network.nodes[current].river)
          break;
        network.nodes[current].river = true;
        current = network.nodes[current].downstream;
      }
    }

  // A one-cell coastal trickle is visually a loose blue dash, not a
  // river system. Group channels by their final land reach and remove only
  // those tiny independent systems; short tributaries attached to a real basin
  // remain part of that basin and survive.
  std::vector<uint32_t> terminal(count, kInvalidNode);
  std::vector<uint32_t> systemSize(count, 0u);
  for (uint32_t id = 0u; id < count; ++id)
  {
    if (!network.nodes[id].river)
      continue;
    uint32_t current = id;
    while (network.nodes[current].downstream != kInvalidNode &&
        graph.node(network.nodes[current].downstream).land)
      current = network.nodes[current].downstream;
    // Key by the final land reach, not its water neighbour: two unrelated
    // coastal trickles can enter the same coarse ocean cell and must not make
    // each other look like one larger river system.
    terminal[id] = current;
    ++systemSize[current];
  }
  for (uint32_t id = 0u; id < count; ++id)
    if (network.nodes[id].river &&
        systemSize[terminal[id]] < settings.minimumRiverSystemCells)
      network.nodes[id].river = false;

  for (uint32_t id = 0u; id < count; ++id)
    if (network.nodes[id].river)
    {
      ++network.riverCells;
      network.largestDischargeCumecs = std::max(
          network.largestDischargeCumecs, network.nodes[id].dischargeCumecs);
    }

  // --- Strahler order -----------------------------------------------------
  //
  // Only channels count. A hillslope cell contributing to a river does not
  // raise its order, or every confluence in the world would be order two.
  for (const uint32_t id : order)
  {
    if (!network.nodes[id].river)
      continue;
    uint32_t highest = 0u;
    uint32_t highestCount = 0u;
    for (const uint32_t neighbourId : graph.node(id).neighbours)
    {
      if (network.nodes[neighbourId].downstream != id ||
          !network.nodes[neighbourId].river)
        continue;
      const uint32_t neighbourOrder = network.nodes[neighbourId].strahlerOrder;
      if (neighbourOrder > highest)
      {
        highest = neighbourOrder;
        highestCount = 1u;
      }
      else if (neighbourOrder == highest)
        ++highestCount;
    }
    network.nodes[id].strahlerOrder =
        highest == 0u ? 1u : (highestCount >= 2u ? highest + 1u : highest);
    network.maximumOrder =
        std::max(network.maximumOrder, network.nodes[id].strahlerOrder);
  }

  // --- waterfalls ---------------------------------------------------------
  //
  // Measured on the real surface rather than the filled one: filling flattens a
  // basin, and the step at its outlet is exactly the kind of place a fall is.
  for (uint32_t id = 0u; id < count; ++id)
  {
    if (!network.nodes[id].river)
      continue;
    const uint32_t downstream = network.nodes[id].downstream;
    if (downstream == kInvalidNode)
      continue;
    const double drop = graph.node(id).intent.elevationMetres -
        graph.node(downstream).intent.elevationMetres;
    if (drop < settings.waterfallMinimumDropMetres)
      continue;
    const double span =
        std::max(1.0, distanceBetween(graph.node(id), graph.node(downstream)));
    const double gradient = drop / span;
    if (gradient < settings.waterfallGradient)
      continue;
    if (network.nodes[id].dischargeCumecs <
        network.largestDischargeCumecs * settings.waterfallDischargeFraction)
      continue;
    waterfallCandidates.push_back(Waterfall{graph.node(id).site, drop, gradient,
      network.nodes[id].dischargeCumecs, id});
  }
  std::sort(waterfallCandidates.begin(), waterfallCandidates.end(),
      [](const Waterfall &lhs, const Waterfall &rhs)
      {
        const double leftStrength = lhs.dropMetres * std::sqrt(lhs.dischargeCumecs);
        const double rightStrength = rhs.dropMetres * std::sqrt(rhs.dischargeCumecs);
        if (leftStrength != rightStrength)
          return leftStrength > rightStrength;
        return lhs.node < rhs.node;
      });
  const auto shortlyDownstream = [&network, &settings](uint32_t from, const uint32_t target)
  {
    for (uint32_t step = 0u; step < settings.waterfallSeparationCells; ++step)
    {
      if (from == kInvalidNode)
        return false;
      from = network.nodes[from].downstream;
      if (from == target)
        return true;
    }
    return false;
  };
  for (const Waterfall &candidate : waterfallCandidates)
  {
    bool tooClose = false;
    for (const Waterfall &selected : network.waterfalls)
      if (shortlyDownstream(candidate.node, selected.node) ||
          shortlyDownstream(selected.node, candidate.node))
      {
        tooClose = true;
        break;
      }
    if (!tooClose)
      network.waterfalls.push_back(candidate);
  }
  std::sort(network.waterfalls.begin(), network.waterfalls.end(),
      [](const Waterfall &lhs, const Waterfall &rhs) { return lhs.node < rhs.node; });

  // --- courses and valleys ------------------------------------------------
  for (uint32_t id = 0u; id < count; ++id)
  {
    if (!network.nodes[id].river)
      continue;
    const uint32_t downstream = network.nodes[id].downstream;
    if (downstream == kInvalidNode)
      continue;
    const bool mouth = !graph.node(downstream).land;
    if (mouth)
      ++network.mouths;
    network.segments.push_back(RiverSegment{graph.node(id).site,
      graph.node(downstream).site, network.nodes[id].dischargeCumecs,
      network.nodes[id].strahlerOrder, id, mouth});
  }

  // A river crossing a plain is still a plain. Valleys and canyons are river
  // cells that their neighbours stand above; a canyon additionally has a
  // world-scale bank rise and a steep wall.
  const double minimumCanyonRise = graph.brief().reliefMetres *
      settings.canyonMinimumBankRiseFraction;
  for (uint32_t id = 0u; id < count; ++id)
  {
    const MapNode &node = graph.node(id);
    if (node.refined() || !node.land || !network.nodes[id].river || node.coastal)
      continue;
    double signedRise = 0.0;
    double positiveRise = 0.0;
    double steepestBank = 0.0;
    uint32_t neighbours = 0u;
    uint32_t higherNeighbours = 0u;
    for (const uint32_t neighbourId : node.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined() || !neighbour.land)
        continue;
      const double rise = neighbour.intent.elevationMetres -
          node.intent.elevationMetres;
      signedRise += rise;
      if (rise > 0.0)
      {
        positiveRise += rise;
        ++higherNeighbours;
        steepestBank = std::max(steepestBank,
            rise / std::max(1.0, distanceBetween(node, neighbour)));
      }
      ++neighbours;
    }
    if (neighbours == 0u)
      continue;
    const double meanRise = signedRise / static_cast<double>(neighbours);
    const double meanBankRise = higherNeighbours == 0u
        ? 0.0
        : positiveRise / static_cast<double>(higherNeighbours);
    if (meanBankRise > 0.0 && steepestBank > 0.0 &&
        meanBankRise >= minimumCanyonRise &&
        steepestBank >= settings.canyonMinimumBankGradient)
    {
      graph.node(id).type = CellType::Canyon;
      graph.node(id).content = CellType::Canyon;
    }
    else if (meanRise > node.intent.reliefAmplitude * 0.12)
    {
      graph.node(id).type = CellType::Valley;
      graph.node(id).content = CellType::Valley;
    }
  }
  return true;
}

} // namespace procedural::world
