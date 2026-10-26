#include "procedural/world/MapPlateaus.hpp"

#include "procedural/world/MapElevation.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace procedural::world
{

namespace
{

uint64_t mix(uint64_t value)
{
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;
  return value;
}

double unitNoise(const uint64_t seed, const uint32_t id)
{
  return static_cast<double>(mix(seed ^ (static_cast<uint64_t>(id) * 0x9e3779b9ull)) >>
             11u) /
      static_cast<double>(1ull << 53u);
}

} // namespace

bool buildPlateaus(
    const PlateauSettings &settings,
    MapGraph &graph,
    PlateauField &field,
    std::string *error)
{
  field = PlateauField{};
  if (graph.nodeCount() == 0u)
  {
    if (error != nullptr)
      *error = "the graph is empty";
    return false;
  }
  if (!(settings.ladderIntervalMetres > 0.0))
  {
    if (error != nullptr)
      *error = "a positive ladder interval is required";
    return false;
  }
  if (settings.minimumRungs == 0u || settings.maximumRungs < settings.minimumRungs)
  {
    if (error != nullptr)
      *error = "the rung range is empty";
    return false;
  }
  const double interval = settings.ladderIntervalMetres;
  field.ladderIntervalMetres = interval;
  field.cellBlock.assign(graph.nodeCount(), kInvalidNode);

  // Local relief per cell: how far the ground around it moves. A block wants
  // ground that is already close to flat, because flattening a slope is what
  // makes a wall rather than a mesa.
  std::vector<double> localRelief(graph.nodeCount(), 0.0);
  std::vector<double> score(graph.nodeCount(), -1.0);
  double landArea = 0.0;
  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
  {
    const MapNode &node = graph.node(id);
    if (node.refined() || !node.land || node.coastal)
      continue;
    landArea += cellArea(node);
    double lowest = node.intent.elevationMetres;
    double highest = node.intent.elevationMetres;
    bool touchesWater = false;
    for (const uint32_t neighbourId : node.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined())
        continue;
      if (!neighbour.land)
      {
        touchesWater = true;
        continue;
      }
      lowest = std::min(lowest, neighbour.intent.elevationMetres);
      highest = std::max(highest, neighbour.intent.elevationMetres);
    }
    localRelief[id] = highest - lowest;
    if (touchesWater || localRelief[id] > settings.maximumLocalReliefMetres)
      continue;

    // Prefer ground that is already tabular. A plateau cell scores highest, a
    // plain next, a mountainside not at all: the landform the elevation stage
    // measured is better evidence than anything reconstructed here.
    double typeWeight = 0.0;
    switch (node.type)
    {
    case CellType::Plateau: typeWeight = 1.0; break;
    case CellType::Hill: typeWeight = 0.62; break;
    case CellType::Plain: typeWeight = 0.5; break;
    case CellType::Desert: typeWeight = 0.45; break;
    case CellType::Rocks: typeWeight = 0.4; break;
    default: typeWeight = 0.0; break;
    }
    if (typeWeight <= 0.0)
      continue;
    const double flatness = 1.0 -
        std::clamp(localRelief[id] / std::max(1.0, settings.maximumLocalReliefMetres),
            0.0, 1.0);
    // A little noise so blocks do not all start on the same kind of cell and
    // tile the world; deterministic in the seed and the node, never in order.
    score[id] = typeWeight * 0.55 + flatness * 0.35 +
        unitNoise(settings.seed, id) * 0.10;
  }

  std::vector<uint32_t> seeds;
  seeds.reserve(graph.nodeCount());
  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
    if (score[id] > 0.0)
      seeds.push_back(id);
  std::sort(seeds.begin(), seeds.end(),
      [&score](const uint32_t left, const uint32_t right)
      {
        if (score[left] != score[right])
          return score[left] > score[right];
        return left < right;
      });

  const double areaBudget = landArea * std::clamp(settings.coverageFraction, 0.0, 1.0);
  double claimed = 0.0;
  double riserTotal = 0.0;

  for (const uint32_t seed : seeds)
  {
    if (claimed >= areaBudget)
      break;
    if (field.cellBlock[seed] != kInvalidNode)
      continue;

    // Grow by best-first over the neighbour graph. Crossing a craton seam costs
    // the candidate its place: a seam is where the ground actually changes, and
    // a block that straddles one is two mesas drawn as one.
    PlateauBlock block{};
    block.id = static_cast<uint32_t>(field.blocks.size());
    const uint16_t craton = graph.node(seed).craton;
    std::vector<uint32_t> pending{seed};
    std::vector<uint8_t> queued(graph.nodeCount(), 0u);
    queued[seed] = 1u;
    double blockArea = 0.0;
    double elevationSum = 0.0;
    while (!pending.empty() && blockArea < settings.targetBlockAreaSquareMetres)
    {
      // Deterministic pick: highest score, node id as the tie-break.
      auto best = pending.begin();
      for (auto candidate = pending.begin(); candidate != pending.end(); ++candidate)
        if (score[*candidate] > score[*best] ||
            (score[*candidate] == score[*best] && *candidate < *best))
          best = candidate;
      const uint32_t id = *best;
      pending.erase(best);
      if (field.cellBlock[id] != kInvalidNode || score[id] <= 0.0)
        continue;
      if (graph.node(id).craton != craton)
        continue;
      field.cellBlock[id] = block.id;
      block.cells.push_back(id);
      const double area = cellArea(graph.node(id));
      blockArea += area;
      elevationSum += graph.node(id).intent.elevationMetres * area;
      for (const uint32_t neighbourId : graph.node(id).neighbours)
      {
        if (queued[neighbourId] != 0u || field.cellBlock[neighbourId] != kInvalidNode)
          continue;
        if (score[neighbourId] <= 0.0)
          continue;
        queued[neighbourId] = 1u;
        pending.push_back(neighbourId);
      }
    }

    if (block.cells.size() < settings.minimumBlockCells)
    {
      for (const uint32_t id : block.cells)
        field.cellBlock[id] = kInvalidNode;
      continue;
    }

    // The ground immediately outside decides the riser. Snapping the block to
    // the ladder is what makes the brink land on a contour.
    double outsideSum = 0.0;
    uint32_t outsideCount = 0u;
    for (const uint32_t id : block.cells)
      for (const uint32_t neighbourId : graph.node(id).neighbours)
      {
        if (field.cellBlock[neighbourId] == block.id)
          continue;
        const MapNode &neighbour = graph.node(neighbourId);
        if (neighbour.refined() || !neighbour.land)
          continue;
        outsideSum += neighbour.intent.elevationMetres;
        ++outsideCount;
      }
    const double mean = elevationSum / std::max(1.0, blockArea);
    const double outside = outsideCount != 0u
        ? outsideSum / static_cast<double>(outsideCount)
        : mean;
    const uint32_t rungSpan = settings.minimumRungs +
        static_cast<uint32_t>(unitNoise(settings.seed ^ 0x51ed270bu, block.id) *
            static_cast<double>(settings.maximumRungs - settings.minimumRungs + 1u));
    const uint32_t rungs = std::clamp(rungSpan, settings.minimumRungs,
        settings.maximumRungs);
    const double base = std::floor(std::max(mean, outside) / interval) * interval;
    block.elevationMetres = base + static_cast<double>(rungs) * interval;
    block.rung = static_cast<int32_t>(std::llround(block.elevationMetres / interval));
    block.riserMetres = block.elevationMetres - outside;
    block.areaSquareMetres = blockArea;

    for (const uint32_t id : block.cells)
    {
      MapNode &node = graph.node(id);
      node.intent.elevationMetres = block.elevationMetres;
      node.type = CellType::Plateau;
      node.content = CellType::Plateau;
      // A flat top has no internal relief left to express.
      node.intent.reliefAmplitude = std::min(node.intent.reliefAmplitude, interval * 0.1);
    }
    claimed += blockArea;
    riserTotal += block.riserMetres;
    field.flattenedCells += static_cast<uint32_t>(block.cells.size());
    field.blocks.push_back(std::move(block));
  }

  if (!field.blocks.empty())
    riserTotal /= static_cast<double>(field.blocks.size());
  field.meanRiserMetres = riserTotal;
  return true;
}

} // namespace procedural::world
