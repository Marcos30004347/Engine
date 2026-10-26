#include "procedural/world/MapElevation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace procedural::world {

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

// The same integer-only hash the raisers use. Integer-only so it is
// bit-identical across targets, which determinism depends on.
uint64_t hashCombine(uint64_t value)
{
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;
  return value;
}

double hashUnit(const uint64_t seed, const int64_t x, const int64_t y)
{
  uint64_t value = seed;
  value = hashCombine(value ^ static_cast<uint64_t>(x) * 0x9e3779b97f4a7c15ull);
  value = hashCombine(value ^ static_cast<uint64_t>(y) * 0xbf58476d1ce4e5b9ull);
  return static_cast<double>(value >> 11u) / 9007199254740992.0;
}

double smoothStep(const double t)
{
  const double c = std::clamp(t, 0.0, 1.0);
  return c * c * (3.0 - 2.0 * c);
}

struct LandformProfile
{
  // Fractions of the world's relief budget. `broadBase` is the province's
  // floor and `inlandRise` is its remaining, deliberately small, regional
  // tilt. Range and detail activity decide how much fine relief survives.
  double broadBase = 0.0;
  double inlandRise = 0.0;
  double rangeActivity = 1.0;
  double detailActivity = 1.0;
  double plateauAffinity = 0.0;
};

LandformProfile blendProfile(const LandformProfile &a, const LandformProfile &b, const double weight)
{
  const auto mix = [weight](const double first, const double second) {
    return first * weight + second * (1.0 - weight);
  };
  return LandformProfile{mix(a.broadBase, b.broadBase), mix(a.inlandRise, b.inlandRise), mix(a.rangeActivity, b.rangeActivity), mix(a.detailActivity, b.detailActivity), mix(a.plateauAffinity, b.plateauAffinity)};
}

double valueNoise(const uint64_t seed, const double x, const double y)
{
  const double fx = std::floor(x);
  const double fy = std::floor(y);
  const int64_t ix = static_cast<int64_t>(fx);
  const int64_t iy = static_cast<int64_t>(fy);
  const double tx = smoothStep(x - fx);
  const double ty = smoothStep(y - fy);
  const double v00 = hashUnit(seed, ix, iy);
  const double v10 = hashUnit(seed, ix + 1, iy);
  const double v01 = hashUnit(seed, ix, iy + 1);
  const double v11 = hashUnit(seed, ix + 1, iy + 1);
  const double top = v00 + (v10 - v00) * tx;
  const double bottom = v01 + (v11 - v01) * tx;
  return top + (bottom - top) * ty;
}

double fractalNoise(const uint64_t seed, double x, double y)
{
  double sum = 0.0;
  double amplitude = 1.0;
  double total = 0.0;
  for (uint32_t octave = 0u; octave < 4u; ++octave)
  {
    sum += valueNoise(seed + octave * 7919u, x, y) * amplitude;
    total += amplitude;
    amplitude *= 0.5;
    x *= 2.0;
    y *= 2.0;
  }
  return sum / total;
}

double coastalPlainFor(const ElevationSettings &settings)
{
  return std::max(1.0, settings.coastalPlainMetres);
}

double distanceBetween(const MapNode &a, const MapNode &b)
{
  const double dx = a.site.x - b.site.x;
  const double dy = a.site.y - b.site.y;
  return std::sqrt(dx * dx + dy * dy);
}

struct QueueEntry
{
  double cost = 0.0;
  uint32_t node = kInvalidNode;

  // Greater-than so std::priority_queue, which is a max-heap, pops the
  // cheapest. Ties break on node id, so the traversal order - and therefore
  // every field derived from it - does not depend on the heap's internals.
  bool operator<(const QueueEntry &other) const
  {
    if (cost != other.cost)
    {
      return cost > other.cost;
    }
    return node > other.node;
  }
};

// Multi-source Dijkstra over the neighbour graph, restricted to cells passing
// `include`, seeded from every cell passing `source`.
template <typename SourceTest, typename IncludeTest> std::vector<double> graphDistance(const MapGraph &graph, const SourceTest &source, const IncludeTest &include, std::vector<uint32_t> *predecessor = nullptr)
{
  std::vector<double> distance(graph.nodeCount(), kInfinity);
  if (predecessor != nullptr)
  {
    predecessor->assign(graph.nodeCount(), kInvalidNode);
  }
  std::priority_queue<QueueEntry> queue;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined() && source(node))
    {
      distance[node.id] = 0.0;
      queue.push(QueueEntry{0.0, node.id});
    }
  }

  while (!queue.empty())
  {
    const QueueEntry entry = queue.top();
    queue.pop();
    if (entry.cost > distance[entry.node])
    {
      continue;
    }
    const MapNode &current = graph.node(entry.node);
    for (const uint32_t neighbourId : current.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined() || !include(neighbour))
      {
        continue;
      }
      const double cost = entry.cost + distanceBetween(current, neighbour);
      if (cost >= distance[neighbourId])
      {
        continue;
      }
      distance[neighbourId] = cost;
      if (predecessor != nullptr)
      {
        (*predecessor)[neighbourId] = entry.node;
      }
      queue.push(QueueEntry{cost, neighbourId});
    }
  }
  return distance;
}

// Land cells grouped into connected landmasses. An archipelago's islands must
// each get their own range spine, or every island but one comes out flat.
std::vector<std::vector<uint32_t>> landComponents(const MapGraph &graph)
{
  std::vector<std::vector<uint32_t>> components;
  std::vector<uint8_t> seen(graph.nodeCount(), 0u);
  std::vector<uint32_t> stack;
  for (const MapNode &start : graph.nodes())
  {
    if (start.refined() || !start.land || seen[start.id] != 0u)
    {
      continue;
    }
    components.emplace_back();
    seen[start.id] = 1u;
    stack.push_back(start.id);
    while (!stack.empty())
    {
      const uint32_t current = stack.back();
      stack.pop_back();
      components.back().push_back(current);
      for (const uint32_t neighbour : graph.node(current).neighbours)
      {
        if (!graph.node(neighbour).refined() && graph.node(neighbour).land && seen[neighbour] == 0u)
        {
          seen[neighbour] = 1u;
          stack.push_back(neighbour);
        }
      }
    }
    std::sort(components.back().begin(), components.back().end());
  }
  return components;
}

// Least-cost traversal over one landmass, preferring the interior.
//
// Crossing a cell near the coast costs several times what crossing a deep
// interior cell does, so a path between two inland points stays inland instead
// of cutting along the beach.
void interiorSweep(const MapGraph &graph, const std::vector<uint32_t> &sources, const std::vector<double> &coastDistance, const double scale, std::vector<double> &distance, std::vector<uint32_t> &predecessor)
{
  distance.assign(graph.nodeCount(), kInfinity);
  predecessor.assign(graph.nodeCount(), kInvalidNode);
  std::priority_queue<QueueEntry> queue;
  for (const uint32_t source : sources)
  {
    distance[source] = 0.0;
    queue.push(QueueEntry{0.0, source});
  }
  while (!queue.empty())
  {
    const QueueEntry entry = queue.top();
    queue.pop();
    if (entry.cost > distance[entry.node])
    {
      continue;
    }
    const MapNode &current = graph.node(entry.node);
    for (const uint32_t neighbourId : current.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined() || !neighbour.land)
      {
        continue;
      }
      // A cell the coast flood never reached has an infinite distance, and
      // infinity over an infinite scale is NaN. Every comparison against NaN is
      // false, so `cost >= distance` never rejects, every neighbour is
      // re-pushed forever and the sweep does not terminate - which is exactly
      // how a two-level hierarchy hung: its cross-parent links were incomplete,
      // some land could not reach water, and one NaN was enough.
      //
      // Treated as the most expensive kind of ground rather than skipped, so
      // the sweep still crosses it if it must.
      const double reach = coastDistance[neighbourId];
      const double interior = std::isfinite(reach) && std::isfinite(scale) ? std::clamp(reach / scale, 0.0, 1.0) : 0.0;
      const double cost = entry.cost + distanceBetween(current, neighbour) * (1.0 + 4.0 * (1.0 - interior));
      if (cost >= distance[neighbourId])
      {
        continue;
      }
      distance[neighbourId] = cost;
      predecessor[neighbourId] = entry.node;
      queue.push(QueueEntry{cost, neighbourId});
    }
  }
}

// Lays out one landmass's range spine and marks the cells on it.
//
// A mountain range is a *linear* feature, and that is the whole difficulty
// here. The obvious layout - take the maxima of the distance-to-coast field -
// gives exactly one seed on any convex landmass, so an island came out as a
// single central cone with perfectly radial drainage, which reads as a volcano
// rather than as a continent.
//
// So the spine follows the landmass's own long axis instead: a double sweep
// finds the two interior points furthest apart, and the least-cost path between
// them is the main range. Larger landmasses then grow branch ranges, each from
// the interior point currently furthest from the range back to it. Both are
// facts about the shape of the land, which is what keeps the layout - and not
// noise - deciding where the mountains are.
// Ranges on the seams of an orogenic plate diagram.
//
// A range is a collision, so it belongs on a seam between plates rather than
// down the middle of a landmass - taking the landmass's long axis gives one
// central ridge on any compact continent, which is where this started.
//
// The diagram is its own, independent of both the cell hierarchy and the blocks
// the landmass grew from. Sharing the growth's blocks was tried and is worse:
// that diagram also decides whether the land holds together, so spreading it to
// spread the ranges fragmented the continent.
bool markSutures(const MapGraph &graph, const std::vector<uint32_t> &component, const double spacing, const double fraction, const uint64_t seed, std::vector<uint8_t> &spine)
{
  if (!(spacing > 0.0))
  {
    return false;
  }

  const auto plateSite = [seed, spacing](const int64_t x, const int64_t y) {
    const double jx = hashUnit(seed ^ 0x3f01ull, x, y) - 0.5;
    const double jy = hashUnit(seed ^ 0x4e12ull, x, y) - 0.5;
    return Point2((static_cast<double>(x) + 0.5 + jx * 0.9) * spacing, (static_cast<double>(y) + 0.5 + jy * 0.9) * spacing);
  };
  // Which plate a point belongs to, as a packed lattice coordinate.
  const auto plateOf = [&](const Point2 &position) {
    const int64_t cx = static_cast<int64_t>(std::floor(position.x / spacing));
    const int64_t cy = static_cast<int64_t>(std::floor(position.y / spacing));
    int64_t bestX = cx;
    int64_t bestY = cy;
    double nearest = kInfinity;
    for (int64_t oy = -1; oy <= 1; ++oy)
    {
      for (int64_t ox = -1; ox <= 1; ++ox)
      {
        const Point2 site = plateSite(cx + ox, cy + oy);
        const double dx = site.x - position.x;
        const double dy = site.y - position.y;
        const double distance = dx * dx + dy * dy;
        if (distance < nearest)
        {
          nearest = distance;
          bestX = cx + ox;
          bestY = cy + oy;
        }
      }
    }
    return std::pair<int64_t, int64_t>(bestX, bestY);
  };
  const auto orogenic = [seed, fraction](const std::pair<int64_t, int64_t> &a, const std::pair<int64_t, int64_t> &b) {
    // Ordered, so the pair hashes the same from either side.
    const std::pair<int64_t, int64_t> low = a < b ? a : b;
    const std::pair<int64_t, int64_t> high = a < b ? b : a;
    const uint64_t mixed = static_cast<uint64_t>(low.first) * 0x9e37ull ^ static_cast<uint64_t>(low.second) * 0x85ebull;
    return hashUnit(seed ^ 0x0209d7ull ^ mixed, high.first, high.second) < fraction;
  };

  uint32_t marked = 0u;
  for (const uint32_t id : component)
  {
    const MapNode &node = graph.node(id);
    const std::pair<int64_t, int64_t> plate = plateOf(node.site);
    for (const uint32_t neighbourId : node.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined() || !neighbour.land)
      {
        continue;
      }
      const std::pair<int64_t, int64_t> other = plateOf(neighbour.site);
      if (other == plate || !orogenic(plate, other))
      {
        continue;
      }
      spine[id] = 1u;
      ++marked;
      break;
    }
  }
  return marked > 0u;
}

void markSpine(
    const MapGraph &graph,
    const std::vector<uint32_t> &component,
    const std::vector<double> &coastDistance,
    const uint32_t maximumRanges,
    const double cellsPerRange,
    const double orogenSpacing,
    const double orogenFraction,
    const uint64_t seed,
    std::vector<uint8_t> &spine)
{
  if (component.empty())
  {
    return;
  }

  // Seams first. A landmass too small to straddle a plate boundary falls
  // through to the long axis, which is the right answer for an island.
  if (markSutures(graph, component, orogenSpacing, orogenFraction, seed, spine))
  {
    return;
  }

  // Finite distances only, for the same reason: one unreachable cell would
  // otherwise make the whole landmass's scale infinite.
  double deepest = 0.0;
  uint32_t core = component.front();
  for (const uint32_t id : component)
  {
    if (std::isfinite(coastDistance[id]) && (coastDistance[id] > deepest || (coastDistance[id] == deepest && id < core)))
    {
      deepest = coastDistance[id];
      core = id;
    }
  }
  spine[core] = 1u;
  if (component.size() < 8u)
  {
    return;
  }
  const double scale = deepest > 0.0 ? deepest : 1.0;

  // Only the interior is eligible, so a range never runs out onto a beach.
  const double interiorCut = deepest * 0.35;
  const auto farthest = [&](const std::vector<double> &distance) {
    uint32_t best = kInvalidNode;
    double bestDistance = -1.0;
    for (const uint32_t id : component)
    {
      if (!std::isfinite(distance[id]) || coastDistance[id] < interiorCut)
      {
        continue;
      }
      if (distance[id] > bestDistance || (distance[id] == bestDistance && id < best))
      {
        bestDistance = distance[id];
        best = id;
      }
    }
    return best;
  };

  std::vector<double> distance;
  std::vector<uint32_t> predecessor;

  // Double sweep for the long axis: farthest from the core, then farthest from
  // that. The path between the two is the main range.
  interiorSweep(graph, {core}, coastDistance, scale, distance, predecessor);
  const uint32_t end = farthest(distance);
  if (end == kInvalidNode)
  {
    return;
  }
  interiorSweep(graph, {end}, coastDistance, scale, distance, predecessor);
  const uint32_t other = farthest(distance);
  if (other == kInvalidNode)
  {
    return;
  }
  for (uint32_t walk = other; walk != kInvalidNode; walk = predecessor[walk])
  {
    spine[walk] = 1u;
    if (walk == end)
    {
      break;
    }
  }

  // Bounded, and deliberately: an unbounded branch count fills a continent
  // with mountains and leaves nowhere for a plain. A branch is a second range,
  // not a twig on the first.
  const size_t branches = std::min<size_t>(maximumRanges == 0u ? 0u : maximumRanges - 1u, static_cast<size_t>(static_cast<double>(component.size()) / std::max(1.0, cellsPerRange)));
  for (size_t branch = 0u; branch < branches; ++branch)
  {
    std::vector<uint32_t> sources;
    for (const uint32_t id : component)
    {
      if (spine[id] != 0u)
      {
        sources.push_back(id);
      }
    }
    if (sources.empty())
    {
      break;
    }
    interiorSweep(graph, sources, coastDistance, scale, distance, predecessor);
    const uint32_t tip = farthest(distance);
    if (tip == kInvalidNode || distance[tip] <= 0.0)
    {
      break;
    }
    for (uint32_t walk = tip; walk != kInvalidNode && spine[walk] == 0u; walk = predecessor[walk])
    {
      spine[walk] = 1u;
    }
  }
}

} // namespace

double cellArea(const MapNode &node)
{
  if (node.area.size() < 3u)
  {
    return 0.0;
  }
  double twiceArea = 0.0;
  for (size_t index = 0u; index < node.area.size(); ++index)
  {
    const Point2 &here = node.area[index];
    const Point2 &next = node.area[(index + 1u) % node.area.size()];
    twiceArea += here.x * next.y - next.x * here.y;
  }
  return std::abs(twiceArea) * 0.5;
}

std::vector<double> coastDistances(const MapGraph &graph)
{
  return graphDistance(
      graph,
      [](const MapNode &node) {
        return !node.land;
      },
      [](const MapNode &node) {
        return node.land;
      });
}

uint32_t classifyWaterBodies(MapGraph &graph, const double extentMetres)
{
  const double margin = extentMetres * 0.02;
  std::vector<uint8_t> ocean(graph.nodeCount(), 0u);
  std::vector<uint32_t> stack;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || node.land)
    {
      continue;
    }
    const bool onBorder = node.site.x < margin || node.site.y < margin || node.site.x > extentMetres - margin || node.site.y > extentMetres - margin;
    if (onBorder && ocean[node.id] == 0u)
    {
      ocean[node.id] = 1u;
      stack.push_back(node.id);
    }
  }
  while (!stack.empty())
  {
    const uint32_t current = stack.back();
    stack.pop_back();
    for (const uint32_t neighbour : graph.node(current).neighbours)
    {
      if (!graph.node(neighbour).refined() && !graph.node(neighbour).land && ocean[neighbour] == 0u)
      {
        ocean[neighbour] = 1u;
        stack.push_back(neighbour);
      }
    }
  }

  uint32_t lakes = 0u;
  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
  {
    MapNode &node = graph.node(id);
    if (node.refined() || node.land)
    {
      continue;
    }
    const CellType wanted = ocean[node.id] != 0u ? CellType::Ocean : CellType::Lake;
    if (node.type != wanted)
    {
      ++lakes;
    }
    node.type = wanted;
    node.content = wanted;
  }
  return lakes;
}

bool assignElevation(const std::vector<Tag> &tags, const ElevationSettings &settings, MapGraph &graph, std::string *error)
{
  const auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }
    return false;
  };

  if (graph.nodeCount() == 0u)
  {
    return fail("cannot assign elevation to an empty graph");
  }
  if (!(settings.extentMetres > 0.0))
  {
    return fail("elevation extent must be positive");
  }
  if (!(settings.reliefMetres > 0.0))
  {
    return fail("relief must be positive");
  }
  if (!std::isfinite(settings.broadReliefFraction) || settings.broadReliefFraction <= 0.0 || settings.broadReliefFraction > 1.0)
  {
    return fail("broad relief fraction must be within (0, 1]");
  }
  if (settings.rangeStrength < 0.0 || settings.rangeStrength > 1.0)
  {
    return fail("range strength must be within [0, 1]");
  }
  if (!(settings.rangeWidthMetres > 0.0))
  {
    return fail("range width must be positive");
  }
  if (!(settings.landformSpacingMetres > 0.0))
  {
    return fail("landform spacing must be positive");
  }
  if (!(settings.landformEdgeMetres > 0.0))
  {
    return fail("landform edge width must be positive");
  }
  if (!std::isfinite(settings.plainProvinceFraction) || !std::isfinite(settings.plateauProvinceFraction) || !std::isfinite(settings.highlandProvinceFraction) || settings.plainProvinceFraction < 0.0 ||
      settings.plateauProvinceFraction < 0.0 || settings.highlandProvinceFraction < 0.0 || settings.plainProvinceFraction + settings.plateauProvinceFraction + settings.highlandProvinceFraction > 1.0)
  {
    return fail("landform province fractions must be non-negative and sum to at most one");
  }
  if (!std::isfinite(settings.landformContrast) || settings.landformContrast < 0.0 || settings.landformContrast > 1.0)
  {
    return fail("landform contrast must be within [0, 1]");
  }
  if (!std::isfinite(settings.cliffMinimumDropFraction) || settings.cliffMinimumDropFraction < 0.0 || settings.cliffMinimumDropFraction > 1.0)
  {
    return fail("cliff minimum drop fraction must be within [0, 1]");
  }
  if (!std::isfinite(settings.cliffMinimumGradient) || settings.cliffMinimumGradient < 0.0)
  {
    return fail("cliff minimum gradient must not be negative");
  }

  // Character tags modulate; they never decide where anything is.
  double reliefScale = 1.0;
  double roughness = std::clamp(settings.roughness, 0.0, 1.0);
  for (const Tag tag : tags)
  {
    if (tag == Tag::Highland)
    {
      reliefScale *= 1.45;
    }
    if (tag == Tag::Lowland)
    {
      reliefScale *= 0.55;
    }
    if (tag == Tag::Barren || tag == Tag::Arid)
    {
      roughness = std::min(1.0, roughness + 0.2);
    }
    if (tag == Tag::Verdant)
    {
      roughness = std::max(0.0, roughness - 0.15);
    }
  }
  const double relief = settings.reliefMetres * settings.broadReliefFraction * reliefScale;

  classifyWaterBodies(graph, settings.extentMetres);

  const std::vector<double> coastDistance = coastDistances(graph);
  const std::vector<double> landDistance = graphDistance(
      graph,
      [](const MapNode &node) {
        return node.land;
      },
      [](const MapNode &node) {
        return !node.land;
      });

  const std::vector<std::vector<uint32_t>> components = landComponents(graph);
  std::vector<uint8_t> spine(graph.nodeCount(), 0u);
  for (const std::vector<uint32_t> &component : components)
  {
    markSpine(graph, component, coastDistance, settings.maximumRangesPerLandmass, settings.cellsPerRange, settings.orogenSpacingMetres, settings.orogenFraction, settings.seed, spine);
  }

  const std::vector<double> spineDistance = graphDistance(
      graph,
      [&spine](const MapNode &node) {
        return spine[node.id] != 0u;
      },
      [](const MapNode &node) {
        return node.land;
      });

  // Tectonic blocks. Sites on a jittered lattice at their own spacing, each
  // block drawing an offset; a cell takes its own block's offset, blended
  // across the boundary over `blockEdgeMetres`.
  //
  // The draw is shaped so most blocks barely move and a few move a long way:
  // a uniform draw gives every block a different height and the map reads as
  // noise, where what is wanted is mostly-level country with the occasional
  // basin or plateau in it.
  const double blockSpacing = std::max(1.0, settings.blockSpacingMetres);
  const double blockRelief = relief * settings.blockReliefFraction;
  const double blockEdge = std::max(1.0, settings.blockEdgeMetres);
  const auto blockOffset = [&](const int64_t x, const int64_t y) {
    const double draw = hashUnit(settings.seed ^ 0x7ec701c5ull, x, y) * 2.0 - 1.0;
    // Cubed: keeps the middle of the range near zero and the tails intact.
    const double shaped = draw * draw * draw;
    const double asymmetry = shaped < 0.0 ? std::max(0.0, settings.blockBasinFraction) : 1.0;
    return (shaped * asymmetry - settings.blockSubsidenceBias) * blockRelief;
  };
  const auto blockSite = [&](const int64_t x, const int64_t y) {
    const double jx = hashUnit(settings.seed ^ 0x11a3ull, x, y) - 0.5;
    const double jy = hashUnit(settings.seed ^ 0x22b7ull, x, y) - 0.5;
    return Point2((static_cast<double>(x) + 0.5 + jx * 0.8) * blockSpacing, (static_cast<double>(y) + 0.5 + jy * 0.8) * blockSpacing);
  };
  const auto blockShift = [&](const Point2 &position) {
    const int64_t cx = static_cast<int64_t>(std::floor(position.x / blockSpacing));
    const int64_t cy = static_cast<int64_t>(std::floor(position.y / blockSpacing));
    double nearest = kInfinity;
    double second = kInfinity;
    double nearestOffset = 0.0;
    double secondOffset = 0.0;
    for (int64_t oy = -1; oy <= 1; ++oy)
    {
      for (int64_t ox = -1; ox <= 1; ++ox)
      {
        const Point2 site = blockSite(cx + ox, cy + oy);
        const double dx = site.x - position.x;
        const double dy = site.y - position.y;
        const double distance = std::sqrt(dx * dx + dy * dy);
        const double offset = blockOffset(cx + ox, cy + oy);
        if (distance < nearest)
        {
          second = nearest;
          secondOffset = nearestOffset;
          nearest = distance;
          nearestOffset = offset;
        }
        else if (distance < second)
        {
          second = distance;
          secondOffset = offset;
        }
      }
    }
    if (!std::isfinite(second))
    {
      return nearestOffset;
    }
    // How far inside its own block the sample is. Zero on the boundary, one
    // well inside it - so the two offsets meet halfway across the edge and the
    // escarpment has a width rather than being a cliff of one cell.
    const double inside = std::clamp((second - nearest) / blockEdge, 0.0, 1.0);
    const double blend = 0.5 + 0.5 * smoothStep(inside);
    return nearestOffset * blend + secondOffset * (1.0 - blend);
  };

  // A second, coarser diagram describes broad landform character. It is not
  // the map's political region diagram and not the tectonic block diagram:
  // sharing any of those would make every visible boundary explain every
  // other boundary, another immediately recognisable procedural pattern.
  const double landformSpacing = std::max(1.0, settings.landformSpacingMetres);
  const double landformEdge = std::max(1.0, settings.landformEdgeMetres);
  const auto landformSite = [&](const int64_t x, const int64_t y) {
    const double jx = hashUnit(settings.seed ^ 0x6c8e9cf5ull, x, y) - 0.5;
    const double jy = hashUnit(settings.seed ^ 0xa24baed4ull, x, y) - 0.5;
    return Point2((static_cast<double>(x) + 0.5 + jx * 0.85) * landformSpacing, (static_cast<double>(y) + 0.5 + jy * 0.85) * landformSpacing);
  };
  const auto landformProfile = [&](const int64_t x, const int64_t y) {
    const double draw = hashUnit(settings.seed ^ 0x9fb21c65ull, x, y);
    const double variation = hashUnit(settings.seed ^ 0xd1b54a32ull, x, y) - 0.5;
    if (draw < settings.plainProvinceFraction)
    {
      // Broad alluvial/continental plains: low, nearly level, and resistant to
      // acquiring a mountain merely because an orogenic seam passes nearby.
      return LandformProfile{0.12 + variation * 0.04, 0.02, 0.12, 0.18, 0.0};
    }
    if (draw < settings.plainProvinceFraction + settings.plateauProvinceFraction)
    {
      // High floor, quiet surface. The province boundary and the independent
      // tectonic blocks supply escarpments; cell noise must not corrugate it.
      return LandformProfile{0.70 + variation * 0.12, 0.025, 0.18, 0.14, 1.0};
    }
    if (draw < settings.plainProvinceFraction + settings.plateauProvinceFraction + settings.highlandProvinceFraction)
    {
      return LandformProfile{0.38 + variation * 0.10, 0.25, 1.0, 1.0, 0.0};
    }
    // The unclaimed share is rolling country rather than another exceptional
    // type. It forms the connective tissue between quiet and dramatic areas.
    return LandformProfile{0.28 + variation * 0.08, 0.15, 0.62, 0.62, 0.0};
  };
  const auto landformAt = [&](const Point2 &position) {
    const int64_t cx = static_cast<int64_t>(std::floor(position.x / landformSpacing));
    const int64_t cy = static_cast<int64_t>(std::floor(position.y / landformSpacing));
    double nearest = kInfinity;
    double second = kInfinity;
    LandformProfile nearestProfile{};
    LandformProfile secondProfile{};
    for (int64_t oy = -1; oy <= 1; ++oy)
    {
      for (int64_t ox = -1; ox <= 1; ++ox)
      {
        const int64_t x = cx + ox;
        const int64_t y = cy + oy;
        const Point2 site = landformSite(x, y);
        const double dx = site.x - position.x;
        const double dy = site.y - position.y;
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance < nearest)
        {
          second = nearest;
          secondProfile = nearestProfile;
          nearest = distance;
          nearestProfile = landformProfile(x, y);
        }
        else if (distance < second)
        {
          second = distance;
          secondProfile = landformProfile(x, y);
        }
      }
    }
    if (!std::isfinite(second))
    {
      return nearestProfile;
    }
    const double inside = std::clamp((second - nearest) / landformEdge, 0.0, 1.0);
    return blendProfile(nearestProfile, secondProfile, 0.5 + 0.5 * smoothStep(inside));
  };

  const double inverseExtent = 1.0 / settings.extentMetres;
  const double coastalPlain = std::max(1.0, settings.coastalPlainMetres);
  const double shelfWidth = std::max(1.0, settings.shelfWidthMetres);

  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
  {
    MapNode &node = graph.node(id);
    if (node.refined())
    {
      continue;
    }
    const double nx = node.site.x * inverseExtent;
    const double ny = node.site.y * inverseExtent;
    // Modulates the flank width of a range that already exists. This is the
    // only place noise touches elevation structure, and it cannot create a
    // range where the spine did not put one - at spineDistance 0 the flank
    // term is 1 regardless of the noise.
    const double flankNoise = 0.7 + 0.6 * valueNoise(settings.seed ^ 0x51ed270bu, nx * 5.0, ny * 5.0);

    if (!node.land)
    {
      const double reach = std::isfinite(landDistance[node.id]) ? landDistance[node.id] : shelfWidth * 4.0;
      const double offshore = smoothStep(reach / shelfWidth);
      const double depth = node.type == CellType::Lake
                               // A lake is a basin in the land, not an arm of the abyss.
                               ? relief * 0.02
                               : relief * (settings.shelfDepthFraction + (settings.abyssalDepthFraction - settings.shelfDepthFraction) * offshore);
      node.intent.elevationMetres = settings.seaLevelMetres - depth;
      node.intent.upliftRate = 0.0;
      node.intent.reliefAmplitude = relief * 0.01;
      node.intent.roughness = roughness * 0.4;
      continue;
    }

    const double inland = std::isfinite(coastDistance[node.id]) ? coastDistance[node.id] : coastalPlain;
    const double coastRamp = smoothStep(inland / coastalPlain);
    const double toSpine = std::isfinite(spineDistance[node.id]) ? spineDistance[node.id] : settings.rangeWidthMetres * 8.0;
    const double flank = std::exp(-toSpine / (settings.rangeWidthMetres * flankNoise));

    const LandformProfile province = landformAt(node.site);
    const double regionalReach = smoothStep(inland / landformSpacing);
    const double provinceBroad = province.broadBase + province.inlandRise * regionalReach;
    const double contrast = settings.landformContrast;
    // At zero contrast this is the former broad inland shelf. Increasing the
    // control replaces it with each province's own floor and very gentle tilt.
    const double broadShape = 1.0 + (provinceBroad - 1.0) * contrast;
    const double rangeActivity = 1.0 + (province.rangeActivity - 1.0) * contrast;
    const double detailActivity = 1.0 + (province.detailActivity - 1.0) * contrast;

    // Both terms meet the sea through the coastal ramp, but only that narrow
    // transition is coast-driven. Beyond it a plain stays a plain and a
    // plateau stays level instead of every landmass climbing toward its core.
    const double plainTerm = (1.0 - settings.rangeStrength) * broadShape * coastRamp;
    const double rangeTerm = settings.rangeStrength * flank * rangeActivity * coastRamp;
    // Sub-range roughness. Applied last, gated by the coastal ramp so the shore
    // stays at the datum, and clamped below so it can never sink a land cell
    // under the sea - the land mask is the raiser's decision, not this one.
    const double detailFrequency = settings.extentMetres / std::max(1.0, settings.detailWavelengthMetres);
    const double detail = fractalNoise(settings.seed ^ 0x2c1b3a97u, nx * detailFrequency, ny * detailFrequency) - 0.5;
    const double roughnessTerm = settings.detailReliefFraction * detail * detailActivity * coastRamp * (0.5 + roughness);
    const double height = std::max(0.01, 0.04 + plainTerm + rangeTerm + roughnessTerm) * relief;

    // The block this cell sits on, faded in from the coast so a block boundary
    // cannot carve a cliff into the shoreline.
    const double tectonic = blockShift(node.site) * coastRamp;
    const double activeRange = settings.rangeStrength * flank * rangeActivity * coastRamp;
    const double baseElevation = settings.seaLevelMetres + height + tectonic;
    const double summitInfluence = smoothStep((activeRange - 0.25) / 0.35);
    const double summitElevation = settings.seaLevelMetres + settings.reliefMetres;
    node.intent.elevationMetres = std::clamp(baseElevation + std::max(0.0, summitElevation - baseElevation) * summitInfluence, settings.seaLevelMetres + relief * 0.004, summitElevation);
    node.intent.upliftRate = settings.upliftRateMetresPerYear * activeRange;
    node.intent.reliefAmplitude = relief * (0.015 + 0.08 * detailActivity + 0.50 * activeRange);
    node.intent.roughness = std::clamp(roughness * (0.25 + 0.75 * detailActivity + 0.4 * activeRange), 0.0, 1.0);

    // Type describes shape, not absolute altitude: a plateau can stand above a
    // nearby mountain foothill while remaining locally flat. Independently
    // uplifted blocks may also become plateaus, so the classification agrees
    // with the elevation field rather than only with the province draw.
    const double tectonicFraction = tectonic / std::max(1.0, relief);
    const double raisedBlock = smoothStep((tectonicFraction - 0.08) / 0.16);
    const double plateauAffinity = std::max(province.plateauAffinity * contrast, raisedBlock * 0.75);
    if (node.coastal)
    {
      node.type = CellType::Coast;
    }
    else if (rangeTerm > 0.25)
    {
      node.type = CellType::Mountain;
    }
    else if (plateauAffinity > 0.52 && coastRamp > 0.55)
    {
      node.type = CellType::Plateau;
    }
    else if (rangeTerm > 0.07 || (detailActivity > 0.58 && broadShape > 0.36))
    {
      node.type = CellType::Hill;
    }
    else
    {
      node.type = CellType::Plain;
    }
    node.content = node.type;
  }

  // Classification has to be a second pass: only now do both sides of every
  // block boundary have their final elevation. Mark the high side, which gives
  // a cliff a stable location and leaves the lower apron available for a plain,
  // hill, or later river valley.
  const double minimumCliffDrop = relief * settings.cliffMinimumDropFraction;
  for (uint32_t id = 0u; id < graph.nodeCount(); ++id)
  {
    MapNode &node = graph.node(id);
    if (node.refined() || !node.land || node.coastal)
    {
      continue;
    }
    double steepestDrop = 0.0;
    double steepestGradient = 0.0;
    for (const uint32_t neighbourId : node.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined() || !neighbour.land)
      {
        continue;
      }
      const double drop = node.intent.elevationMetres - neighbour.intent.elevationMetres;
      if (drop <= 0.0)
      {
        continue;
      }
      const double gradient = drop / std::max(1.0, distanceBetween(node, neighbour));
      if (gradient > steepestGradient)
      {
        steepestDrop = drop;
        steepestGradient = gradient;
      }
    }
    if (!(steepestDrop > 0.0) || steepestDrop < minimumCliffDrop || steepestGradient < settings.cliffMinimumGradient)
    {
      continue;
    }
    node.type = CellType::Cliff;
    node.content = CellType::Cliff;
    node.intent.reliefAmplitude = std::max(node.intent.reliefAmplitude, steepestDrop * 0.5);
    node.intent.roughness = std::max(node.intent.roughness, 0.65);
  }
  return true;
}

} // namespace procedural::world
