#include "procedural/world/land/Blend.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world {

namespace {

// A sample exactly on a site, in fractions of the support radius. Below this
// the inverse-distance weight overflows to infinity and normalization produces
// NaN, so the sample is answered as a pure hit on that cell instead.
constexpr double kSiteEpsilon = 1.0e-9;

constexpr double kMinimumSharpness = 0.5;
constexpr double kMaximumSharpness = 16.0;

double squaredDistance(const Point2 &a, const Point2 &b)
{
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  return dx * dx + dy * dy;
}

// Modified Shepard, with `radius` the distance to the first site outside the
// contributing set. The (radius - distance) factor is what makes a contributor
// fade to nothing as it leaves that set; plain 1/d^p would step.
double shepardWeight(const double distance, const double radius, const double exponent)
{
  if (distance >= radius)
  {
    return 0.0;
  }
  return std::pow((radius - distance) / (radius * distance), exponent);
}

double smoothStep(const double low, const double high, const double value)
{
  if (!(high > low))
  {
    return value >= high ? 1.0 : 0.0;
  }
  const double t = std::clamp((value - low) / (high - low), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

uint64_t hashCell(uint64_t value)
{
  value ^= value >> 30u;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27u;
  value *= 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

double cellWeightUnit(const MapGraph &graph, const uint32_t node)
{
  const uint64_t value = hashCell(graph.brief().seed ^ 0x726567696f6e7765ull ^ (static_cast<uint64_t>(node) + 1u) * 0x9e3779b97f4a7c15ull);
  return static_cast<double>(value >> 11u) * (1.0 / 9007199254740992.0);
}

} // namespace

double transitionSharpness(CellType first, CellType second)
{
  if (first == second)
  {
    return 2.0;
  }

  // A coastline is the one edge in this system that is genuinely hard: land
  // stops. Blurring it across half a cell is the single clearest way to make a
  // map look procedural.
  if (cellTypeIsWater(first) != cellTypeIsWater(second))
  {
    return 8.0;
  }

  // Sorted so the table below is symmetric without having to list both
  // orderings, which is what keeps the effective exponent independent of which
  // cell happens to be nearest.
  if (static_cast<uint8_t>(second) < static_cast<uint8_t>(first))
  {
    std::swap(first, second);
  }

  const auto pair = [first, second](const CellType a, const CellType b) {
    return first == a && second == b;
  };

  // These types encode a measured discontinuity, so semantic blending must
  // retain the wall or gorge boundary that caused the classification.
  if (first == CellType::Cliff || second == CellType::Cliff || first == CellType::Canyon || second == CellType::Canyon)
  {
    return 6.0;
  }

  // Exposed bedrock ends where it ends; there is no gradual version of it.
  if (first == CellType::Rocks || second == CellType::Rocks)
  {
    return 5.0;
  }

  // The escarpment case from the spec: a range meeting a plain is a wall.
  if (pair(CellType::Plain, CellType::Mountain))
  {
    return 6.0;
  }
  if (pair(CellType::Plain, CellType::Plateau))
  {
    return 4.5;
  }
  if (pair(CellType::Hill, CellType::Plateau))
  {
    return 3.0;
  }
  if (pair(CellType::Mountain, CellType::Plateau))
  {
    return 3.5;
  }
  if (pair(CellType::Mountain, CellType::Valley))
  {
    return 4.0;
  }
  if (pair(CellType::Hill, CellType::Mountain))
  {
    return 2.5;
  }
  // ...and the diffuse case it is contrasted with.
  if (pair(CellType::Plain, CellType::Hill))
  {
    return 1.6;
  }
  if (pair(CellType::Plain, CellType::Valley))
  {
    return 1.8;
  }
  if (pair(CellType::Plain, CellType::Swamp))
  {
    return 1.4;
  }
  if (pair(CellType::Plain, CellType::Wood))
  {
    return 1.5;
  }
  if (pair(CellType::Hill, CellType::Wood))
  {
    return 1.5;
  }
  if (pair(CellType::Swamp, CellType::Wood))
  {
    return 1.4;
  }
  // Deserts have edges - the vegetation line is visible from orbit - but not
  // escarpment edges.
  if (first == CellType::Desert || second == CellType::Desert)
  {
    return 3.0;
  }
  // Beaches grade into whatever is behind them.
  if (first == CellType::Coast || second == CellType::Coast)
  {
    return 1.5;
  }

  return 2.0;
}

bool SiteIndex::build(const MapGraph &graph, const uint8_t level, const double extentMetres, std::string *error)
{
  sites_.clear();
  bucketOffsets_.clear();
  bucketEntries_.clear();
  gridSide_ = 0u;
  cellSize_ = 0.0;
  extentMetres_ = extentMetres;

  if (!(extentMetres > 0.0))
  {
    if (error != nullptr)
    {
      *error = "SiteIndex requires a positive extent";
    }
    return false;
  }

  // Leaves at or below the requested level. A refined cell is a container, and
  // blending its colour in as well would smear a coarse average over the detail
  // that replaced it.
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined() && node.level >= level)
    {
      sites_.push_back(Site{node.site, node.id});
    }
  }
  if (sites_.empty())
  {
    if (error != nullptr)
    {
      *error = "SiteIndex found no nodes at level " + std::to_string(level);
    }
    return false;
  }

  // One site per grid cell on average. Capped because a refined node can carry
  // a very large site count and the grid is allocated eagerly.
  const double side = std::sqrt(static_cast<double>(sites_.size()));
  gridSide_ = static_cast<uint32_t>(std::clamp(std::floor(side), 1.0, 1024.0));
  cellSize_ = extentMetres_ / static_cast<double>(gridSide_);

  const size_t bucketCount = static_cast<size_t>(gridSide_) * gridSide_;
  std::vector<uint32_t> counts(bucketCount, 0u);
  std::vector<uint32_t> bucketOf(sites_.size(), 0u);
  for (size_t i = 0u; i < sites_.size(); ++i)
  {
    const uint32_t bucket = bucketIndex(sites_[i].position);
    bucketOf[i] = bucket;
    ++counts[bucket];
  }

  bucketOffsets_.assign(bucketCount + 1u, 0u);
  for (size_t bucket = 0u; bucket < bucketCount; ++bucket)
  {
    bucketOffsets_[bucket + 1u] = bucketOffsets_[bucket] + counts[bucket];
  }
  bucketEntries_.assign(sites_.size(), 0u);
  std::vector<uint32_t> cursor(bucketOffsets_.begin(), bucketOffsets_.end() - 1);
  for (size_t i = 0u; i < sites_.size(); ++i)
  {
    bucketEntries_[cursor[bucketOf[i]]++] = static_cast<uint32_t>(i);
  }
  return true;
}

uint32_t SiteIndex::bucketIndex(const Point2 &position) const
{
  const int32_t last = static_cast<int32_t>(gridSide_) - 1;
  const int32_t x = std::clamp(static_cast<int32_t>(std::floor(position.x / cellSize_)), 0, last);
  const int32_t y = std::clamp(static_cast<int32_t>(std::floor(position.y / cellSize_)), 0, last);
  return static_cast<uint32_t>(y) * gridSide_ + static_cast<uint32_t>(x);
}

void SiteIndex::nearest(const Point2 position, const uint32_t count, std::vector<SiteHit> &out) const
{
  out.clear();
  if (sites_.empty() || count == 0u)
  {
    return;
  }

  const int32_t last = static_cast<int32_t>(gridSide_) - 1;
  const int32_t centreX = std::clamp(static_cast<int32_t>(std::floor(position.x / cellSize_)), 0, last);
  const int32_t centreY = std::clamp(static_cast<int32_t>(std::floor(position.y / cellSize_)), 0, last);

  const auto consider = [&](const uint32_t siteIndex) {
    const Site &site = sites_[siteIndex];
    const SiteHit hit{site.node, std::sqrt(squaredDistance(position, site.position))};
    // Ties break on node id, so the result does not depend on the order the
    // grid happened to be walked - which changes with the cell size.
    const auto before = std::lower_bound(out.begin(), out.end(), hit, [](const SiteHit &a, const SiteHit &b) {
      if (a.distanceMetres != b.distanceMetres)
      {
        return a.distanceMetres < b.distanceMetres;
      }
      return a.node < b.node;
    });
    if (before == out.end() && out.size() >= count)
    {
      return;
    }
    out.insert(before, hit);
    if (out.size() > count)
    {
      out.pop_back();
    }
  };

  const auto visitCell = [&](const int32_t x, const int32_t y) {
    if (x < 0 || y < 0 || x > last || y > last)
    {
      return;
    }
    const uint32_t bucket = static_cast<uint32_t>(y) * gridSide_ + static_cast<uint32_t>(x);
    for (uint32_t i = bucketOffsets_[bucket]; i < bucketOffsets_[bucket + 1u]; ++i)
    {
      consider(bucketEntries_[i]);
    }
  };

  const int32_t maximumRing = gridSide_;
  for (int32_t ring = 0; ring <= maximumRing; ++ring)
  {
    if (ring == 0)
    {
      visitCell(centreX, centreY);
    }
    else
    {
      for (int32_t x = centreX - ring; x <= centreX + ring; ++x)
      {
        visitCell(x, centreY - ring);
        visitCell(x, centreY + ring);
      }
      for (int32_t y = centreY - ring + 1; y <= centreY + ring - 1; ++y)
      {
        visitCell(centreX - ring, y);
        visitCell(centreX + ring, y);
      }
    }
    // Nothing in the next ring can be closer than (ring) cells away, since the
    // sample sits somewhere inside its own cell. Once the worst kept hit beats
    // that, the answer is final.
    if (out.size() >= count && out.back().distanceMetres <= static_cast<double>(ring) * cellSize_)
    {
      return;
    }
  }
}

void blendAt(const MapGraph &graph, const SiteIndex &index, const Point2 position, const BlendSettings &settings, std::vector<BlendWeight> &out)
{
  out.clear();
  if (index.empty() || settings.neighbourCount == 0u)
  {
    return;
  }

  // One more than the contributor count: that extra site's distance is the
  // support radius.
  thread_local std::vector<SiteHit> hits;
  index.nearest(position, settings.neighbourCount + 1u, hits);
  if (hits.empty())
  {
    return;
  }
  if (hits.size() == 1u)
  {
    out.push_back(BlendWeight{hits[0].node, 1.0});
    return;
  }

  const size_t contributors = hits.size() - 1u;
  const double radius = hits.back().distanceMetres;
  if (!(radius > 0.0) || hits[0].distanceMetres <= radius * kSiteEpsilon)
  {
    out.push_back(BlendWeight{hits[0].node, 1.0});
    return;
  }

  const auto accumulate = [&](const double exponent) {
    out.clear();
    double total = 0.0;
    for (size_t i = 0u; i < contributors; ++i)
    {
      const double weight = shepardWeight(hits[i].distanceMetres, radius, exponent);
      if (weight <= 0.0)
      {
        continue;
      }
      out.push_back(BlendWeight{hits[i].node, weight});
      total += weight;
    }
    if (!(total > 0.0))
    {
      out.clear();
      out.push_back(BlendWeight{hits[0].node, 1.0});
      return;
    }
    for (BlendWeight &entry : out)
    {
      entry.weight /= total;
    }
  };

  // Two passes. The first is a neutral blend whose only job is to say how much
  // each cell is present here; the second uses that to pick the exponent.
  //
  // The exponent has to be a symmetric function of the whole contributing set
  // rather than of the nearest cell's type, or it would change discontinuously
  // as the sample crossed the midline between two cells of different type -
  // exactly where the blend is most visible.
  accumulate(2.0);
  double exponent = 0.0;
  for (const BlendWeight &a : out)
  {
    for (const BlendWeight &b : out)
    {
      exponent += a.weight * b.weight * transitionSharpness(graph.node(a.node).type, graph.node(b.node).type);
    }
  }
  exponent = std::clamp(exponent * std::max(0.0, settings.sharpnessScale), kMinimumSharpness, kMaximumSharpness);
  accumulate(exponent);
}

void applyCellInfluenceWeights(const MapGraph &graph, const double strength, std::vector<BlendWeight> &weights)
{
  if (weights.size() < 2u)
  {
    return;
  }

  // Logarithmic bias is symmetric around one: with strength one, individual
  // cells range from about 0.7x to 1.4x, or a two-to-one ratio. Multiplying
  // normalized spatial influence by this value is equivalent to a weighted
  // Voronoi field, but retains compact nearest-neighbour support and its
  // continuous falloff.
  const double amount = std::clamp(strength, 0.0, 2.0);
  if (!(amount > 0.0))
  {
    return;
  }
  double total = 0.0;
  for (BlendWeight &entry : weights)
  {
    if (entry.node >= graph.nodeCount())
    {
      entry.weight = 0.0;
      continue;
    }
    const double centred = cellWeightUnit(graph, entry.node) * 2.0 - 1.0;
    entry.weight *= std::exp2(centred * amount * 0.5);
    total += entry.weight;
  }
  if (!(total > 0.0))
  {
    return;
  }
  for (BlendWeight &entry : weights)
  {
    entry.weight /= total;
  }
}

void restrictBlendToSurface(const MapGraph &graph, const bool land, std::vector<BlendWeight> &weights)
{
  if (weights.empty())
  {
    return;
  }
  uint32_t fallback = weights.front().node;
  for (const BlendWeight &weight : weights)
  {
    if (weight.node < graph.nodeCount() && graph.node(weight.node).land == land)
    {
      fallback = weight.node;
      break;
    }
  }
  weights.erase(
      std::remove_if(
          weights.begin(),
          weights.end(),
          [&](const BlendWeight &weight) {
            return weight.node >= graph.nodeCount() || graph.node(weight.node).land != land;
          }),
      weights.end());
  double total = 0.0;
  for (const BlendWeight &weight : weights)
  {
    total += weight.weight;
  }
  if (!(total > 0.0))
  {
    weights = {BlendWeight{fallback, 1.0}};
    return;
  }
  for (BlendWeight &weight : weights)
  {
    weight.weight /= total;
  }
}

void shapeBlendByHeight(const MapGraph &graph, const double elevationMetres, const double localVariationMetres, const double worldReliefMetres, const HeightBlendSettings &settings, std::vector<BlendWeight> &weights)
{
  if (weights.size() < 2u || !(worldReliefMetres > 0.0) || !std::isfinite(elevationMetres) || !std::isfinite(localVariationMetres))
  {
    return;
  }

  const double influence = std::clamp(settings.influence, 0.0, 1.0);
  const double low = std::max(0.0, settings.lowVariationFraction) * worldReliefMetres;
  const double high = std::max(settings.lowVariationFraction, settings.highVariationFraction) * worldReliefMetres;
  const double terrain = influence * smoothStep(low, std::max(low + 1.0, high), std::max(0.0, localVariationMetres));
  if (!(terrain > 0.0))
  {
    return;
  }

  const double band = std::max(1.0, std::max(0.001, settings.elevationBandFraction) * worldReliefMetres);
  const double maximumExponent = std::max(1.0, settings.maximumWeightExponent);
  const double exponent = 1.0 + (maximumExponent - 1.0) * terrain;
  double total = 0.0;
  for (BlendWeight &entry : weights)
  {
    if (entry.node >= graph.nodeCount())
    {
      entry.weight = 0.0;
      continue;
    }
    const double delta = std::abs(graph.node(entry.node).intent.elevationMetres - elevationMetres);
    const double normalized = delta / band;
    const double affinity = std::exp(-0.5 * normalized * normalized);
    const double heightGate = (1.0 - terrain) + terrain * (0.08 + 0.92 * affinity);
    entry.weight = std::pow(std::max(0.0, entry.weight), exponent) * heightGate;
    total += entry.weight;
  }
  if (!(total > 0.0))
  {
    return;
  }
  for (BlendWeight &entry : weights)
  {
    entry.weight /= total;
  }
}

} // namespace procedural::world
