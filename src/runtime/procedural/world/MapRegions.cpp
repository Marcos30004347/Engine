#include "procedural/world/MapRegions.hpp"

#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapElevation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>

namespace procedural::world
{

namespace
{

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

uint64_t mix(const uint64_t seed, const uint64_t salt)
{
  return hashCombine(hashCombine(seed ^ 0x9e3779b97f4a7c15ull) ^
      (salt * 0xbf58476d1ce4e5b9ull));
}

double unitFrom(const uint64_t value)
{
  return static_cast<double>(value >> 11u) / 9007199254740992.0;
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
  uint32_t region = kInvalidNode;

  // Cheapest first from a max-heap; ties on node then region so the frontier
  // resolves the same way on every run.
  bool operator<(const QueueEntry &other) const
  {
    if (cost != other.cost)
      return cost > other.cost;
    if (node != other.node)
      return node > other.node;
    return region > other.region;
  }
};

// ---------------------------------------------------------------------------
// Toponymy
// ---------------------------------------------------------------------------

const std::array<const char *, 24> kOnsets{"b", "br", "c", "cr", "d", "dr", "f",
  "g", "gl", "gr", "h", "k", "l", "m", "n", "p", "r", "s", "sk", "st", "t",
  "th", "v", "w"};
const std::array<const char *, 10> kNuclei{
  "a", "e", "i", "o", "u", "ae", "ei", "ia", "ou", "y"};
const std::array<const char *, 12> kCodas{
  "", "", "n", "r", "l", "s", "th", "rn", "ld", "st", "m", "k"};

std::string capitalize(std::string word)
{
  if (!word.empty() && word[0] >= 'a' && word[0] <= 'z')
    word[0] = static_cast<char>(word[0] - 'a' + 'A');
  return word;
}

// The cell type that best names a biome, for the map's own vocabulary.
CellType contentForBiome(const Biome biome)
{
  switch (biome)
  {
  case Biome::Ocean:
    return CellType::Ocean;
  case Biome::Lake:
    return CellType::Lake;
  case Biome::Shore:
    return CellType::Coast;
  case Biome::Glacier:
  case Biome::BareRock:
    return CellType::Rocks;
  case Biome::Tundra:
  case Biome::ColdDesert:
  case Biome::SaltFlat:
  case Biome::Desert:
  case Biome::SandDesert:
    return CellType::Desert;
  case Biome::Taiga:
  case Biome::TemperateForest:
  case Biome::TemperateRainforest:
  case Biome::MontaneForest:
  case Biome::Mediterranean:
  case Biome::SubtropicalForest:
  case Biome::TropicalSeasonalForest:
  case Biome::TropicalRainforest:
    return CellType::Wood;
  case Biome::Wetland:
  case Biome::Swamp:
  case Biome::Mangrove:
    return CellType::Swamp;
  case Biome::AlpineMeadow:
    return CellType::Mountain;
  case Biome::Grassland:
  case Biome::Steppe:
  case Biome::Shrubland:
  case Biome::Savanna:
  default:
    return CellType::Plain;
  }
}

} // namespace

std::string placeName(const uint64_t seed, const uint64_t salt)
{
  uint64_t state = mix(seed, salt);
  const auto next = [&state]()
  {
    state = hashCombine(state + 0x9e3779b97f4a7c15ull);
    return state;
  };
  // Two syllables most of the time, three sometimes. One is too curt to read as
  // a place and four starts sounding generated.
  const uint32_t syllables = (next() % 4u) == 0u ? 3u : 2u;
  std::string name;
  for (uint32_t syllable = 0u; syllable < syllables; ++syllable)
  {
    name += kOnsets[next() % kOnsets.size()];
    name += kNuclei[next() % kNuclei.size()];
    // A coda inside a word makes a consonant cluster at the join, which is
    // where names start sounding unpronounceable. Only the last syllable takes
    // one freely.
    if (syllable + 1u == syllables || (next() % 5u) == 0u)
      name += kCodas[next() % kCodas.size()];
  }
  return capitalize(name);
}

std::string namedPlace(
    const CellType content, const uint64_t seed, const uint64_t salt)
{
  const std::string name = placeName(seed, salt);
  const uint64_t choice = mix(seed, salt ^ 0x5bf03635ull);
  const auto pick = [&choice](const std::initializer_list<const char *> forms)
  {
    return *(forms.begin() + (choice % forms.size()));
  };

  const char *form = "the %s Country";
  switch (content)
  {
  case CellType::Mountain:
    form = pick({"the %s Mountains", "the %s Range", "the Peaks of %s",
        "the %s Massif"});
    break;
  case CellType::Plateau:
    form = pick({"the %s Plateau", "the Tablelands of %s", "the %s Mesa",
        "the %s Heights"});
    break;
  case CellType::Hill:
    form = pick({"the %s Hills", "the %s Downs", "the %s Uplands",
        "the Highlands of %s"});
    break;
  case CellType::Wood:
    form = pick({"%s Wood", "the Forest of %s", "the %s Weald", "%s Greenwood"});
    break;
  case CellType::Swamp:
    form = pick({"the %s Marshes", "the %s Fens", "the Mire of %s",
        "the %s Moss"});
    break;
  case CellType::Desert:
    form = pick({"the %s Waste", "the %s Sands", "the %s Desert",
        "the Barrens of %s"});
    break;
  case CellType::Rocks:
    form =
        pick({"the %s Crags", "the %s Scree", "the Tors of %s", "the %s Reach"});
    break;
  case CellType::Cliff:
    form = pick({"the %s Cliffs", "the Scarps of %s", "the %s Escarpment",
        "the %s Bluffs"});
    break;
  case CellType::Canyon:
    form = pick({"%s Canyon", "the Gorge of %s", "the %s Chasm",
        "%s Ravine"});
    break;
  case CellType::Valley:
    form = pick({"the Vale of %s", "%s Dale", "the %s Glen", "%s Hollow"});
    break;
  case CellType::Coast:
    form = pick({"the %s Shore", "the %s Strand", "the Cliffs of %s",
        "the %s Coast"});
    break;
  case CellType::Plain:
    form = pick({"the %s Plain", "the %s Fields", "the Vale of %s",
        "the %s Flats"});
    break;
  case CellType::Lake:
    form = pick({"Lake %s", "the %s Mere", "Loch %s", "the Water of %s"});
    break;
  case CellType::Ocean:
    form = pick({"the %s Sea", "the %s Ocean", "the Gulf of %s",
        "the %s Deeps"});
    break;
  case CellType::Count:
    break;
  }

  std::string result(form);
  const size_t marker = result.find("%s");
  if (marker != std::string::npos)
    result = result.substr(0u, marker) + name + result.substr(marker + 2u);
  return result;
}

std::string namedRiver(const uint64_t seed, const uint64_t salt)
{
  const std::string name = placeName(seed, salt);
  switch (mix(seed, salt ^ 0x71f3c9edull) % 4u)
  {
  case 0u:
    return "the " + name;
  case 1u:
    return "the " + name + " Water";
  case 2u:
    return name + " River";
  default:
    return "the " + name + "flow";
  }
}

bool buildRegions(
    const RegionSettings &settings,
    const RiverNetwork &rivers,
    MapGraph &graph,
    RegionMap &regions,
    std::string *error)
{
  const auto fail = [&](const std::string &message)
  {
    if (error != nullptr)
      *error = message;
    return false;
  };

  regions = RegionMap{};
  if (graph.nodeCount() == 0u)
    return fail("cannot build regions on an empty graph");
  if (!(settings.targetAreaSquareMetres > 0.0))
    return fail("target region area must be positive");
  if (settings.ridgeResistance < 0.0 || settings.riverResistance < 0.0)
    return fail("region resistances must not be negative");
  if (!std::isfinite(settings.hierarchyCohesion) ||
      settings.hierarchyCohesion < 0.0 || settings.hierarchyCohesion > 1.0)
    return fail("region hierarchy cohesion must be between zero and one");

  const size_t count = graph.nodeCount();
  regions.cellRegion.assign(count, kInvalidNode);

  std::vector<uint32_t> land;
  double landArea = 0.0;
  for (const MapNode &node : graph.nodes())
    if (!node.refined() && node.land)
    {
      land.push_back(node.id);
      landArea += cellArea(node);
    }
  if (land.empty())
    return true;

  const size_t wanted = std::max<size_t>(
      1u, static_cast<size_t>(std::llround(landArea /
              settings.targetAreaSquareMetres)));

  // --- seeding ------------------------------------------------------------
  //
  // Farthest-point seeding: each new seed is the land cell furthest from every
  // seed already placed. Spread without randomness, so two worlds that differ
  // only in seed do not differ in how evenly their regions are distributed -
  // and so a region's position is a fact about the land, not a draw.
  std::vector<double> spread(count, kInfinity);
  std::vector<uint32_t> seeds;
  seeds.push_back(land.front());
  for (size_t placed = 0u; placed < wanted; ++placed)
  {
    const uint32_t source = seeds.back();
    // Plain hop distance here: seeding wants geometric spread, and the ridge
    // and river costs below are about where a boundary falls, not where a
    // centre sits.
    std::priority_queue<QueueEntry> queue;
    std::vector<double> distance(count, kInfinity);
    distance[source] = 0.0;
    queue.push(QueueEntry{0.0, source, 0u});
    while (!queue.empty())
    {
      const QueueEntry entry = queue.top();
      queue.pop();
      if (entry.cost > distance[entry.node])
        continue;
      for (const uint32_t neighbourId : graph.node(entry.node).neighbours)
      {
        if (graph.node(neighbourId).refined() || !graph.node(neighbourId).land)
          continue;
        const double cost = entry.cost +
            distanceBetween(graph.node(entry.node), graph.node(neighbourId));
        if (cost >= distance[neighbourId])
          continue;
        distance[neighbourId] = cost;
        queue.push(QueueEntry{cost, neighbourId, 0u});
      }
    }
    for (const uint32_t id : land)
      spread[id] = std::min(spread[id], distance[id]);
    if (placed + 1u >= wanted)
      break;
    uint32_t next = kInvalidNode;
    double furthest = -1.0;
    for (const uint32_t id : land)
      if (std::isfinite(spread[id]) &&
          (spread[id] > furthest || (spread[id] == furthest && id < next)))
      {
        furthest = spread[id];
        next = id;
      }
    if (next == kInvalidNode || furthest <= 0.0)
      break;
    seeds.push_back(next);
  }

  // Farthest-point seeding spreads seeds over the land it can *reach*, and on
  // an archipelago it cannot reach everything: when the seed budget runs out
  // before a small island is picked, that island gets no seed, no region and no
  // name. Give every connected landmass at least one.
  {
    std::vector<uint8_t> seeded(count, 0u);
    std::vector<uint32_t> stack;
    const auto floodFrom = [&](const uint32_t start)
    {
      stack.assign(1u, start);
      seeded[start] = 1u;
      while (!stack.empty())
      {
        const uint32_t current = stack.back();
        stack.pop_back();
        for (const uint32_t neighbour : graph.node(current).neighbours)
          if (!graph.node(neighbour).refined() && graph.node(neighbour).land &&
              seeded[neighbour] == 0u)
          {
            seeded[neighbour] = 1u;
            stack.push_back(neighbour);
          }
      }
    };
    for (const uint32_t seed : seeds)
      if (seeded[seed] == 0u)
        floodFrom(seed);
    for (const uint32_t id : land)
      if (seeded[id] == 0u)
      {
        seeds.push_back(id);
        floodFrom(id);
      }
  }

  // --- growing ------------------------------------------------------------
  //
  // One priority queue over all seeds at once, so the regions meet where the
  // accumulated cost balances rather than wherever a sequential flood ran out.
  // Crossing a ridge or a river costs extra, which is what puts the boundaries
  // on watersheds and ranges the way a map's boundaries fall.
  regions.regions.resize(seeds.size());
  std::vector<double> cost(count, kInfinity);
  std::priority_queue<QueueEntry> queue;
  for (size_t index = 0u; index < seeds.size(); ++index)
  {
    cost[seeds[index]] = 0.0;
    queue.push(QueueEntry{0.0, seeds[index], static_cast<uint32_t>(index)});
    regions.regions[index].id = static_cast<uint32_t>(index);
    regions.regions[index].seedCell = seeds[index];
  }
  while (!queue.empty())
  {
    const QueueEntry entry = queue.top();
    queue.pop();
    if (entry.cost > cost[entry.node])
      continue;
    if (regions.cellRegion[entry.node] != kInvalidNode &&
        regions.cellRegion[entry.node] != entry.region)
      continue;
    regions.cellRegion[entry.node] = entry.region;
    const MapNode &current = graph.node(entry.node);
    for (const uint32_t neighbourId : current.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined() || !neighbour.land)
        continue;
      double step = distanceBetween(current, neighbour);
      // A hierarchy parent is a larger, calmer unit than the leaf cells used
      // for terrain. Make travel within one parent cheaper and crossing its
      // seam dearer, so the multi-source growth tends to claim parent groups
      // whole instead of drawing a sawtooth through every child.
      if (current.parent != kInvalidNode && neighbour.parent != kInvalidNode)
        step *= current.parent == neighbour.parent
            ? 1.0 - settings.hierarchyCohesion * 0.35
            : 1.0 + settings.hierarchyCohesion * 0.35;
      // Climbing is expensive, descending is not. This is what makes the
      // boundaries fall on watersheds: a region seeded in a basin spreads over
      // it cheaply and stalls against the surrounding crest, where the region
      // climbing the far side meets it. A low saddle costs less than a high
      // crest, so the boundary picks the pass, which is where real boundaries
      // go.
      //
      // Charging for *occupying* high ground instead - the obvious first
      // reading of "ranges resist" - does nothing. It only delays when the
      // front reaches a range; it never stops a region spanning one, and it
      // measured slightly worse than no resistance at all.
      const double climb = std::max(0.0,
          neighbour.intent.elevationMetres - current.intent.elevationMetres);
      step *= 1.0 +
          settings.ridgeResistance *
              std::clamp(climb / std::max(1.0, graph.brief().reliefMetres) * 8.0,
                  0.0, 1.0);
      if (rivers.nodes.size() == count &&
          (rivers.nodes[neighbourId].river || rivers.nodes[entry.node].river))
        step *= 1.0 + settings.riverResistance;
      const double total = entry.cost + step;
      if (total >= cost[neighbourId])
        continue;
      cost[neighbourId] = total;
      queue.push(QueueEntry{total, neighbourId, entry.region});
    }
  }

  // --- measurement --------------------------------------------------------
  for (const uint32_t id : land)
  {
    const uint32_t region = regions.cellRegion[id];
    if (region == kInvalidNode)
      continue;
    MapRegion &target = regions.regions[region];
    const MapNode &node = graph.node(id);
    const double area = cellArea(node);
    target.cells.push_back(id);
    target.areaSquareMetres += area;
    target.centroid.x += node.site.x * area;
    target.centroid.y += node.site.y * area;
    target.meanElevationMetres += node.intent.elevationMetres * area;
    target.meanUpliftRate += node.intent.upliftRate * area;
    if (node.coastal)
      target.coastalFraction += 1.0;
    if (rivers.nodes.size() == count && rivers.nodes[id].river)
      target.riverFraction += 1.0;
  }
  for (MapRegion &region : regions.regions)
  {
    if (region.cells.empty() || region.areaSquareMetres <= 0.0)
      continue;
    region.centroid.x /= region.areaSquareMetres;
    region.centroid.y /= region.areaSquareMetres;
    region.meanElevationMetres /= region.areaSquareMetres;
    region.meanUpliftRate /= region.areaSquareMetres;
    region.coastalFraction /= static_cast<double>(region.cells.size());
    region.riverFraction /= static_cast<double>(region.cells.size());
    double lowest = kInfinity;
    double highest = -kInfinity;
    for (const uint32_t id : region.cells)
    {
      lowest = std::min(lowest, graph.node(id).intent.elevationMetres);
      highest = std::max(highest, graph.node(id).intent.elevationMetres);
    }
    region.reliefMetres = highest - lowest;
  }
  // Drop regions that ended up with nothing - possible when two seeds land on
  // the same one-cell island - and renumber, so a region id is always a valid
  // index and the map never carries an empty label.
  {
    std::vector<uint32_t> remap(regions.regions.size(), kInvalidNode);
    std::vector<MapRegion> kept;
    for (MapRegion &region : regions.regions)
      if (!region.cells.empty())
      {
        remap[region.id] = static_cast<uint32_t>(kept.size());
        region.id = static_cast<uint32_t>(kept.size());
        kept.push_back(std::move(region));
      }
    regions.regions = std::move(kept);
    for (uint32_t &region : regions.cellRegion)
      if (region != kInvalidNode)
        region = remap[region];
  }

  // --- content ------------------------------------------------------------
  //
  // The dominant biome of the region's own cells, by area. See the header for
  // why this is a lookup and not a second scoring pass.
  for (MapRegion &region : regions.regions)
  {
    std::vector<double> byBiome(
        static_cast<size_t>(Biome::Count), 0.0);
    for (const uint32_t id : region.cells)
      byBiome[static_cast<size_t>(biomeOf(graph.node(id)))] +=
          cellArea(graph.node(id));
    size_t dominant = static_cast<size_t>(Biome::Grassland);
    double best = -1.0;
    for (size_t index = 0u; index < byBiome.size(); ++index)
      if (byBiome[index] > best)
      {
        best = byBiome[index];
        dominant = index;
      }
    region.biome = static_cast<Biome>(dominant);
    region.content = contentForBiome(region.biome);
    region.name = namedPlace(region.content, settings.seed, region.id);
  }

  for (uint32_t id = 0u; id < count; ++id)
  {
    MapNode &node = graph.node(id);
    if (node.refined())
      continue;
    node.region = regions.cellRegion[id];
    if (!node.land)
    {
      node.content = node.type;
      continue;
    }
    node.content = node.region == kInvalidNode
        ? node.type
        : regions.regions[node.region].content;
    // Structural landforms keep their identity inside a forest or desert
    // region: unlike a biome, each was measured directly from the terrain.
    if (node.type == CellType::Valley || node.type == CellType::Cliff ||
        node.type == CellType::Canyon)
      node.content = node.type;
  }

  // --- rivers -------------------------------------------------------------
  //
  // Named from the mouth up the largest tributary at each confluence, which is
  // how a river keeps one name from sea to source. Only channels worth a label
  // get one; a map with every headwater named is unreadable.
  if (rivers.nodes.size() == count)
  {
    std::vector<uint8_t> named(count, 0u);
    std::vector<uint32_t> mouths;
    for (uint32_t id = 0u; id < count; ++id)
      if (!graph.node(id).refined() && rivers.nodes[id].river &&
          rivers.nodes[id].downstream != kInvalidNode &&
          !graph.node(rivers.nodes[id].downstream).land)
        mouths.push_back(id);
    std::sort(mouths.begin(), mouths.end(),
        [&rivers](const uint32_t lhs, const uint32_t rhs)
        {
          if (rivers.nodes[lhs].dischargeCumecs !=
              rivers.nodes[rhs].dischargeCumecs)
            return rivers.nodes[lhs].dischargeCumecs >
                rivers.nodes[rhs].dischargeCumecs;
          return lhs < rhs;
        });

    for (const uint32_t mouth : mouths)
    {
      if (named[mouth] != 0u)
        continue;
      NamedRiver river{};
      river.dischargeCumecs = rivers.nodes[mouth].dischargeCumecs;
      river.strahlerOrder = rivers.nodes[mouth].strahlerOrder;
      uint32_t walk = mouth;
      while (walk != kInvalidNode && named[walk] == 0u)
      {
        named[walk] = 1u;
        river.stem.push_back(walk);
        uint32_t best = kInvalidNode;
        double largest = 0.0;
        for (const uint32_t neighbour : graph.node(walk).neighbours)
        {
          if (!rivers.nodes[neighbour].river ||
              rivers.nodes[neighbour].downstream != walk)
            continue;
          const double flow = rivers.nodes[neighbour].dischargeCumecs;
          if (flow > largest || (flow == largest && neighbour < best))
          {
            largest = flow;
            best = neighbour;
          }
        }
        walk = best;
      }
      // A two-cell trickle is not a river anyone names, and neither is a
      // headwater beside a trunk twenty times its size.
      if (river.stem.size() < 3u || river.strahlerOrder < 2u ||
          river.dischargeCumecs <
              rivers.largestDischargeCumecs * settings.riverNamingFraction)
        continue;
      river.name = namedRiver(
          settings.seed, 0x9c17ull + static_cast<uint64_t>(mouth));
      regions.rivers.push_back(std::move(river));
    }
  }
  return true;
}

} // namespace procedural::world
