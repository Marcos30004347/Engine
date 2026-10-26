#include "procedural/world/Raisers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace procedural::world {

namespace {

// Integer hash. Self-contained and deterministic across targets: it only ever
// does integer arithmetic, so unlike anything built on square roots it produces
// the same world on every machine. That property is a gate for this system.
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
  // 53 bits into [0,1): the full mantissa, so the value is not quantized
  // coarsely enough to band the coastline.
  return static_cast<double>(value >> 11u) / 9007199254740992.0;
}

double smoothStep(const double t)
{
  const double c = std::clamp(t, 0.0, 1.0);
  return c * c * (3.0 - 2.0 * c);
}

// Value noise on a lattice. Deliberately not fBm-with-many-octaves: this only
// has to warp a coastline, and structure comes from the archetype rather than
// from the noise.
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

// Each archetype is a scalar field over normalized coordinates in [0,1]^2.
// Higher means "more likely to be land". The land fraction is applied
// afterwards by ranking, so absolute magnitudes here do not matter - only the
// ordering they induce.
//
// `landFraction` is passed in for the one thing ordering cannot supply: where
// the coastline will end up. An archetype that carves a feature *into* the
// coast has to know where the coast is, and for the monotone coastal fields
// here that is ny = landFraction, since the sites are uniform. Placing such a
// feature at a fixed ny instead puts it wherever the land fraction is not - the
// bay came out as a detached inland sea, which the border-fraction classifier
// cannot see and only the map view revealed.
// A landmass core: a centre, a reach, and a weight. Cores are placed by the
// layout and the land field is their union, so a continent is lobed rather than
// round and the lobes are a decision rather than a wobble.
struct Core
{
  double x = 0.5;
  double y = 0.5;
  double reach = 0.3;
  double weight = 1.0;
};

// Deterministic core placement.
//
// `spread` is how far from the middle a core may sit and `overlap` how much the
// reaches are inflated. Together they say whether the cores fuse into one
// continent with gulfs and peninsulas, or stay apart as an archipelago - the
// same construction serving both, which is why the archipelago no longer needs
// its own interfering sinusoids and no longer reads as a grid.
std::vector<Core> placeCores(const uint64_t seed, const uint32_t count, const double spread, const double overlap)
{
  std::vector<Core> cores;
  cores.reserve(std::max(1u, count));
  for (uint32_t index = 0u; index < std::max(1u, count); ++index)
  {
    const uint64_t base = seed ^ (static_cast<uint64_t>(index) * 0x9e3779b9ull);
    Core core{};
    // Cores after the first walk outward on a rotating bearing rather than
    // being scattered independently: a chain of lobes reads as a continent,
    // and independent draws clump.
    const double angle = (hashUnit(base, 1, 0) + static_cast<double>(index) * 0.618033988749895) * 6.283185307179586;
    const double radius = index == 0u ? hashUnit(base, 2, 0) * spread * 0.35 : spread * (0.45 + 0.55 * hashUnit(base, 3, 0));
    core.x = 0.5 + std::cos(angle) * radius;
    core.y = 0.5 + std::sin(angle) * radius;
    core.reach = overlap * (0.55 + 0.75 * hashUnit(base, 4, 0));
    core.weight = 0.7 + 0.5 * hashUnit(base, 5, 0);
    cores.push_back(core);
  }
  return cores;
}

// Union of the cores, as a smooth maximum. A hard max creates a crease where
// two lobes meet and the coastline kinks visibly along it; a soft one lets them
// fuse into an isthmus.
double coreField(const std::vector<Core> &cores, const double nx, const double ny)
{
  double best = -1.0e9;
  double soft = 0.0;
  for (const Core &core : cores)
  {
    const double dx = nx - core.x;
    const double dy = ny - core.y;
    const double distance = std::sqrt(dx * dx + dy * dy);
    const double value = core.weight * (1.0 - distance / std::max(1e-6, core.reach));
    best = std::max(best, value);
    soft += std::exp(value * 6.0);
  }
  const double smooth = std::log(std::max(1e-30, soft)) / 6.0;
  // Mostly the smooth union, with a little of the hard one so a lobe's tip
  // stays a tip.
  return smooth * 0.8 + best * 0.2;
}

double archetypeField(const Tag shape, const double nx, const double ny, const double landFraction, const std::vector<Core> &cores)
{
  const double cx = nx - 0.5;
  const double cy = ny - 0.5;
  const double radius = std::sqrt(cx * cx + cy * cy);

  switch (shape)
  {
  case Tag::Island:
    // A continent built from several overlapping cores, tapered at the border
    // so it stays surrounded by sea.
    return coreField(cores, nx, ny) - std::max(0.0, radius - 0.40) * 2.6;

  case Tag::Archipelago:
    // The same cores, spread further and inflated less, so they stay separate.
    return coreField(cores, nx, ny) - std::max(0.0, radius - 0.30) * 1.6;

  case Tag::Coast:
    // Land on one side, open water on the other.
    return 1.0 - ny * 1.6;

  case Tag::Bay: {
    // Coast with a rounded bite taken out of it, centred on the coastline so
    // the bay opens into the sea rather than closing into an inland lake.
    const double by = ny - landFraction;
    const double bay = std::sqrt((nx - 0.5) * (nx - 0.5) + by * by);
    return (1.0 - ny * 1.6) - smoothStep(1.0 - bay * 4.0) * 0.9;
  }

  case Tag::Fjord: {
    // Coast cut by deep narrow inlets. The high-frequency term along x is what
    // makes the inlets; it is intentionally sharp rather than smooth.
    const double inlets = std::sin(nx * 38.0);
    return (1.0 - ny * 1.5) - std::pow(std::max(inlets, 0.0), 6.0) * 1.1;
  }

  case Tag::Headland:
    // A promontory reaching into open water.
    return 1.0 - std::abs(nx - 0.5) * 4.0 - ny * 0.8;

  case Tag::Lake:
    // Land everywhere except an interior basin.
    return radius * 2.0 - 0.55;

  case Tag::Landlocked:
    // All land; the ranking still orders cells so landFraction can carve
    // interior water if one is requested.
    return 1.0 - radius * 0.2;

  default:
    return 1.0 - radius * 2.0;
  }
}

} // namespace

bool raiseLand(const Tag shape, const RaiseSettings &settings, MapGraph &graph, std::string *error)
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
    return fail("cannot raise land on an empty partition");
  }
  if (!(settings.extentMetres > 0.0))
  {
    return fail("raise extent must be positive");
  }
  if (settings.landFraction < 0.0 || settings.landFraction > 1.0)
  {
    return fail("land fraction must be within [0, 1]");
  }

  const double complexity = std::clamp(settings.coastComplexity, 0.0, 1.0);
  const double detail = std::clamp(settings.coastDetail, 0.0, 1.0);
  const double inverseExtent = 1.0 / settings.extentMetres;

  const bool separated = shape == Tag::Archipelago;
  const std::vector<Core> cores = placeCores(settings.seed, std::max(1u, settings.landmassCores), separated ? 0.42 : 0.30, separated ? 0.26 : 0.40);

  // Domain warp, three octaves.
  //
  // The coordinate is displaced before the archetype is evaluated, rather than
  // the archetype's value being nudged afterwards. Both honour §5.3 - noise
  // cannot invent land in open sea either way, because the value still comes
  // from the archetype - but warping the domain bends a coastline into
  // peninsulas, inlets and offshore islets, where warping the value only makes
  // the existing outline fuzzy. Every island reading as a blob was that
  // difference.
  //
  // The octaves are weighted by coastDetail: the first is a continental bend
  // whatever the setting, the finer two are what put fjords and headlands on
  // it.
  const std::array<double, 3> frequency{2.4, 6.0, 15.0};
  const std::array<double, 3> weight{1.0, 0.45 + 0.55 * detail, 0.12 + 0.62 * detail};
  const double warpScale = complexity * 1.1;

  // Leaves only. A refined cell is a container: classifying it as well would
  // double-count its area in the land fraction and put a second, disagreeing
  // answer on the same ground.
  std::vector<uint32_t> leaves;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined())
    {
      leaves.push_back(node.id);
    }
  }

  std::vector<double> score(graph.nodeCount(), 0.0);
  for (const uint32_t leaf : leaves)
  {
    const MapNode &node = graph.node(leaf);
    double nx = node.site.x * inverseExtent;
    double ny = node.site.y * inverseExtent;
    double warpX = 0.0;
    double warpY = 0.0;
    double amplitude = 1.0;
    for (size_t octave = 0u; octave < frequency.size(); ++octave)
    {
      const double fx = nx * frequency[octave];
      const double fy = ny * frequency[octave];
      warpX += (fractalNoise(settings.seed + octave * 131u, fx, fy) - 0.5) * amplitude * weight[octave];
      warpY += (fractalNoise(settings.seed + octave * 131u + 977u, fx, fy) - 0.5) * amplitude * weight[octave];
      amplitude *= 0.5;
    }
    nx += warpX * warpScale;
    ny += warpY * warpScale;
    score[leaf] = archetypeField(shape, nx, ny, settings.landFraction, cores);
  }

  // Rank and cut at the requested fraction. This is what makes landFraction
  // exact regardless of archetype, noise or cell count; a fixed threshold on
  // the field would drift with all three.
  std::vector<uint32_t> order = leaves;
  std::sort(order.begin(), order.end(), [&score](const uint32_t lhs, const uint32_t rhs) {
    // Ties break on index so the result is stable rather than dependent on
    // the sort's internal ordering.
    if (score[lhs] != score[rhs])
    {
      return score[lhs] > score[rhs];
    }
    return lhs < rhs;
  });

  const size_t landCount = static_cast<size_t>(std::llround(static_cast<double>(order.size()) * std::clamp(settings.landFraction, 0.0, 1.0)));

  for (size_t rank = 0u; rank < order.size(); ++rank)
  {
    MapNode &node = graph.node(order[rank]);
    const bool land = rank < landCount;
    node.land = land;
    // Land starts as Plain; the elevation stage refines it into hill, mountain
    // and valley. Water is Ocean here and the lake pass reclassifies enclosed
    // bodies later.
    node.type = land ? CellType::Plain : CellType::Ocean;
    // Until the region stage runs, the content is the landform. Keeping them
    // equal rather than leaving content unset is what lets validate() check
    // the two agree about water at every stage in between.
    node.content = node.type;
    node.coastal = false;
    node.shallow = false;
  }

  // Coastal and shallow are derived, never authored: a land cell touching water
  // is coastal, and a water cell touching land is shallow.
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

double borderLandFraction(const MapGraph &graph, const double extentMetres)
{
  if (graph.nodeCount() == 0u || !(extentMetres > 0.0))
  {
    return 0.0;
  }
  // A cell counts as border when its site sits within one nominal cell of the
  // edge. Using the site rather than the polygon avoids depending on border
  // polygons being clipped, which they are not yet.
  const double margin = extentMetres * 0.08;
  uint32_t border = 0u;
  uint32_t borderLand = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined())
    {
      continue;
    }
    const bool onBorder = node.site.x < margin || node.site.y < margin || node.site.x > extentMetres - margin || node.site.y > extentMetres - margin;
    if (!onBorder)
    {
      continue;
    }
    ++border;
    if (node.land)
    {
      ++borderLand;
    }
  }
  return border == 0u ? 0.0 : static_cast<double>(borderLand) / static_cast<double>(border);
}

Tag measuredShape(const MapGraph &graph, const double extentMetres)
{
  const double fraction = borderLandFraction(graph, extentMetres);
  uint32_t water = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined() && !node.land)
    {
      ++water;
    }
  }

  // Mirrors the reference generator's own classifier, which reads the land
  // fraction of border cells. Used here to check that what we generated is what
  // was asked for.
  if (water == 0u)
  {
    return Tag::Landlocked;
  }
  if (fraction < 0.2)
  {
    return Tag::Island;
  }
  if (fraction < 0.9)
  {
    return Tag::Coast;
  }
  return Tag::Lake;
}

} // namespace procedural::world
