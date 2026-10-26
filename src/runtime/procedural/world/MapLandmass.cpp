#include "procedural/world/MapLandmass.hpp"

#include "procedural/world/MapElevation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace procedural::world {

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

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

double fractalNoise(const uint64_t seed, double x, double y, const uint32_t octaves = 5u)
{
  double sum = 0.0;
  double amplitude = 1.0;
  double total = 0.0;
  for (uint32_t octave = 0u; octave < octaves; ++octave)
  {
    sum += valueNoise(seed + octave * 7919u, x, y) * amplitude;
    total += amplitude;
    amplitude *= 0.5;
    x *= 2.0;
    y *= 2.0;
  }
  return sum / total;
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

  bool operator<(const QueueEntry &other) const
  {
    if (cost != other.cost)
    {
      return cost > other.cost;
    }
    return node > other.node;
  }
};

} // namespace

bool growLandmasses(const LandmassSettings &settings, MapGraph &graph, std::string *error)
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
    return fail("cannot grow landmasses on an empty graph");
  }
  if (!(settings.extentMetres > 0.0))
  {
    return fail("landmass extent must be positive");
  }
  if (settings.landFraction < 0.0 || settings.landFraction > 1.0)
  {
    return fail("land fraction must be within [0, 1]");
  }
  if (settings.continents == 0u)
  {
    return fail("a world needs at least one continent");
  }
  if (settings.raggedness < 0.0 || settings.coastDetail < 0.0)
  {
    return fail("raggedness and coast detail must not be negative");
  }

  // ---- stage 1: grow over the coarsest level ------------------------------
  std::vector<uint32_t> coarse;
  for (const MapNode &node : graph.nodes())
  {
    if (node.level == 0u)
    {
      coarse.push_back(node.id);
    }
  }
  if (coarse.empty())
  {
    return fail("no coarse level to grow landmasses on");
  }

  double coarseArea = 0.0;
  for (const uint32_t id : coarse)
  {
    coarseArea += cellArea(graph.node(id));
  }

  // Either a span per continent or a share of the world, and the span wins when
  // it is given. Capped at three quarters of the world so a continent asked for
  // larger than the sea it sits in still leaves a sea.
  double landFraction = settings.landFraction;
  if (settings.continentSpanMetres > 0.0)
  {
    const double perContinent = settings.continentSpanMetres * settings.continentSpanMetres * 0.6;
    landFraction = std::clamp(perContinent * static_cast<double>(settings.continents) / (settings.extentMetres * settings.extentMetres), 0.0, 0.75);
  }
  const double wantedArea = coarseArea * landFraction;

  // Seeds: spread apart and away from the border, so a continent sits in the
  // world rather than running off it. Farthest-point over the coarse graph, as
  // everywhere else in this system - deterministic, and a fact about the
  // partition rather than a draw.
  const double margin = settings.extentMetres * 0.10;
  std::vector<uint32_t> interior;
  for (const uint32_t id : coarse)
  {
    const Point2 &site = graph.node(id).site;
    if (site.x > margin && site.y > margin && site.x < settings.extentMetres - margin && site.y < settings.extentMetres - margin)
    {
      interior.push_back(id);
    }
  }
  if (interior.empty())
  {
    interior = coarse;
  }

  // Continent seeds first, well apart; then each continent's cratons clustered
  // around its own seed.
  //
  // Placing every craton by farthest-point over the whole world instead put
  // them in opposite corners, and each grew its own island until the area
  // budget ran out - an archipelago where a continent was asked for. A
  // continent is several blocks *near each other* that fused, and the clustering
  // radius is what says "near each other".
  std::vector<uint32_t> seeds;
  {
    const Point2 middle(settings.extentMetres * 0.5, settings.extentMetres * 0.5);
    uint32_t best = interior.front();
    double nearest = kInfinity;
    for (const uint32_t id : interior)
    {
      const double dx = graph.node(id).site.x - middle.x;
      const double dy = graph.node(id).site.y - middle.y;
      const double distance = dx * dx + dy * dy;
      if (distance < nearest)
      {
        nearest = distance;
        best = id;
      }
    }
    std::vector<uint32_t> continentSeeds{best};
    while (continentSeeds.size() < settings.continents)
    {
      uint32_t furthest = kInvalidNode;
      double bestSpread = -1.0;
      for (const uint32_t id : interior)
      {
        double spread = kInfinity;
        for (const uint32_t seed : continentSeeds)
        {
          spread = std::min(spread, distanceBetween(graph.node(id), graph.node(seed)));
        }
        if (spread > bestSpread || (spread == bestSpread && id < furthest))
        {
          bestSpread = spread;
          furthest = id;
        }
      }
      if (furthest == kInvalidNode || bestSpread <= 0.0)
      {
        break;
      }
      continentSeeds.push_back(furthest);
    }

    // Radius the cratons of one continent may spread over. Derived from the
    // continent's own size, so a small country's blocks stay close and a large
    // one's spread out.
    const double span = settings.continentSpanMetres > 0.0 ? settings.continentSpanMetres : settings.extentMetres * std::sqrt(std::max(0.02, settings.landFraction / std::max(1u, settings.continents)));
    // Wide enough that the seams between the blocks - and so the ranges on
    // them - reach across the continent rather than sitting in its middle
    // third.
    const double cluster = span * std::max(0.05, settings.cratonSpreadFraction);

    const uint32_t cratons = std::max(1u, settings.cratonsPerContinent);
    for (const uint32_t continentSeed : continentSeeds)
    {
      std::vector<uint32_t> nearby;
      for (const uint32_t id : coarse)
      {
        if (distanceBetween(graph.node(id), graph.node(continentSeed)) <= cluster)
        {
          nearby.push_back(id);
        }
      }
      std::vector<uint32_t> placed{continentSeed};
      while (placed.size() < cratons && placed.size() < nearby.size())
      {
        uint32_t furthest = kInvalidNode;
        double bestSpread = -1.0;
        for (const uint32_t id : nearby)
        {
          double spread = kInfinity;
          for (const uint32_t seed : placed)
          {
            spread = std::min(spread, distanceBetween(graph.node(id), graph.node(seed)));
          }
          if (spread > bestSpread || (spread == bestSpread && id < furthest))
          {
            bestSpread = spread;
            furthest = id;
          }
        }
        if (furthest == kInvalidNode || bestSpread <= 0.0)
        {
          break;
        }
        placed.push_back(furthest);
      }
      seeds.insert(seeds.end(), placed.begin(), placed.end());
    }
  }

  // Grow all fronts from one queue, so continents compete for ground rather
  // than being grown one after another and overlapping.
  const double inverseExtent = 1.0 / settings.extentMetres;
  double coarseSpan = 0.0;
  uint32_t coarseSpans = 0u;
  for (const uint32_t id : coarse)
  {
    if (!graph.node(id).neighbours.empty())
    {
      coarseSpan += distanceBetween(graph.node(id), graph.node(graph.node(id).neighbours.front()));
      ++coarseSpans;
    }
  }
  const double coarseCell = coarseSpans == 0u ? settings.extentMetres * 0.05 : coarseSpan / coarseSpans;
  std::vector<uint8_t> land(graph.nodeCount(), 0u);
  std::vector<double> cost(graph.nodeCount(), kInfinity);
  // Which block claimed each cell. Kept because the seams between blocks are
  // where the mountains belong - a range is a collision, not the middle of a
  // landmass.
  std::vector<uint16_t> claim(graph.nodeCount(), 0xffffu);
  std::priority_queue<QueueEntry> queue;
  for (size_t index = 0u; index < seeds.size(); ++index)
  {
    cost[seeds[index]] = 0.0;
    claim[seeds[index]] = static_cast<uint16_t>(index);
    queue.push(QueueEntry{0.0, seeds[index]});
  }

  double grown = 0.0;
  while (!queue.empty() && grown < wantedArea)
  {
    const QueueEntry entry = queue.top();
    queue.pop();
    if (entry.cost > cost[entry.node] || land[entry.node] != 0u)
    {
      continue;
    }
    land[entry.node] = 1u;
    grown += cellArea(graph.node(entry.node));

    const MapNode &current = graph.node(entry.node);
    for (const uint32_t neighbourId : current.neighbours)
    {
      if (land[neighbourId] != 0u)
      {
        continue;
      }
      const MapNode &neighbour = graph.node(neighbourId);
      // The cost of taking a cell varies over the map, so the front advances
      // fast along some directions and stalls along others. That variation is
      // the whole reason the outline is not a circle.
      const double nx = neighbour.site.x * inverseExtent;
      const double ny = neighbour.site.y * inverseExtent;
      // Wavelength tied to the coarse cell, not to a fixed number of features
      // across the world. At a fixed frequency of 3.5 the field has a 400 km
      // wavelength on a 1,400 km world, so the growth front sees a nearly
      // constant resistance and comes out round however high the raggedness is.
      const double frequency = std::clamp(settings.extentMetres / std::max(1.0, coarseCell * 2.5), 3.0, 40.0);
      // Two octaves, and squared. Five octaves of fBm concentrate near 0.5, so
      // the spread between the cheapest and dearest direction was under two -
      // not enough to make a lobe. Fewer octaves and a power curve give the
      // contrast the front needs to actually run further one way than another.
      const double rough = fractalNoise(settings.seed ^ 0x1a5e91ull, nx * frequency, ny * frequency, 2u);
      const double contrast = rough * rough * (3.0 - 2.0 * rough);
      const double resistance = 1.0 + settings.raggedness * 9.0 * contrast * contrast;
      // Pushing toward the border is expensive, so a continent stops before it
      // runs off the world instead of being clipped by it.
      const double toBorder = std::min(std::min(neighbour.site.x, neighbour.site.y), std::min(settings.extentMetres - neighbour.site.x, settings.extentMetres - neighbour.site.y));
      // Only the last stretch before the edge is discouraged. At 6x over 12%
      // of the world the penalty reached a seventh of the way in from every
      // side, which is a bowl - and a front climbing out of a bowl grows a
      // circle whatever else is done to it.
      const double border = 1.0 + 2.5 * std::exp(-toBorder / (settings.extentMetres * 0.05));
      const double step = distanceBetween(current, neighbour) * resistance * border;
      if (entry.cost + step >= cost[neighbourId])
      {
        continue;
      }
      cost[neighbourId] = entry.cost + step;
      claim[neighbourId] = claim[entry.node];
      queue.push(QueueEntry{cost[neighbourId], neighbourId});
    }
  }

  // ---- stage 2: push down to the leaves and detail the coast --------------
  std::vector<uint32_t> leaves;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined())
    {
      leaves.push_back(node.id);
    }
  }
  if (leaves.empty())
  {
    return fail("no leaves to classify");
  }

  const auto ancestorLand = [&](uint32_t id) {
    while (graph.node(id).parent != kInvalidNode && graph.node(id).level > 0u)
    {
      id = graph.node(id).parent;
    }
    return land[id] != 0u;
  };

  const auto ancestorCraton = [&](uint32_t id) {
    while (graph.node(id).parent != kInvalidNode && graph.node(id).level > 0u)
    {
      id = graph.node(id).parent;
    }
    return claim[id];
  };

  std::vector<uint8_t> leafLand(graph.nodeCount(), 0u);
  for (const uint32_t leaf : leaves)
  {
    leafLand[leaf] = ancestorLand(leaf) ? 1u : 0u;
    graph.node(leaf).craton = ancestorCraton(leaf);
  }

  // Signed distance to the coarse coastline, measured over the leaf graph:
  // positive inland, negative offshore. Perturbing *this* rather than the land
  // flag is what keeps the perturbation local to the coast - noise far inland
  // cannot punch a hole in a continent.
  std::vector<double> distance(graph.nodeCount(), kInfinity);
  std::priority_queue<QueueEntry> front;
  for (const uint32_t leaf : leaves)
  {
    for (const uint32_t neighbour : graph.node(leaf).neighbours)
    {
      if (!graph.node(neighbour).refined() && leafLand[neighbour] != leafLand[leaf])
      {
        distance[leaf] = 0.0;
        front.push(QueueEntry{0.0, leaf});
        break;
      }
    }
  }
  while (!front.empty())
  {
    const QueueEntry entry = front.top();
    front.pop();
    if (entry.cost > distance[entry.node])
    {
      continue;
    }
    for (const uint32_t neighbourId : graph.node(entry.node).neighbours)
    {
      if (graph.node(neighbourId).refined())
      {
        continue;
      }
      const double step = entry.cost + distanceBetween(graph.node(entry.node), graph.node(neighbourId));
      if (step >= distance[neighbourId])
      {
        continue;
      }
      distance[neighbourId] = step;
      front.push(QueueEntry{step, neighbourId});
    }
  }

  // Amplitude scaled to the *leaf* cell, not the coarse one.
  //
  // Scaling it to the coarse cell put roughly 48 km of wobble on a coastline
  // grown at 80 km, which does not detail the shape - it dissolves it. The
  // render came out as an archipelago of mid-sized fragments where a continent
  // had been grown. Detail belongs below the scale of the thing it details:
  // a few leaf cells of movement, so the coast is ragged and the landmass is
  // still the landmass.
  double leafSpan = 0.0;
  uint32_t spans = 0u;
  for (const uint32_t leaf : leaves)
  {
    if (!graph.node(leaf).neighbours.empty())
    {
      leafSpan += distanceBetween(graph.node(leaf), graph.node(graph.node(leaf).neighbours.front()));
      ++spans;
    }
  }
  const double amplitude = (spans == 0u ? settings.extentMetres * 0.01 : leafSpan / spans) * 3.0 * settings.coastDetail;

  std::vector<double> score(graph.nodeCount(), 0.0);
  for (const uint32_t leaf : leaves)
  {
    const MapNode &node = graph.node(leaf);
    const double signed_ = (leafLand[leaf] != 0u ? 1.0 : -1.0) * (std::isfinite(distance[leaf]) ? distance[leaf] : amplitude * 4.0);
    const double nx = node.site.x * inverseExtent;
    const double ny = node.site.y * inverseExtent;
    // Frequency matched to the amplitude: a wobble of a few cells wants a
    // wavelength of a few cells, or the coast gains a long smooth bulge
    // instead of a ragged edge.
    const double detailFrequency = std::clamp(settings.extentMetres / std::max(1.0, amplitude * 3.0), 4.0, 220.0);
    const double detail = fractalNoise(settings.seed ^ 0x77c0a5ull, nx * detailFrequency, ny * detailFrequency) - 0.5;
    score[leaf] = signed_ + detail * 2.0 * amplitude;
  }

  // Cut by ranking, as everywhere else in this system, so the land fraction is
  // exact whatever the noise did.
  std::vector<uint32_t> order = leaves;
  std::sort(order.begin(), order.end(), [&score](const uint32_t lhs, const uint32_t rhs) {
    if (score[lhs] != score[rhs])
    {
      return score[lhs] > score[rhs];
    }
    return lhs < rhs;
  });
  double totalLeafArea = 0.0;
  for (const uint32_t leaf : leaves)
  {
    totalLeafArea += cellArea(graph.node(leaf));
  }
  const double wantedLeafArea = totalLeafArea * landFraction;

  double taken = 0.0;
  for (const uint32_t leaf : order)
  {
    MapNode &node = graph.node(leaf);
    const bool isLand = taken < wantedLeafArea;
    if (isLand)
    {
      taken += cellArea(node);
    }
    node.land = isLand;
    node.type = isLand ? CellType::Plain : CellType::Ocean;
    node.content = node.type;
    node.coastal = false;
    node.shallow = false;
  }

  // ---- stage 3: drop fragments -------------------------------------------
  //
  // A growth front sheds specks. A two-cell rock is an island nothing can reach
  // and nothing can be placed on, and it clutters every later stage - the
  // elevation stage gives it a range of its own, the region stage names it.
  std::vector<uint8_t> seen(graph.nodeCount(), 0u);
  std::vector<uint32_t> stack;
  std::vector<std::vector<uint32_t>> components;
  double landArea = 0.0;
  for (const uint32_t leaf : leaves)
  {
    if (!graph.node(leaf).land)
    {
      continue;
    }
    landArea += cellArea(graph.node(leaf));
    if (seen[leaf] != 0u)
    {
      continue;
    }
    components.emplace_back();
    seen[leaf] = 1u;
    stack.push_back(leaf);
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
  }
  for (const std::vector<uint32_t> &component : components)
  {
    double area = 0.0;
    for (const uint32_t leaf : component)
    {
      area += cellArea(graph.node(leaf));
    }
    if (area >= landArea * settings.minimumIslandFraction)
    {
      continue;
    }
    for (const uint32_t leaf : component)
    {
      MapNode &node = graph.node(leaf);
      node.land = false;
      node.type = CellType::Ocean;
      node.content = CellType::Ocean;
    }
  }

  // Coastal and shallow are derived, never authored.
  for (const uint32_t leaf : leaves)
  {
    MapNode &node = graph.node(leaf);
    for (const uint32_t neighbour : node.neighbours)
    {
      if (graph.node(neighbour).refined())
      {
        continue;
      }
      const bool neighbourLand = graph.node(neighbour).land;
      if (node.land && !neighbourLand)
      {
        node.coastal = true;
      }
      if (!node.land && neighbourLand)
      {
        node.shallow = true;
      }
    }
  }
  return true;
}

} // namespace procedural::world
