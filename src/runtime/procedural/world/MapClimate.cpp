#include "procedural/world/MapClimate.hpp"

#include "procedural/world/MapElevation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace procedural::world {

namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kPi = 3.14159265358979323846;

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

// Zonal mean temperature at sea level. Cosine in latitude, which fits the real
// profile closely enough that nothing here is worth a table.
double sealevelTemperature(const double latitudeDegrees, const ClimateSettings &settings)
{
  const double latitude = std::clamp(std::abs(latitudeDegrees), 0.0, 90.0);
  const double t = std::cos(latitude * kPi / 180.0);
  return settings.poleTemperature + (settings.equatorTemperature - settings.poleTemperature) * t;
}

// Broad classes, for the compatibility gate. Two biomes can share a border when
// their classes are the same or adjacent on the moisture and temperature axes;
// the classes exist so the rule is stated once rather than as a table of pairs.
struct BiomeAxes
{
  // 0 polar, 1 boreal, 2 temperate, 3 subtropical, 4 tropical
  int8_t warmth = 2;
  // 0 hyper-arid, 1 arid, 2 semi-arid, 3 sub-humid, 4 humid, 5 perhumid
  int8_t wetness = 3;
};

// The warmth values are the biome's own climate zone, not a guess: a cold
// desert is temperate (the Gobi), tundra is subpolar rather than polar, and
// savanna is subtropical. Getting these wrong is what breaks a chain, and a
// broken chain is what the gate reports.
BiomeAxes axesOf(const Biome biome)
{
  switch (biome)
  {
  case Biome::Glacier:
    return {0, 2};
  case Biome::Tundra:
    return {1, 3};
  case Biome::ColdDesert:
    return {2, 1};
  case Biome::SaltFlat:
    return {3, 0};
  case Biome::Taiga:
    return {1, 3};
  case Biome::SubtropicalForest:
    return {3, 5};
  case Biome::TemperateForest:
    return {2, 4};
  case Biome::TemperateRainforest:
    return {2, 5};
  case Biome::MontaneForest:
    return {2, 4};
  case Biome::Mediterranean:
    return {3, 2};
  case Biome::Grassland:
    return {2, 2};
  case Biome::Steppe:
    return {2, 1};
  case Biome::Shrubland:
    return {3, 2};
  case Biome::Savanna:
    return {3, 2};
  case Biome::TropicalSeasonalForest:
    // Subtropical rather than tropical, and that is not a quibble: it is the
    // only humid biome between temperate forest and tropical rainforest, so
    // putting it at warmth 4 left a tropical mountainside with no legal
    // transition down to its temperate flank. The gate found this - 182 bad
    // borders on a world moved to the equator - not inspection.
    return {3, 3};
  case Biome::TropicalRainforest:
    return {4, 5};
  case Biome::Desert:
  case Biome::SandDesert:
    return {3, 0};
  case Biome::Wetland:
    return {2, 5};
  case Biome::Swamp:
    return {3, 5};
  case Biome::Mangrove:
    return {4, 5};
  case Biome::AlpineMeadow:
    return {1, 3};
  case Biome::BareRock:
    return {2, 1};
  default:
    return {2, 3};
  }
}

} // namespace

const char *biomeName(const Biome biome)
{
  switch (biome)
  {
  case Biome::Ocean:
    return "ocean";
  case Biome::Lake:
    return "lake";
  case Biome::Shore:
    return "shore";
  case Biome::Glacier:
    return "glacier";
  case Biome::Tundra:
    return "tundra";
  case Biome::ColdDesert:
    return "cold desert";
  case Biome::SaltFlat:
    return "salt flat";
  case Biome::Taiga:
    return "taiga";
  case Biome::TemperateForest:
    return "temperate forest";
  case Biome::TemperateRainforest:
    return "temperate rainforest";
  case Biome::MontaneForest:
    return "montane forest";
  case Biome::Mediterranean:
    return "mediterranean woodland";
  case Biome::Grassland:
    return "grassland";
  case Biome::Steppe:
    return "steppe";
  case Biome::Shrubland:
    return "shrubland";
  case Biome::Savanna:
    return "savanna";
  case Biome::TropicalSeasonalForest:
    return "seasonal forest";
  case Biome::TropicalRainforest:
    return "rainforest";
  case Biome::SubtropicalForest:
    return "subtropical forest";
  case Biome::Desert:
    return "desert";
  case Biome::SandDesert:
    return "sand desert";
  case Biome::Wetland:
    return "wetland";
  case Biome::Swamp:
    return "swamp";
  case Biome::Mangrove:
    return "mangrove";
  case Biome::AlpineMeadow:
    return "alpine meadow";
  case Biome::BareRock:
    return "bare rock";
  case Biome::Count:
    break;
  }
  return "unknown";
}

bool biomeIsWater(const Biome biome)
{
  return biome == Biome::Ocean || biome == Biome::Lake;
}

Biome classifyBiome(const double temperatureCelsius, const double moisture, const double elevationMetres, const double slope)
{
  // Permanent snow and ice needs cold *and* accumulation. A pure temperature
  // threshold gets it wrong at both ends: at -8 C the snowline sat above every
  // mountain this generator makes, so nothing was ever white; at 0 C alone,
  // cold dry lowlands glaciate, and central Siberia is -10 C and is taiga.
  //
  // Below freezing with snow to fall is a glacier; far below freezing is an ice
  // cap whatever falls on it.
  // The accumulation threshold matters as much as the temperature one: a high
  // plateau is cold *and* dry - it sits in the rain shadow of its own rim - and
  // Tibet is cold desert, not an ice cap. At 0.3 the whole of one came out
  // white.
  const bool accumulatedAlpineSnow = elevationMetres > 1500.0 && temperatureCelsius < 3.0 && moisture > 0.38;
  if (temperatureCelsius < -12.0 || (temperatureCelsius < 0.0 && moisture > 0.48) || accumulatedAlpineSnow)
  {
    return Biome::Glacier;
  }
  // Then bare ground: too steep or too broken to hold soil, whatever the
  // climate. This is the one non-climatic override and it is why a wet mountain
  // is rock and not rainforest.
  if (slope > 0.34 && elevationMetres > 1200.0)
  {
    return Biome::BareRock;
  }
  if (temperatureCelsius < 0.0)
  {
    return moisture > 0.14 ? Biome::Tundra : Biome::ColdDesert;
  }
  if (temperatureCelsius < 4.0)
  {
    if (moisture < 0.22)
    {
      return Biome::ColdDesert;
    }
    if (elevationMetres > 1400.0)
    {
      return Biome::AlpineMeadow;
    }
    return moisture > 0.45 ? Biome::Taiga : Biome::Tundra;
  }

  // Whittaker proper: warmth against moisture.
  if (temperatureCelsius < 13.0)
  {
    if (moisture < 0.18)
    {
      return Biome::ColdDesert;
    }
    if (moisture < 0.32)
    {
      return Biome::Steppe;
    }
    if (elevationMetres > 1100.0 && moisture > 0.46)
    {
      return Biome::MontaneForest;
    }
    if (moisture < 0.40)
    {
      return Biome::Grassland;
    }
    if (moisture < 0.72)
    {
      return Biome::TemperateForest;
    }
    return Biome::TemperateRainforest;
  }
  if (temperatureCelsius < 21.0)
  {
    if (moisture < 0.11 && slope < 0.04)
    {
      return Biome::SandDesert;
    }
    if (moisture < 0.18)
    {
      return Biome::Desert;
    }
    if (moisture < 0.42)
    {
      return temperatureCelsius > 15.0 ? Biome::Mediterranean : Biome::Shrubland;
    }
    if (elevationMetres > 1200.0 && moisture > 0.52)
    {
      return Biome::MontaneForest;
    }
    if (moisture < 0.60)
    {
      return Biome::Grassland;
    }
    if (moisture < 0.80)
    {
      return Biome::TemperateForest;
    }
    // The warm end of the temperate band is already subtropical, and a wet
    // subtropical forest is what stands between a temperate one and a tropical
    // one. Without this the classifier steps across two warmth zones in a
    // single cell wherever the 21 C line runs through wet ground.
    return temperatureCelsius > 17.5 ? Biome::SubtropicalForest : Biome::TemperateRainforest;
  }
  if (moisture < 0.11 && slope < 0.04)
  {
    return Biome::SandDesert;
  }
  if (moisture < 0.15)
  {
    return Biome::Desert;
  }
  if (moisture < 0.34)
  {
    return Biome::Savanna;
  }
  if (moisture < 0.62)
  {
    return Biome::TropicalSeasonalForest;
  }
  // Cool tropics, or tropical altitude: still wet, not yet equatorial.
  return (temperatureCelsius < 24.0 || elevationMetres > 1100.0) ? Biome::SubtropicalForest : Biome::TropicalRainforest;
}

bool biomesCompatible(const Biome first, const Biome second)
{
  if (first == second)
  {
    return true;
  }
  // Water borders anything: a coastline is allowed to be abrupt, and is.
  if (biomeIsWater(first) || biomeIsWater(second) || first == Biome::Shore || second == Biome::Shore)
  {
    return true;
  }
  // Bare rock and glacier are landform-driven rather than climate-driven, so
  // they may sit against anything they are high enough to overlook.
  if (first == Biome::BareRock || second == Biome::BareRock || first == Biome::Glacier || second == Biome::Glacier)
  {
    return true;
  }
  // Wetland is a drainage feature and can appear inside any climate that is not
  // a desert.
  const auto drainageBiome = [](const Biome biome) {
    return biome == Biome::Wetland || biome == Biome::Swamp || biome == Biome::Mangrove || biome == Biome::SaltFlat;
  };
  if (drainageBiome(first) || drainageBiome(second))
  {
    return true;
  }

  const BiomeAxes a = axesOf(first);
  const BiomeAxes b = axesOf(second);
  // One step on each axis. Two steps is the jump that has no transition in it -
  // a rainforest against a desert, a tundra against a savanna.
  return std::abs(a.warmth - b.warmth) <= 1 && std::abs(a.wetness - b.wetness) <= 2;
}

uint32_t countIncompatibleBorders(const MapGraph &graph)
{
  uint32_t bad = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined())
    {
      continue;
    }
    for (const uint32_t neighbourId : node.neighbours)
    {
      if (neighbourId <= node.id || graph.node(neighbourId).refined())
      {
        continue;
      }
      if (!biomesCompatible(biomeOf(node), biomeOf(graph.node(neighbourId))))
      {
        ++bad;
      }
    }
  }
  return bad;
}

std::vector<IncompatiblePair> incompatibleBorderPairs(const MapGraph &graph)
{
  std::vector<IncompatiblePair> pairs;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined())
    {
      continue;
    }
    for (const uint32_t neighbourId : node.neighbours)
    {
      if (neighbourId <= node.id || graph.node(neighbourId).refined())
      {
        continue;
      }
      Biome a = biomeOf(node);
      Biome b = biomeOf(graph.node(neighbourId));
      if (biomesCompatible(a, b))
      {
        continue;
      }
      if (static_cast<uint8_t>(b) < static_cast<uint8_t>(a))
      {
        std::swap(a, b);
      }
      auto found = std::find_if(pairs.begin(), pairs.end(), [a, b](const IncompatiblePair &pair) {
        return pair.first == a && pair.second == b;
      });
      if (found == pairs.end())
      {
        pairs.push_back(IncompatiblePair{a, b, 1u});
      }
      else
      {
        ++found->count;
      }
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const IncompatiblePair &lhs, const IncompatiblePair &rhs) {
    return lhs.count > rhs.count;
  });
  return pairs;
}

bool buildClimate(const ClimateSettings &settings, const RiverNetwork &rivers, MapGraph &graph, ClimateMap &climate, std::string *error)
{
  const auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }
    return false;
  };

  climate = ClimateMap{};
  if (graph.nodeCount() == 0u)
  {
    return fail("cannot build a climate for an empty graph");
  }
  if (!(settings.extentMetres > 0.0))
  {
    return fail("climate extent must be positive");
  }
  if (!(settings.coastalMoistureRangeMetres > 0.0))
  {
    return fail("coastal moisture range must be positive");
  }
  if (settings.latitudeSpanDegrees < 0.0)
  {
    return fail("latitude span must not be negative");
  }

  const size_t count = graph.nodeCount();
  climate.cells.assign(count, ClimateCell{});

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
    return fail("no leaves to build a climate for");
  }

  // --- temperature --------------------------------------------------------
  for (const uint32_t leaf : leaves)
  {
    const MapNode &node = graph.node(leaf);
    // World y increases north, so the south edge is the low latitude.
    const double northward = std::clamp(node.site.y / settings.extentMetres, 0.0, 1.0);
    const double latitude = settings.southLatitudeDegrees + northward * settings.latitudeSpanDegrees;
    climate.cells[leaf].latitudeDegrees = latitude;
    const double elevationKilometres = std::max(0.0, node.intent.elevationMetres) * 1e-3;
    climate.cells[leaf].temperatureCelsius = sealevelTemperature(latitude, settings) - settings.lapseRatePerKilometre * elevationKilometres;
  }

  // --- distance to the sea ------------------------------------------------
  //
  // The *ocean*, not any water. Seeding this from every water cell let an
  // inland lake reset the maritime distance to zero, so the dry interior around
  // it came out as wet as the coast - a lake does not make a maritime climate.
  std::vector<double> toSea(count, kInfinity);
  {
    std::priority_queue<QueueEntry> queue;
    for (const uint32_t leaf : leaves)
    {
      if (!graph.node(leaf).land && graph.node(leaf).type != CellType::Lake)
      {
        toSea[leaf] = 0.0;
        queue.push(QueueEntry{0.0, leaf});
      }
    }
    while (!queue.empty())
    {
      const QueueEntry entry = queue.top();
      queue.pop();
      if (entry.cost > toSea[entry.node])
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
        if (step >= toSea[neighbourId])
        {
          continue;
        }
        toSea[neighbourId] = step;
        queue.push(QueueEntry{step, neighbourId});
      }
    }
  }

  // --- rain shadow --------------------------------------------------------
  //
  // March upwind from each land cell and remember the highest ground crossed.
  // Moisture arriving here had to get over that, and what it costs to get over
  // it is what makes one side of a range wet and the other side a desert. This
  // is the single heuristic that most makes a biome map look like a real one.
  const double bearing = settings.prevailingWindBearingDegrees * kPi / 180.0;
  // A bearing is where the wind comes *from*, measured clockwise from north.
  const double upwindX = std::sin(bearing);
  const double upwindY = std::cos(bearing);
  const double halfHeight = std::max(1.0, settings.rainShadowHalfHeightMetres);

  for (const uint32_t leaf : leaves)
  {
    const MapNode &start = graph.node(leaf);
    if (!start.land)
    {
      continue;
    }
    double barrier = 0.0;
    uint32_t current = leaf;
    double travelled = 0.0;
    const double reach = settings.coastalMoistureRangeMetres;
    // Bounded so a long continent cannot make this quadratic.
    for (uint32_t step = 0u; step < 64u && travelled < reach; ++step)
    {
      const MapNode &here = graph.node(current);
      uint32_t next = kInvalidNode;
      double bestAlignment = 0.25;
      for (const uint32_t neighbourId : here.neighbours)
      {
        const MapNode &neighbour = graph.node(neighbourId);
        if (neighbour.refined())
        {
          continue;
        }
        const double dx = neighbour.site.x - here.site.x;
        const double dy = neighbour.site.y - here.site.y;
        const double span = std::sqrt(dx * dx + dy * dy);
        if (span <= 0.0)
        {
          continue;
        }
        const double alignment = (dx * upwindX + dy * upwindY) / span;
        if (alignment > bestAlignment)
        {
          bestAlignment = alignment;
          next = neighbourId;
        }
      }
      if (next == kInvalidNode)
      {
        break;
      }
      travelled += distanceBetween(here, graph.node(next));
      current = next;
      if (!graph.node(next).land)
      {
        break;
      }
      barrier = std::max(barrier, graph.node(next).intent.elevationMetres - start.intent.elevationMetres);
    }
    climate.cells[leaf].rainShadow = 1.0 - std::exp(-std::max(0.0, barrier) / halfHeight * 0.6931);
  }

  // --- moisture -----------------------------------------------------------
  double largestDischarge = 0.0;
  if (rivers.nodes.size() == count)
  {
    largestDischarge = std::max(1e-9, rivers.largestDischargeCumecs);
  }

  // How far the maritime moisture actually has to fall off over.
  //
  // The configured 240 km is a fact about the real atmosphere, and it is the
  // wrong number for any map whose continents are smaller than that: on a world
  // whose deepest interior is 25 km from a shore, exp(-25/240) is 0.90 and the
  // whole landmass is perhumid. Measured maps came out with median moisture
  // 0.88 and p95 pinned at 1.00 - half of every world classified as temperate
  // rainforest, and desert, savanna and steppe unreachable at any latitude.
  //
  // The decay length is therefore taken from the landmass: a fraction of its
  // own deepest point inland, so the interior of a continent is always dry
  // relative to its coast whatever scale the continent is drawn at. A world
  // with continents larger than the configured range keeps the configured
  // range, because the physical number is the smaller one there.
  double deepestInland = 0.0;
  for (const uint32_t leaf : leaves)
  {
    if (graph.node(leaf).land && std::isfinite(toSea[leaf]))
    {
      deepestInland = std::max(deepestInland, toSea[leaf]);
    }
  }

  double moistureRange = settings.coastalMoistureRangeMetres;
  if (deepestInland > 0.0)
  {
    moistureRange = std::min(moistureRange, std::max(1.0, deepestInland * 0.40));
  }

  for (const uint32_t leaf : leaves)
  {
    MapNode &node = graph.node(leaf);
    ClimateCell &cell = climate.cells[leaf];
    if (!node.land)
    {
      cell.moisture = 1.0;
      setBiome(node, node.type == CellType::Lake ? Biome::Lake : Biome::Ocean);
      continue;
    }

    // Maritime moisture, falling off inland.
    const double inland = std::isfinite(toSea[leaf]) ? toSea[leaf] : 0.0;
    double moisture = std::exp(-inland / moistureRange);
    // What the barrier upwind took out of it.
    moisture *= 1.0 - settings.rainShadowStrength * cell.rainShadow;
    // ...and what the subtropical high takes out of it.
    const double fromBelt = (std::abs(cell.latitudeDegrees) - settings.dryBeltCentreDegrees) / std::max(1.0, settings.dryBeltWidthDegrees);
    moisture *= 1.0 - settings.dryBeltStrength * std::exp(-fromBelt * fromBelt);
    // Orographic gain multiplies what arrived rather than adding to it: rising
    // ground wrings out the moisture passing over it, and it cannot wring out
    // moisture that never got there. Added instead, a raised interior plateau
    // came out wetter than the coast.
    moisture *= 1.0 + 0.45 * std::clamp(node.intent.elevationMetres / 2500.0, 0.0, 1.0) * (1.0 - cell.rainShadow);
    // A channel or a lake beside you is water in the ground. Include the first
    // bank cell, not only the channel cell: that is the actual floodplain and
    // the useful settlement corridor.
    if (largestDischarge > 0.0)
    {
      double localDischarge = rivers.nodes[leaf].river ? rivers.nodes[leaf].dischargeCumecs : 0.0;
      bool besideLake = false;
      for (const uint32_t neighbourId : node.neighbours)
      {
        const MapNode &neighbour = graph.node(neighbourId);
        if (neighbour.refined())
        {
          continue;
        }
        if (rivers.nodes[neighbourId].river)
        {
          localDischarge = std::max(localDischarge, rivers.nodes[neighbourId].dischargeCumecs * 0.55);
        }
        besideLake = besideLake || neighbour.type == CellType::Lake;
      }
      moisture += settings.riparianStrength * std::clamp(std::sqrt(localDischarge / largestDischarge), 0.0, 1.0);
      if (besideLake)
      {
        moisture += settings.riparianStrength * 0.45;
      }
    }
    cell.moisture = std::clamp(moisture, 0.0, 1.0);
  }

  // --- classification -----------------------------------------------------
  for (const uint32_t leaf : leaves)
  {
    MapNode &node = graph.node(leaf);
    if (!node.land)
    {
      continue;
    }
    const ClimateCell &cell = climate.cells[leaf];

    // Local slope, from the steepest neighbour. Bare rock and alpine meadow are
    // landform decisions and need it.
    double slope = 0.0;
    for (const uint32_t neighbourId : node.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.refined())
      {
        continue;
      }
      const double span = std::max(1.0, distanceBetween(node, neighbour));
      slope = std::max(slope, std::abs(neighbour.intent.elevationMetres - node.intent.elevationMetres) / span);
    }

    // The landform first, then what grows on it.
    //
    // Temperature and moisture alone decided the biome, and the landform was a
    // separate answer drawn on a separate panel - so a cliff could come out
    // grassland and a valley floor the same shrubland as the ridge above it.
    // Two independent answers about one piece of ground do not combine into
    // anything a reader believes.
    //
    // They are not independent. A cliff, a canyon wall and a rock field hold no
    // soil, whatever the climate; a valley gathers the water that runs off
    // everything around it, and is greener than its own rainfall says. So the
    // landform adjusts the climate the classifier is given, and the biome comes
    // out of one decision instead of being reconciled with another afterwards.
    double localMoisture = cell.moisture;
    double localSlope = slope;

    switch (node.type)
    {
    case CellType::Cliff:
    case CellType::Rocks:
      // Ground too broken to hold soil. Expressed as slope rather than as an
      // override, so the classifier's own bare-rock rule makes the call and a
      // warm wet cliff still carries scrub where a cold dry one does not.
      localSlope = std::max(localSlope, 0.42);
      break;

    case CellType::Canyon:
      localSlope = std::max(localSlope, 0.36);
      // A canyon floor carries the river that cut it.
      localMoisture = std::min(1.0, localMoisture + 0.10);
      break;

    case CellType::Valley:
      // Run-off collects here, so a valley is greener than its rainfall.
      localMoisture = std::min(1.0, localMoisture + 0.14);
      break;

    case CellType::Plateau:
      // Level ground above the plain: it sheds water and has no catchment.
      localMoisture = std::max(0.0, localMoisture - 0.06);
      break;

    default:
      break;
    }

    Biome biome = classifyBiome(cell.temperatureCelsius, localMoisture, node.intent.elevationMetres, localSlope);
    bool riparian = false;
    bool besideLake = false;
    if (rivers.nodes.size() == count)
    {
      riparian = rivers.nodes[leaf].river;
      for (const uint32_t neighbourId : node.neighbours)
      {
        const MapNode &neighbour = graph.node(neighbourId);
        if (neighbour.refined())
        {
          continue;
        }
        riparian = riparian || rivers.nodes[neighbourId].river;
        besideLake = besideLake || neighbour.type == CellType::Lake;
      }
    }
    // A dry closed basin is a salt flat. In wetter climates, conditioned river
    // banks become wetland or warm forested swamp rather than isolated dots on
    // the channel centreline.
    const double fillDepth = rivers.nodes.size() == count ? rivers.nodes[leaf].filledElevationMetres - node.intent.elevationMetres : 0.0;
    if (fillDepth > 12.0 && cell.moisture < 0.20 && slope < 0.012 && biome != Biome::Glacier)
    {
      biome = Biome::SaltFlat;
    }
    else if ((riparian || besideLake) && slope < 0.018 && cell.moisture > 0.52 && !biomeIsWater(biome) && biome != Biome::Glacier)
    {
      biome = cell.temperatureCelsius > 5.0 && cell.moisture > 0.52 ? Biome::Swamp : Biome::Wetland;
    }
    if (node.coastal && biome != Biome::Glacier)
    {
      biome = cell.temperatureCelsius > 20.0 && cell.moisture > 0.72 && slope < 0.018 ? Biome::Mangrove : Biome::Shore;
    }
    setBiome(node, biome);
  }
  return true;
}

} // namespace procedural::world
