#include "procedural/world/MapHierarchy.hpp"

#include "procedural/PoissonDiskSampler.hpp"
#include "procedural/world/MapCoast.hpp"
#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapPartition.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace procedural::world {

namespace {

uint64_t hashCombine(uint64_t value)
{
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  value *= 0xc4ceb9fe1a85ec53ull;
  value ^= value >> 33u;
  return value;
}

Biome childBiomeVariant(const Biome parent, const uint64_t draw)
{
  // Local heterogeneity within one coarse ecological prior. These are close
  // transitions rather than arbitrary biome replacement; climate-scale
  // decisions still belong to the root map.
  switch (parent)
  {
  case Biome::Grassland: return draw & 1u ? Biome::Shrubland : Biome::Steppe;
  case Biome::Steppe: return draw & 1u ? Biome::Grassland : Biome::Shrubland;
  case Biome::Shrubland: return draw & 1u ? Biome::Grassland : Biome::Steppe;
  case Biome::TemperateForest: return draw & 1u ? Biome::Grassland : Biome::Shrubland;
  case Biome::TemperateRainforest: return draw & 1u ? Biome::TemperateForest : Biome::Wetland;
  case Biome::Mediterranean: return draw & 1u ? Biome::Shrubland : Biome::Grassland;
  case Biome::Taiga: return draw & 1u ? Biome::Tundra : Biome::AlpineMeadow;
  case Biome::Savanna: return draw & 1u ? Biome::Grassland : Biome::Shrubland;
  case Biome::TropicalSeasonalForest: return draw & 1u ? Biome::Savanna : Biome::TropicalRainforest;
  case Biome::TropicalRainforest: return draw & 1u ? Biome::TropicalSeasonalForest : Biome::Wetland;
  case Biome::MontaneForest: return draw & 1u ? Biome::AlpineMeadow : Biome::TemperateForest;
  case Biome::Tundra: return draw & 1u ? Biome::ColdDesert : Biome::AlpineMeadow;
  case Biome::Desert: return draw & 1u ? Biome::SandDesert : Biome::Shrubland;
  case Biome::SandDesert: return Biome::Desert;
  default: return parent;
  }
}

// A node's refinement seed. Derived from the world seed and the node's own
// identity and nothing else, so refining a node twice - or refining it after
// its siblings, or before them - gives the same cells.
uint64_t refinementSeed(const uint64_t worldSeed, const MapNode &node)
{
  uint64_t value = worldSeed ^ 0x5eed1e55ull;
  value = hashCombine(value ^ static_cast<uint64_t>(node.id) * 0x9e3779b97f4a7c15ull);
  value = hashCombine(value ^ static_cast<uint64_t>(node.level + 1u) * 0xbf58476d1ce4e5b9ull);
  return value;
}

bool pointInPolygon(const std::vector<Point2> &polygon, const Point2 &point)
{
  bool inside = false;
  for (size_t index = 0u, previous = polygon.size() - 1u; index < polygon.size(); previous = index++)
  {
    const Point2 &a = polygon[index];
    const Point2 &b = polygon[previous];
    if ((a.y > point.y) != (b.y > point.y) && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)
    {
      inside = !inside;
    }
  }
  return inside;
}

void polygonBounds(const std::vector<Point2> &polygon, Point2 &low, Point2 &high)
{
  low = Point2(polygon.front().x, polygon.front().y);
  high = low;
  for (const Point2 &vertex : polygon)
  {
    low.x = std::min(low.x, vertex.x);
    low.y = std::min(low.y, vertex.y);
    high.x = std::max(high.x, vertex.x);
    high.y = std::max(high.y, vertex.y);
  }
}

double polygonArea(const std::vector<Point2> &polygon)
{
  if (polygon.size() < 3u)
  {
    return 0.0;
  }
  double twice = 0.0;
  for (size_t index = 0u; index < polygon.size(); ++index)
  {
    const Point2 &here = polygon[index];
    const Point2 &next = polygon[(index + 1u) % polygon.size()];
    twice += here.x * next.y - next.x * here.y;
  }
  return std::abs(twice) * 0.5;
}

// Counter-clockwise winding, which the convex clipper's inside test assumes.
void makeCounterClockwise(std::vector<Point2> &polygon)
{
  double twice = 0.0;
  for (size_t index = 0u; index < polygon.size(); ++index)
  {
    const Point2 &here = polygon[index];
    const Point2 &next = polygon[(index + 1u) % polygon.size()];
    twice += here.x * next.y - next.x * here.y;
  }
  if (twice < 0.0)
  {
    std::reverse(polygon.begin(), polygon.end());
  }
}

double segmentDistance(const Point2 &a, const Point2 &b, const Point2 &c, const Point2 &d)
{
  const auto pointToSegment = [](const Point2 &p, const Point2 &s, const Point2 &e) {
    const double dx = e.x - s.x;
    const double dy = e.y - s.y;
    const double lengthSquared = dx * dx + dy * dy;
    double t = 0.0;
    if (lengthSquared > 0.0)
    {
      t = std::clamp(((p.x - s.x) * dx + (p.y - s.y) * dy) / lengthSquared, 0.0, 1.0);
    }
    const double px = s.x + dx * t - p.x;
    const double py = s.y + dy * t - p.y;
    return std::sqrt(px * px + py * py);
  };
  return std::min(std::min(pointToSegment(a, c, d), pointToSegment(b, c, d)), std::min(pointToSegment(c, a, b), pointToSegment(d, a, b)));
}

// Do two polygons share a length of boundary?
//
// Children of different parents are clipped against the same parent edge, but
// against it independently, so their vertices along it do not coincide and a
// shared-vertex test finds nothing. Testing the edges for near-coincidence is
// what actually links the two sides of a parent boundary - and without that
// link the finest level is a set of disconnected islands, one per parent, and
// every stage that walks neighbours quietly stops at a parent edge.
bool sharesBoundary(const std::vector<Point2> &lhs, const std::vector<Point2> &rhs, const double tolerance)
{
  for (size_t i = 0u; i < lhs.size(); ++i)
  {
    const Point2 &a = lhs[i];
    const Point2 &b = lhs[(i + 1u) % lhs.size()];
    for (size_t j = 0u; j < rhs.size(); ++j)
    {
      const Point2 &c = rhs[j];
      const Point2 &d = rhs[(j + 1u) % rhs.size()];
      if (segmentDistance(a, b, c, d) > tolerance)
      {
        continue;
      }
      // Touching at a single corner is not a shared boundary; require the two
      // edges to be near-parallel and overlapping.
      const double abx = b.x - a.x;
      const double aby = b.y - a.y;
      const double cdx = d.x - c.x;
      const double cdy = d.y - c.y;
      const double abLength = std::sqrt(abx * abx + aby * aby);
      const double cdLength = std::sqrt(cdx * cdx + cdy * cdy);
      if (abLength <= tolerance || cdLength <= tolerance)
      {
        continue;
      }
      const double cross = std::abs(abx * cdy - aby * cdx) / (abLength * cdLength);
      if (cross < 0.05)
      {
        return true;
      }
    }
  }
  return false;
}

// Sites for one level of the ladder.
//
// The shared dart-throwing sampler is reliable in bulk and not at all reliable
// in the small. Measured over a 361 km world, its yield against the ideal count
// is 0.58-0.60 for radii of 20 km and below, and then 0.39 at 40 km, 0.44 at
// 80 km and **0.25** at 60 km - erratic, because a coarse radius leaves only a
// handful of grid cells and boundary rejection dominates. At 60 km it returned
// nine sites for a wanted thirty-six, and with nine sites almost every one is
// on the convex hull, so its Voronoi cell is unbounded, gets dropped, and the
// level came out with four cells.
//
// So: sample at a radius well inside the reliable regime and thin the result
// down to the count the level actually wants, by farthest-point selection. The
// spacing guarantee becomes a target rather than a hard minimum, which for a
// realm-scale cell is the right trade - and the selection is deterministic, so
// the world stays reproducible.
std::vector<Point2> spreadSites(const double spacing, const double width, const double height, const uint64_t seed)
{
  if (!(spacing > 0.0) || !(width > 0.0) || !(height > 0.0))
  {
    return {};
  }

  // 0.6 is the sampler's own asymptotic yield, so a level asked for a spacing
  // gets the count it would have got had the sampler behaved at that radius.
  const size_t wanted = static_cast<size_t>(std::max<int64_t>(1, std::llround(width * height * 0.6 / (spacing * spacing))));

  PoissonDiskSampler sampler;
  PoissonDiskSampler::Settings settings{};
  settings.radius = spacing * 0.45;
  settings.width = width;
  settings.height = height;
  settings.seed = seed;
  settings.method = PoissonDiskSampler::Method::RandomThrows;
  std::vector<Point2> candidates = sampler.generate(settings);
  if (candidates.size() <= wanted)
  {
    return candidates;
  }

  // Farthest-point thinning: start from the candidate nearest the middle, then
  // repeatedly take whichever is furthest from everything already taken. Ties
  // on index, so the choice never depends on iteration order.
  std::vector<uint8_t> taken(candidates.size(), 0u);
  std::vector<double> spread(candidates.size(), std::numeric_limits<double>::infinity());
  const Point2 middle(width * 0.5, height * 0.5);
  size_t current = 0u;
  double best = std::numeric_limits<double>::infinity();
  for (size_t index = 0u; index < candidates.size(); ++index)
  {
    const double dx = candidates[index].x - middle.x;
    const double dy = candidates[index].y - middle.y;
    const double distance = dx * dx + dy * dy;
    if (distance < best)
    {
      best = distance;
      current = index;
    }
  }

  std::vector<Point2> sites;
  sites.reserve(wanted);
  for (size_t placed = 0u; placed < wanted; ++placed)
  {
    taken[current] = 1u;
    sites.push_back(candidates[current]);
    size_t next = candidates.size();
    double furthest = -1.0;
    for (size_t index = 0u; index < candidates.size(); ++index)
    {
      if (taken[index] != 0u)
      {
        continue;
      }
      const double dx = candidates[index].x - candidates[current].x;
      const double dy = candidates[index].y - candidates[current].y;
      spread[index] = std::min(spread[index], dx * dx + dy * dy);
      if (spread[index] > furthest)
      {
        furthest = spread[index];
        next = index;
      }
    }
    if (next == candidates.size())
    {
      break;
    }
    current = next;
  }
  return sites;
}

void link(MapGraph &graph, const uint32_t lhs, const uint32_t rhs)
{
  if (lhs == rhs)
  {
    return;
  }
  auto &list = graph.node(lhs).neighbours;
  if (std::find(list.begin(), list.end(), rhs) == list.end())
  {
    list.push_back(rhs);
  }
  auto &other = graph.node(rhs).neighbours;
  if (std::find(other.begin(), other.end(), lhs) == other.end())
  {
    other.push_back(lhs);
  }
}

} // namespace

namespace {

void collectLeaves(const MapGraph &graph, const uint32_t node, std::vector<uint32_t> &out)
{
  if (!graph.node(node).refined())
  {
    out.push_back(node);
    return;
  }
  for (const uint32_t child : graph.node(node).children)
  {
    collectLeaves(graph, child, out);
  }
}

} // namespace

double landFractionOf(const MapGraph &graph, const uint32_t node)
{
  if (node >= graph.nodeCount())
  {
    return 0.0;
  }
  std::vector<uint32_t> leaves;
  collectLeaves(graph, node, leaves);
  double total = 0.0;
  double land = 0.0;
  for (const uint32_t leaf : leaves)
  {
    const double area = polygonArea(graph.node(leaf).area);
    total += area;
    if (graph.node(leaf).land)
    {
      land += area;
    }
  }
  return total > 0.0 ? land / total : 0.0;
}

void aggregateToParents(MapGraph &graph)
{
  // Deepest first, so a node's children are already summarised when it is
  // reached. Levels are small and bounded, so a sort by level is enough of an
  // ordering; no traversal needed.
  std::vector<uint32_t> order;
  order.reserve(graph.nodeCount());
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined())
    {
      order.push_back(node.id);
    }
  }
  std::sort(order.begin(), order.end(), [&graph](const uint32_t lhs, const uint32_t rhs) {
    return graph.node(lhs).level > graph.node(rhs).level;
  });

  for (const uint32_t id : order)
  {
    const std::vector<uint32_t> children = graph.node(id).children;
    if (children.empty())
    {
      continue;
    }

    double total = 0.0;
    double land = 0.0;
    double elevation = 0.0;
    double uplift = 0.0;
    double relief = 0.0;
    double roughness = 0.0;
    bool coastal = false;
    bool shallow = false;
    std::vector<double> byType(static_cast<size_t>(CellType::Count), 0.0);
    std::vector<double> byContent(static_cast<size_t>(CellType::Count), 0.0);
    for (const uint32_t child : children)
    {
      const MapNode &node = graph.node(child);
      const double area = std::max(1.0, polygonArea(node.area));
      total += area;
      if (node.land)
      {
        land += area;
      }
      elevation += node.intent.elevationMetres * area;
      uplift += node.intent.upliftRate * area;
      relief += node.intent.reliefAmplitude * area;
      roughness += node.intent.roughness * area;
      coastal = coastal || node.coastal;
      shallow = shallow || node.shallow;
      byType[static_cast<size_t>(node.type)] += area;
      byContent[static_cast<size_t>(node.content)] += area;
    }
    if (total <= 0.0)
    {
      continue;
    }

    MapNode &parent = graph.node(id);
    // Land by majority of area. A coarse cell that is half sea is drawn, and
    // treated, as whichever it mostly is; the detail underneath still says
    // exactly where the coast runs.
    parent.land = land * 2.0 > total;
    parent.intent.elevationMetres = elevation / total;
    parent.intent.upliftRate = uplift / total;
    parent.intent.reliefAmplitude = relief / total;
    parent.intent.roughness = roughness / total;
    parent.coastal = coastal;
    parent.shallow = shallow;

    // Dominant type and content, but only among those that agree with the
    // parent's own land flag - otherwise a mostly-land cell can end up typed
    // Ocean, which validate() rejects and which would be wrong anyway.
    const auto dominant = [&](const std::vector<double> &weights) {
      size_t best = static_cast<size_t>(CellType::Count);
      double bestWeight = -1.0;
      for (size_t index = 0u; index < weights.size(); ++index)
      {
        const CellType type = static_cast<CellType>(index);
        if (cellTypeIsWater(type) == parent.land)
        {
          continue;
        }
        if (weights[index] > bestWeight)
        {
          bestWeight = weights[index];
          best = index;
        }
      }
      return best;
    };
    const size_t type = dominant(byType);
    const size_t content = dominant(byContent);
    parent.type = type < static_cast<size_t>(CellType::Count) ? static_cast<CellType>(type) : (parent.land ? CellType::Plain : CellType::Ocean);
    parent.content = content < static_cast<size_t>(CellType::Count) ? static_cast<CellType>(content) : parent.type;
  }
}

std::vector<uint32_t> leafNodes(const MapGraph &graph)
{
  std::vector<uint32_t> leaves;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.refined())
    {
      leaves.push_back(node.id);
    }
  }
  return leaves;
}

bool refineNode(const uint32_t node, const double cellSpacingMetres, const uint64_t seed, MapGraph &graph, std::string *error)
{
  const auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }
    return false;
  };

  if (node >= graph.nodeCount())
  {
    return fail("cannot refine a node that does not exist");
  }
  if (!(cellSpacingMetres > 0.0))
  {
    return fail("refinement cell spacing must be positive");
  }
  if (graph.node(node).refined())
  {
    return fail("cannot refine a node that already has children");
  }

  std::vector<Point2> window = graph.node(node).area;
  if (window.size() < 3u)
  {
    return true;
  }
  makeCounterClockwise(window);

  Point2 low{};
  Point2 high{};
  polygonBounds(window, low, high);
  const double width = high.x - low.x;
  const double height = high.y - low.y;
  if (!(width > 0.0) || !(height > 0.0))
  {
    return true;
  }

  // A parent too small to hold a few cells is left as a leaf. Refining it would
  // produce one child identical to itself, which is a level of hierarchy that
  // costs memory and says nothing.
  if (polygonArea(window) < cellSpacingMetres * cellSpacingMetres * 3.0)
  {
    return true;
  }

  const uint64_t nodeSeed = refinementSeed(seed, graph.node(node));
  // Sampled over the parent's bounding box and then filtered to the polygon.
  // Sampling the polygon directly would make the site set depend on the
  // rejection order, and RandomThrows is chosen precisely because it does not.
  std::vector<Point2> sites;
  for (const Point2 &sample : spreadSites(cellSpacingMetres, width, height, nodeSeed))
  {
    const Point2 site(low.x + sample.x, low.y + sample.y);
    if (pointInPolygon(window, site))
    {
      sites.push_back(site);
    }
  }
  if (sites.size() < 3u)
  {
    return true;
  }

  std::vector<VoronoiCell> cells;
  // A parent whose sites give no usable cells stays a leaf. It is not a failure
  // of the world: an awkwardly shaped or nearly degenerate cell simply has no
  // sensible refinement, and aborting the whole build over one of them meant a
  // two-level hierarchy failed outright with "partition produced no cells".
  if (!buildVoronoiCells(sites, window, cellSpacingMetres * 1e-6, cells, nullptr))
  {
    return true;
  }

  // The children must actually tile the parent. Nothing downstream re-checks
  // this: a refined parent is never drawn or sampled again, so children that
  // cover none of it turn the parent's ground into a hole in the world that no
  // stage reports and no test that counts cells can see. One malformed border
  // ring did exactly that - seven zero-area children over a 1,549 km2 parent,
  // six percent of the map gone. Refusing the refinement leaves a coarse cell,
  // which is merely blunt where the alternative is missing.
  double covered = 0.0;
  for (const VoronoiCell &cell : cells)
  {
    covered += polygonArea(cell.area);
  }
  if (covered < polygonArea(window) * 0.5)
  {
    return true;
  }

  const MapNode parentCopy = graph.node(node);
  const auto edgeDistance = [](const Point2 &p, const std::vector<Point2> &polygon)
  {
    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < polygon.size(); ++i)
    {
      const Point2 &a = polygon[i], &b = polygon[(i + 1) % polygon.size()];
      const double dx = b.x - a.x, dy = b.y - a.y;
      const double dd = dx * dx + dy * dy;
      const double t = dd > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / dd, 0.0, 1.0) : 0.0;
      best = std::min(best, std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t)));
    }
    return best;
  };
  std::vector<uint32_t> created;
  created.reserve(cells.size());
  for (VoronoiCell &cell : cells)
  {
    MapNode child{};
    child.level = static_cast<uint8_t>(parentCopy.level + 1u);
    child.parent = node;
    child.site = cell.site;
    child.area = std::move(cell.area);
    // A child inherits its parent's classification. Nothing here decides land
    // or content; the raiser and the later stages do that on the leaves.
    child.type = parentCopy.type;
    child.content = parentCopy.content;
    child.land = parentCopy.land;
    child.biome = parentCopy.biome;
    child.region = parentCopy.region;
    child.craton = parentCopy.craton;
    child.coastal = parentCopy.coastal;
    child.shallow = parentCopy.shallow;
    child.intent = parentCopy.intent;

    uint64_t localHash = seed ^ (uint64_t(std::llround(child.site.x * 16)) * 0x9e3779b97f4a7c15ULL) ^ (uint64_t(std::llround(child.site.y * 16)) * 0xbf58476d1ce4e5b9ULL);
    localHash = hashCombine(localHash);
    // About one child in five becomes a compatible sub-biome. Clusters arise
    // from the spatially quantized seed while the majority preserves the root
    // assignment. Forest clearings also change cover; grass/scrub variants
    // remain open ground with the same authoritative land mask.
    if (child.land && double(localHash & 0xffffu) / 65535.0 < .22)
    {
      const Biome parentBiome = biomeOf(parentCopy);
      const Biome variant = childBiomeVariant(parentBiome, localHash >> 16);
      setBiome(child, variant);
      if ((parentBiome == Biome::TemperateForest || parentBiome == Biome::TemperateRainforest || parentBiome == Biome::MontaneForest || parentBiome == Biome::Taiga) &&
          (variant == Biome::Grassland || variant == Biome::Shrubland || variant == Biome::AlpineMeadow || variant == Biome::Tundra))
        child.content = CellType::Plain;
    }

    // Root cells own the coarse ecological decision, but their boundary must
    // not print through the refined world. Compatible neighbouring land may
    // diffuse its landform/cover/biome into a narrow, deterministic band of
    // child cells. Water/land remains a hard boundary and rivers stay in the
    // hydrology product. Both sides perform this rule, producing an irregular
    // interleaved transition instead of a second straight contour.
    const MapNode *donor = nullptr;
    // Several child cells participate in a transition. A one-cell band still
    // traced the parent polygon almost perfectly at map scale.
    const double transitionWidth = cellSpacingMetres * 4.5;
    double donorDistance = transitionWidth;
    for (const uint32_t neighbourId : parentCopy.neighbours)
    {
      const MapNode &neighbour = graph.node(neighbourId);
      if (neighbour.land != parentCopy.land || neighbour.area.size() < 3 || neighbour.biome == parentCopy.biome)
        continue;
      const double d = edgeDistance(child.site, neighbour.area);
      if (d < donorDistance)
      {
        donorDistance = d;
        donor = &neighbour;
      }
    }
    if (donor)
    {
      const double draw = double((localHash >> 32) & 0xffffu) / 65535.0;
      const double chance = .78 * (1.0 - donorDistance / transitionWidth);
      if (draw < chance)
      {
        child.type = donor->type;
        child.content = donor->content;
        child.biome = donor->biome;
        child.craton = donor->craton;
      }
    }
    created.push_back(graph.addNode(std::move(child)));
  }
  for (size_t index = 0u; index < cells.size(); ++index)
  {
    for (const uint32_t neighbour : cells[index].neighbours)
    {
      link(graph, created[index], created[neighbour]);
    }
  }
  graph.node(node).children = created;
  return true;
}

bool buildHierarchy(const HierarchySettings &settings, MapGraph &graph, std::string *error)
{
  const auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }
    return false;
  };

  if (settings.cellSpacingMetres.empty())
  {
    return fail("a hierarchy needs at least one level");
  }
  if (!(settings.extentMetres > 0.0))
  {
    return fail("hierarchy extent must be positive");
  }
  for (size_t level = 1u; level < settings.cellSpacingMetres.size(); ++level)
  {
    if (!(settings.cellSpacingMetres[level] < settings.cellSpacingMetres[level - 1u]))
    {
      return fail("hierarchy levels must get finer, coarsest first");
    }
  }

  const double extent = settings.extentMetres;
  const std::vector<Point2> coarseSites = spreadSites(settings.cellSpacingMetres.front(), extent, extent, settings.seed);
  if (coarseSites.size() < 3u)
  {
    return fail("the coarsest level produced fewer than three sites");
  }

  // The world rectangle is the window, so the coarsest level's border cells are
  // bounded here rather than left unbounded for clipCellsToExtent to rescue.
  const std::vector<Point2> world{Point2(0.0, 0.0), Point2(extent, 0.0), Point2(extent, extent), Point2(0.0, extent)};
  std::vector<VoronoiCell> coarseCells;
  if (!buildVoronoiCells(coarseSites, world, settings.cellSpacingMetres.front() * 1e-6, coarseCells, error))
  {
    return false;
  }

  std::vector<uint32_t> ids;
  ids.reserve(coarseCells.size());
  for (VoronoiCell &cell : coarseCells)
  {
    MapNode node{};
    node.level = 0u;
    node.site = cell.site;
    node.area = std::move(cell.area);
    node.type = CellType::Ocean;
    node.content = CellType::Ocean;
    node.land = false;
    ids.push_back(graph.addNode(std::move(node)));
  }
  for (size_t index = 0u; index < coarseCells.size(); ++index)
  {
    for (const uint32_t neighbour : coarseCells[index].neighbours)
    {
      link(graph, ids[index], ids[neighbour]);
    }
  }

  for (size_t level = 1u; level < settings.cellSpacingMetres.size(); ++level)
  {
    const double spacing = settings.cellSpacingMetres[level];
    // Snapshot: refining appends nodes, and a parent must not be re-refined as
    // its own grandchild.
    std::vector<uint32_t> parents;
    for (const MapNode &node : graph.nodes())
    {
      if (node.level + 1u == level && !node.refined())
      {
        parents.push_back(node.id);
      }
    }

    for (const uint32_t parent : parents)
    {
      if (!refineNode(parent, spacing, settings.seed, graph, error))
      {
        return false;
      }
    }

    // Children of neighbouring parents that share a length of the parents'
    // boundary. Without this the level is one disconnected island per parent.
    //
    // Bounding boxes first. The edge-against-edge test is quadratic in vertex
    // count and almost every pair of children across a parent boundary is
    // nowhere near it: without this broad phase a three-level world did not
    // finish in two minutes, and with it the whole build is milliseconds.
    const double tolerance = spacing * 1e-3;
    const auto boundsOf = [&graph](const uint32_t node) {
      Point2 low{};
      Point2 high{};
      polygonBounds(graph.node(node).area, low, high);
      return std::pair<Point2, Point2>(low, high);
    };
    const auto boundsOverlap = [tolerance](const std::pair<Point2, Point2> &a, const std::pair<Point2, Point2> &b) {
      return a.first.x - tolerance <= b.second.x && b.first.x - tolerance <= a.second.x && a.first.y - tolerance <= b.second.y && b.first.y - tolerance <= a.second.y;
    };

    for (const uint32_t parent : parents)
    {
      const std::vector<uint32_t> children = graph.node(parent).children;
      if (children.empty())
      {
        continue;
      }
      std::vector<std::pair<Point2, Point2>> childBounds;
      childBounds.reserve(children.size());
      for (const uint32_t child : children)
      {
        childBounds.push_back(boundsOf(child));
      }

      for (const uint32_t other : graph.node(parent).neighbours)
      {
        // Once per pair.
        if (other < parent)
        {
          continue;
        }
        const std::vector<uint32_t> &otherChildren = graph.node(other).children;
        if (otherChildren.empty())
        {
          // The neighbour was too small to refine, so it stays a leaf. Its
          // children are itself: link it to whatever touches it, or the two
          // levels stop talking to each other along that edge.
          const std::pair<Point2, Point2> otherBounds = boundsOf(other);
          for (size_t index = 0u; index < children.size(); ++index)
          {
            if (boundsOverlap(childBounds[index], otherBounds) && sharesBoundary(graph.node(children[index]).area, graph.node(other).area, tolerance))
            {
              link(graph, children[index], other);
            }
          }
          continue;
        }
        for (const uint32_t otherChild : otherChildren)
        {
          const std::pair<Point2, Point2> otherBounds = boundsOf(otherChild);
          for (size_t index = 0u; index < children.size(); ++index)
          {
            if (boundsOverlap(childBounds[index], otherBounds) && sharesBoundary(graph.node(children[index]).area, graph.node(otherChild).area, tolerance))
            {
              link(graph, children[index], otherChild);
            }
          }
        }
      }
    }
  }
  return true;
}

} // namespace procedural::world
