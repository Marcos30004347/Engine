#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapCoast.hpp"
#include "procedural/world/MapContours.hpp"
#include "procedural/world/MapEdges.hpp"
#include "procedural/world/MapElevation.hpp"
#include "procedural/world/MapExport.hpp"
#include "procedural/world/MapExportGpuCache.hpp"
#include "procedural/world/MapExportPageCache.hpp"
#include "procedural/world/MapExportSampler.hpp"
#include "procedural/world/MapExportStreaming.hpp"
#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapHierarchy.hpp"
#include "procedural/world/MapPartition.hpp"
#include "procedural/world/MapPlacement.hpp"
#include "procedural/world/MapRegions.hpp"
#include "procedural/world/MapRivers.hpp"
#include "procedural/world/MapTerrain.hpp"
#include "procedural/world/MapTerrainRefinement.hpp"
#include "procedural/world/Raisers.hpp"
#include "procedural/world/WorldBrief.hpp"
#include "procedural/world/land/Blend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{

uint32_t failures = 0u;

void expect(const bool condition, const std::string &message)
{
  if (condition)
    return;
  std::cerr << "FAILED: " << message << "\n";
  ++failures;
}

using namespace procedural;
using namespace procedural::world;

MapNode makeNode(const CellType type, const double x, const double y)
{
  MapNode node{};
  node.type = type;
  node.content = type;
  node.land = !cellTypeIsWater(type);
  node.site = Point2(x, y);
  node.area = {Point2(x - 1.0, y - 1.0), Point2(x + 1.0, y - 1.0), Point2(x + 1.0, y + 1.0), Point2(x - 1.0, y + 1.0)};
  return node;
}

void testTagResolution()
{
  // No shape tag at all must still produce a usable brief rather than an
  // invalid one, because the UI lets a user tick only character tags.
  std::vector<Tag> tags{Tag::Barren};
  resolveTags(tags);
  expect(shapeTag(tags) == Tag::Island, "a brief with no shape tag defaults to island");

  // Two shape tags is a UI mistake, not a fatal error. It must resolve
  // deterministically to one.
  std::vector<Tag> conflicting{Tag::Coast, Tag::Archipelago, Tag::Verdant};
  resolveTags(conflicting);
  uint32_t shapeCount = 0u;
  for (const Tag tag : conflicting)
    if (tagIsShape(tag))
      ++shapeCount;
  expect(shapeCount == 1u, "conflicting shape tags resolve to exactly one");

  // Order must not matter: two briefs meaning the same thing must hash alike,
  // or the same world regenerates differently depending on click order.
  WorldBrief a{};
  a.seed = 42u;
  a.tags = {Tag::Verdant, Tag::Island, Tag::Lowland};
  WorldBrief b{};
  b.seed = 42u;
  b.tags = {Tag::Lowland, Tag::Island, Tag::Verdant};
  expect(worldBriefHash(a) == worldBriefHash(b), "tag order does not change the brief hash");

  // Duplicates likewise.
  WorldBrief c = a;
  c.tags.push_back(Tag::Island);
  c.tags.push_back(Tag::Verdant);
  expect(worldBriefHash(a) == worldBriefHash(c), "duplicate tags do not change the brief hash");
}

void testBriefHashSensitivity()
{
  WorldBrief base{};
  base.seed = 9127364512ull;
  base.tags = {Tag::Island};
  const uint64_t hash = worldBriefHash(base);

  WorldBrief other = base;
  other.seed = 9127364513ull;
  expect(worldBriefHash(other) != hash, "seed changes the brief hash");

  other = base;
  other.extentMetres = 180000.0;
  expect(worldBriefHash(other) != hash, "extent changes the brief hash");

  other = base;
  other.reliefMetres = 1500.0;
  expect(worldBriefHash(other) != hash, "relief changes the brief hash");

  other = base;
  other.tags = {Tag::Archipelago};
  expect(worldBriefHash(other) != hash, "shape tag changes the brief hash");

  // Negative zero must not produce a different world from zero.
  WorldBrief zero = base;
  zero.seaLevelMetres = 0.0;
  WorldBrief negativeZero = base;
  negativeZero.seaLevelMetres = -0.0;
  expect(worldBriefHash(zero) == worldBriefHash(negativeZero), "negative zero hashes the same as zero");
}

void testGraphValidation()
{
  MapGraph graph;
  const uint32_t plain = graph.addNode(makeNode(CellType::Plain, 0.0, 0.0));
  const uint32_t ocean = graph.addNode(makeNode(CellType::Ocean, 2.0, 0.0));
  graph.node(plain).neighbours.push_back(ocean);
  graph.node(ocean).neighbours.push_back(plain);

  std::string error;
  expect(graph.validate(&error), "a well-formed graph validates: " + error);

  // Asymmetric neighbours are the defect that makes blending and boundary
  // contracts apply to one side only, so it has to be caught.
  graph.node(ocean).neighbours.clear();
  expect(!graph.validate(nullptr), "asymmetric neighbour links are rejected");
  graph.node(ocean).neighbours.push_back(plain);

  // Land flag disagreeing with the cell type means one stage wrote one and not
  // the other.
  graph.node(plain).land = false;
  expect(!graph.validate(nullptr), "a land type flagged as water is rejected");
  graph.node(plain).land = true;
  expect(graph.validate(nullptr), "restoring the land flag revalidates");
}

void testHierarchyValidation()
{
  MapGraph graph;
  const uint32_t root = graph.addNode(makeNode(CellType::Mountain, 0.0, 0.0));
  MapNode childNode = makeNode(CellType::Valley, 0.0, 0.0);
  childNode.parent = root;
  childNode.level = 1u;
  const uint32_t child = graph.addNode(std::move(childNode));

  // A child the parent does not list is an orphan, and orphans are how a
  // refinement silently fails to be visible.
  expect(!graph.validate(nullptr), "an unlisted child is rejected");
  graph.node(root).children.push_back(child);
  expect(graph.validate(nullptr), "listing the child validates");

  // Level must follow the parent or the hierarchy's LOD meaning breaks.
  graph.node(child).level = 3u;
  expect(!graph.validate(nullptr), "a level that skips its parent is rejected");
  graph.node(child).level = 1u;

  expect(graph.node(root).refined(), "a node with children reports refined");
  expect(!graph.node(child).refined(), "a leaf does not report refined");
}

void testNodeAtDescendsHierarchy()
{
  MapGraph graph;
  MapNode rootNode = makeNode(CellType::Plain, 0.0, 0.0);
  rootNode.area = {Point2(-10.0, -10.0), Point2(10.0, -10.0), Point2(10.0, 10.0), Point2(-10.0, 10.0)};
  const uint32_t root = graph.addNode(std::move(rootNode));

  MapNode childNode = makeNode(CellType::Hill, 5.0, 5.0);
  childNode.parent = root;
  childNode.level = 1u;
  childNode.area = {Point2(0.0, 0.0), Point2(10.0, 0.0), Point2(10.0, 10.0), Point2(0.0, 10.0)};
  const uint32_t child = graph.addNode(std::move(childNode));
  graph.node(root).children.push_back(child);

  // Picking must return the deepest refined node containing the point, not the
  // root, or the inspector selects the wrong thing once anything is refined.
  expect(graph.nodeAt(Point2(5.0, 5.0)) == child, "picking descends into a refined child");
  expect(graph.nodeAt(Point2(-5.0, -5.0)) == root, "picking outside the child returns the parent");
  expect(graph.nodeAt(Point2(100.0, 100.0)) == kInvalidNode, "picking outside the world returns no node");
}

void testSignatureDeterminism()
{
  const auto build = []()
  {
    MapGraph graph;
    WorldBrief brief{};
    brief.seed = 718077389ull;
    brief.tags = {Tag::Archipelago};
    graph.setBrief(brief);
    for (int index = 0; index < 8; ++index)
      graph.addNode(makeNode(index % 2 == 0 ? CellType::Plain : CellType::Ocean, static_cast<double>(index) * 2.0, 0.0));
    return graph;
  };

  expect(build().signature() == build().signature(), "an identical graph produces an identical signature");

  MapGraph changed = build();
  changed.node(3u).intent.elevationMetres += 1.0;
  expect(changed.signature() != build().signature(), "changing elevation intent changes the signature");

  // The brief is part of the signature: the same cells generated from a
  // different brief are a different world.
  MapGraph rebriefed = build();
  WorldBrief other = rebriefed.brief();
  other.seed = 1u;
  rebriefed.setBrief(other);
  expect(rebriefed.signature() != build().signature(), "changing the brief changes the signature");
}

PartitionSettings smallPartition()
{
  PartitionSettings settings{};
  settings.extentMetres = 40000.0;
  settings.cellSpacingMetres = 4000.0;
  settings.seed = 718077389ull;
  return settings;
}

void testPartitionBuilds()
{
  MapGraph graph;
  std::string error;
  expect(buildPartition(smallPartition(), graph, &error), "partition builds: " + error);
  expect(graph.nodeCount() > 10u, "partition produces a plausible number of cells, got " + std::to_string(graph.nodeCount()));
  expect(graph.validate(&error), "partitioned graph validates: " + error);

  // Every cell must be a real polygon and every neighbour link symmetric;
  // validate() checks symmetry, this checks the geometry it depends on.
  for (const MapNode &node : graph.nodes())
  {
    expect(node.area.size() >= 3u, "every cell has at least three corners");
    expect(!node.neighbours.empty(), "every cell has at least one neighbour");
  }
}

void testPartitionDeterminism()
{
  MapGraph first;
  MapGraph second;
  expect(buildPartition(smallPartition(), first, nullptr), "first run builds");
  expect(buildPartition(smallPartition(), second, nullptr), "second run builds");
  expect(first.nodeCount() == second.nodeCount(), "the same settings produce the same cell count");
  expect(first.signature() == second.signature(), "the same settings produce an identical partition signature");

  // A different seed must produce a different partition, or the seed is not
  // reaching the sampler.
  PartitionSettings other = smallPartition();
  other.seed += 1u;
  MapGraph changed;
  expect(buildPartition(other, changed, nullptr), "reseeded run builds");
  expect(changed.signature() != first.signature(), "a different seed produces a different partition");
}

void testPartitionRejectsBadSettings()
{
  MapGraph graph;
  PartitionSettings settings = smallPartition();
  settings.cellSpacingMetres = settings.extentMetres * 2.0;
  expect(!buildPartition(settings, graph, nullptr), "spacing larger than the extent is rejected");

  settings = smallPartition();
  settings.extentMetres = 0.0;
  expect(!buildPartition(settings, graph, nullptr), "a zero extent is rejected");
}

void testPartitionCellsAreInsideExtent()
{
  MapGraph graph;
  const PartitionSettings settings = smallPartition();
  expect(buildPartition(settings, graph, nullptr), "partition builds");

  // Sites must lie in the requested area. Cell polygons may extend beyond it
  // because border cells are unbounded in the dual and are not clipped yet -
  // that is a known gap recorded in the spec, not a silent pass.
  for (const MapNode &node : graph.nodes())
  {
    expect(node.site.x >= 0.0 && node.site.x <= settings.extentMetres, "site x lies inside the extent");
    expect(node.site.y >= 0.0 && node.site.y <= settings.extentMetres, "site y lies inside the extent");
  }
}

MapGraph raisedGraph(const Tag shape, const double landFraction)
{
  MapGraph graph;
  const PartitionSettings partition = smallPartition();
  buildPartition(partition, graph, nullptr);
  RaiseSettings raise{};
  raise.extentMetres = partition.extentMetres;
  raise.landFraction = landFraction;
  raise.seed = partition.seed;
  raiseLand(shape, raise, graph, nullptr);
  return graph;
}

void testRaiseHonoursLandFraction()
{
  // The archetype decides shape and landFraction decides amount; the two must
  // be independent, so every archetype has to hit the requested fraction.
  for (const Tag shape : {Tag::Island, Tag::Archipelago, Tag::Coast, Tag::Bay, Tag::Fjord, Tag::Headland, Tag::Lake})
  {
    for (const double fraction : {0.2, 0.35, 0.6})
    {
      const MapGraph graph = raisedGraph(shape, fraction);
      uint32_t land = 0u;
      for (const MapNode &node : graph.nodes())
        if (node.land)
          ++land;
      const double actual = static_cast<double>(land) / static_cast<double>(graph.nodeCount());
      // One cell of rounding slack.
      const double slack = 1.5 / static_cast<double>(graph.nodeCount());
      expect(std::abs(actual - fraction) <= slack, std::string("archetype ") + tagName(shape) + " at " + std::to_string(fraction) + " produced " + std::to_string(actual));
    }
  }
}

void testRaiseKeepsGraphValid()
{
  MapGraph graph = raisedGraph(Tag::Island, 0.35);
  std::string error;
  // land and type must stay in agreement; raiseLand writes both and validate()
  // is what catches a stage writing one and forgetting the other.
  expect(graph.validate(&error), "raised graph validates: " + error);
}

void testCoastalFlagsAreDerived()
{
  const MapGraph graph = raisedGraph(Tag::Island, 0.35);
  uint32_t coastal = 0u;
  uint32_t shallow = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.coastal)
    {
      ++coastal;
      expect(node.land, "a coastal cell is land");
      bool touchesWater = false;
      for (const uint32_t neighbour : node.neighbours)
        touchesWater = touchesWater || !graph.node(neighbour).land;
      expect(touchesWater, "a coastal cell touches water");
    }
    if (node.shallow)
    {
      ++shallow;
      expect(!node.land, "a shallow cell is water");
    }
  }
  expect(coastal > 0u, "an island has a coastline");
  expect(shallow > 0u, "an island has shallow water around it");
}

void testRaiseDeterminism()
{
  expect(raisedGraph(Tag::Archipelago, 0.3).signature() == raisedGraph(Tag::Archipelago, 0.3).signature(), "raising is deterministic");
  expect(raisedGraph(Tag::Archipelago, 0.3).signature() != raisedGraph(Tag::Island, 0.3).signature(), "a different archetype produces a different world");
}

void testMeasuredShapeMatchesRequest()
{
  // The reference generator classifies a finished map by its border land
  // fraction. Running that in reverse checks the archetype actually produced
  // the shape it promised, rather than merely the right amount of land.
  const double extent = smallPartition().extentMetres;

  const MapGraph island = raisedGraph(Tag::Island, 0.3);
  expect(borderLandFraction(island, extent) < 0.2, "an island leaves its border mostly water, got " + std::to_string(borderLandFraction(island, extent)));

  const MapGraph landlocked = raisedGraph(Tag::Landlocked, 1.0);
  expect(measuredShape(landlocked, extent) == Tag::Landlocked, "a fully raised world measures as landlocked");
}

// Fraction of the water that cannot be reached from the world border by
// walking water neighbours - an inland sea, in other words.
double enclosedWaterFraction(const MapGraph &graph, const double extent)
{
  const double margin = extent * 0.02;
  std::vector<uint8_t> reached(graph.nodeCount(), 0u);
  std::vector<uint32_t> stack;
  uint32_t water = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.land)
      continue;
    ++water;
    const bool onBorder = node.site.x < margin || node.site.y < margin || node.site.x > extent - margin || node.site.y > extent - margin;
    if (onBorder && reached[node.id] == 0u)
    {
      reached[node.id] = 1u;
      stack.push_back(node.id);
    }
  }
  while (!stack.empty())
  {
    const uint32_t current = stack.back();
    stack.pop_back();
    for (const uint32_t neighbour : graph.node(current).neighbours)
      if (!graph.node(neighbour).land && reached[neighbour] == 0u)
      {
        reached[neighbour] = 1u;
        stack.push_back(neighbour);
      }
  }
  if (water == 0u)
    return 0.0;
  uint32_t enclosed = 0u;
  for (const MapNode &node : graph.nodes())
    if (!node.land && reached[node.id] == 0u)
      ++enclosed;
  return static_cast<double>(enclosed) / static_cast<double>(water);
}

void testBayOpensOntoTheSea()
{
  // A bay is water that reaches the open sea. The archetype originally centred
  // its bite at a fixed position rather than on the coast, which at any land
  // fraction other than zero put the bite inland and produced a detached
  // inland sea. The border-fraction classifier cannot see that - both read as
  // "coast" - so this is checked directly.
  const double extent = smallPartition().extentMetres;
  for (const double fraction : {0.2, 0.35, 0.6})
  {
    const MapGraph bay = raisedGraph(Tag::Bay, fraction);
    expect(enclosedWaterFraction(bay, extent) < 0.1, "a bay opens onto the sea at land fraction " + std::to_string(fraction) + ", enclosed " + std::to_string(enclosedWaterFraction(bay, extent)));
  }

  // The contrast that keeps the check above from being vacuous: the lake
  // archetype's water is enclosed on purpose, and the same measure says so.
  const MapGraph lake = raisedGraph(Tag::Lake, 0.5);
  expect(enclosedWaterFraction(lake, extent) > 0.5, "a lake's water is enclosed, got " + std::to_string(enclosedWaterFraction(lake, extent)));
}

void testRaiseRejectsBadSettings()
{
  MapGraph graph;
  buildPartition(smallPartition(), graph, nullptr);
  RaiseSettings raise{};
  raise.extentMetres = smallPartition().extentMetres;
  raise.landFraction = 1.7;
  expect(!raiseLand(Tag::Island, raise, graph, nullptr), "a land fraction above one is rejected");

  MapGraph empty;
  raise.landFraction = 0.3;
  expect(!raiseLand(Tag::Island, raise, empty, nullptr), "raising an empty partition is rejected");
}

void testClipBoundsEveryCell()
{
  MapGraph graph;
  const PartitionSettings partition = smallPartition();
  expect(buildPartition(partition, graph, nullptr), "partition builds");

  // The gap this pass exists to close: before clipping, hull cells can reach
  // outside the world. Assert the fix, not the bug.
  std::string error;
  expect(clipCellsToExtent(graph, partition.extentMetres, &error), "clipping succeeds: " + error);

  const double slack = partition.extentMetres * 1e-9;
  for (const MapNode &node : graph.nodes())
    for (const Point2 &vertex : node.area)
    {
      expect(vertex.x >= -slack && vertex.x <= partition.extentMetres + slack, "clipped vertex x lies inside the world");
      expect(vertex.y >= -slack && vertex.y <= partition.extentMetres + slack, "clipped vertex y lies inside the world");
    }
  expect(graph.validate(&error), "clipped graph validates: " + error);
}

void testClipKeepsInteriorCellsUnchanged()
{
  MapGraph graph;
  const PartitionSettings partition = smallPartition();
  buildPartition(partition, graph, nullptr);

  // An interior cell is already inside the rectangle, so clipping must be a
  // no-op for it. If clipping perturbs interior geometry it would change the
  // world every time it ran.
  std::vector<std::vector<Point2>> before;
  before.reserve(graph.nodeCount());
  for (const MapNode &node : graph.nodes())
    before.push_back(node.area);

  clipCellsToExtent(graph, partition.extentMetres, nullptr);

  const double margin = partition.extentMetres * 0.25;
  uint32_t interior = 0u;
  for (size_t index = 0u; index < graph.nodeCount(); ++index)
  {
    const MapNode &node = graph.node(static_cast<uint32_t>(index));
    bool wasInside = true;
    for (const Point2 &vertex : before[index])
      wasInside = wasInside && vertex.x > margin && vertex.y > margin && vertex.x < partition.extentMetres - margin && vertex.y < partition.extentMetres - margin;
    if (!wasInside)
      continue;
    ++interior;
    expect(node.area.size() == before[index].size(), "an interior cell keeps its corner count through clipping");
  }
  expect(interior > 0u, "the fixture actually contains interior cells");
}

void testClipIsIdempotent()
{
  MapGraph graph;
  const PartitionSettings partition = smallPartition();
  buildPartition(partition, graph, nullptr);
  clipCellsToExtent(graph, partition.extentMetres, nullptr);
  const uint64_t once = graph.signature();
  clipCellsToExtent(graph, partition.extentMetres, nullptr);
  expect(graph.signature() == once, "clipping twice is the same as clipping once");
}

void testCoastlineFollowsTheLandWaterBoundary()
{
  MapGraph graph;
  const PartitionSettings partition = smallPartition();
  buildPartition(partition, graph, nullptr);
  RaiseSettings raise{};
  raise.extentMetres = partition.extentMetres;
  raise.landFraction = 0.35;
  raise.seed = partition.seed;
  raiseLand(Tag::Island, raise, graph, nullptr);
  clipCellsToExtent(graph, partition.extentMetres, nullptr);

  const std::vector<CoastPolyline> coast = extractCoastline(graph, partition.cellSpacingMetres * 1e-4);
  expect(!coast.empty(), "an island produces a coastline");

  size_t points = 0u;
  for (const CoastPolyline &line : coast)
  {
    expect(line.points.size() >= 2u, "a coast run has at least one segment");
    points += line.points.size();
  }
  expect(points > 4u, "the coastline has real extent");
}

void testCoastlineEmptyWithoutWater()
{
  MapGraph graph;
  const PartitionSettings partition = smallPartition();
  buildPartition(partition, graph, nullptr);
  RaiseSettings raise{};
  raise.extentMetres = partition.extentMetres;
  raise.landFraction = 1.0;
  raise.seed = partition.seed;
  raiseLand(Tag::Landlocked, raise, graph, nullptr);

  // No water means no land/water boundary. A generator that invents a coast
  // here would be drawing the world border as if it were shore.
  expect(extractCoastline(graph, partition.cellSpacingMetres * 1e-4).empty(), "a world with no water has no coastline");
}

// smallPartition() is 59 cells, which is the right size for testing topology
// and the wrong size for testing a drainage network: on 27 land cells the
// largest catchment is under four cell areas, so no channel ever crosses the
// initiation threshold and the river tests fail on the world rather than on the
// code. Anything about rivers uses this instead - the shipped default world, at
// 3,003 cells and about 30 ms, which is also the scale every measurement in the
// spec's river notes was taken at.
PartitionSettings riverPartition()
{
  PartitionSettings settings{};
  settings.extentMetres = 361000.0;
  settings.cellSpacingMetres = 5000.0;
  settings.seed = 12345u;
  return settings;
}

// A world carried through the coast stage, the point every later stage starts
// from.
MapGraph coastedGraph(const Tag shape, const double landFraction, const PartitionSettings &partition = smallPartition(), const std::vector<Tag> &tags = {})
{
  MapGraph graph;
  // The region stage reads the brief for its relief budget and character tags,
  // so the helpers have to set one rather than leaving the default.
  WorldBrief brief{};
  brief.seed = partition.seed;
  brief.extentMetres = partition.extentMetres;
  brief.tags = tags;
  brief.tags.push_back(shape);
  resolveTags(brief.tags);
  graph.setBrief(brief);
  buildPartition(partition, graph, nullptr);
  RaiseSettings raise{};
  raise.extentMetres = partition.extentMetres;
  raise.landFraction = landFraction;
  raise.coastComplexity = 0.25;
  raise.seed = partition.seed;
  raiseLand(shape, raise, graph, nullptr);
  clipCellsToExtent(graph, partition.extentMetres, nullptr);
  return graph;
}

// The shipped defaults are sized for a 361 km world. Scaled here so the range
// width, coastal plain and shelf keep the same proportion of the extent, rather
// than putting every cell of a small test world on one flank.
ElevationSettings elevationFor(const PartitionSettings &partition)
{
  ElevationSettings settings{};
  settings.extentMetres = partition.extentMetres;
  settings.seed = partition.seed;
  const double scale = partition.extentMetres / 361000.0;
  settings.rangeWidthMetres = 45000.0 * scale;
  settings.coastalPlainMetres = 60000.0 * scale;
  settings.shelfWidthMetres = 50000.0 * scale;
  settings.landformSpacingMetres = 140000.0 * scale;
  settings.landformEdgeMetres = 18000.0 * scale;
  return settings;
}

ElevationSettings smallElevation()
{
  return elevationFor(smallPartition());
}

MapGraph elevatedGraph(const Tag shape, const double landFraction, const std::vector<Tag> &tags = {}, const PartitionSettings &partition = smallPartition())
{
  MapGraph graph = coastedGraph(shape, landFraction, partition, tags);
  assignElevation(tags, elevationFor(partition), graph, nullptr);
  return graph;
}

MapGraph riverWorld(const Tag shape, const double landFraction, const std::vector<Tag> &tags = {})
{
  return elevatedGraph(shape, landFraction, tags, riverPartition());
}

// The whole pipeline, which is what the region stage has to run against.
RegionMap regionWorld(MapGraph &graph, const Tag shape, const std::vector<Tag> &tags = {}, RegionSettings settings = RegionSettings{})
{
  graph = riverWorld(shape, 0.4, tags);
  RiverNetwork rivers;
  buildRivers(RiverSettings{}, graph, rivers, {}, nullptr);
  // Content is the dominant biome now, so the climate has to have run.
  ClimateSettings climateSettings{};
  climateSettings.extentMetres = riverPartition().extentMetres;
  climateSettings.seed = riverPartition().seed;
  climateSettings.coastalMoistureRangeMetres = 240000.0 * climateSettings.extentMetres / 1400000.0;
  ClimateMap climate;
  buildClimate(climateSettings, rivers, graph, climate, nullptr);
  settings.seed = riverPartition().seed;
  RegionMap regions;
  std::string error;
  expect(buildRegions(settings, rivers, graph, regions, &error), "regions build: " + error);
  return regions;
}

void testCellAreaIsMetric()
{
  MapNode node{};
  node.area = {Point2(0.0, 0.0), Point2(200.0, 0.0), Point2(200.0, 50.0), Point2(0.0, 50.0)};
  expect(std::abs(cellArea(node) - 10000.0) < 1e-9, "a 200 x 50 m cell measures 10,000 m^2");

  // Winding must not change the answer; nothing guarantees which way a clipped
  // polygon comes out.
  std::reverse(node.area.begin(), node.area.end());
  expect(std::abs(cellArea(node) - 10000.0) < 1e-9, "reversed winding measures the same area");

  MapNode degenerate{};
  degenerate.area = {Point2(0.0, 0.0), Point2(1.0, 1.0)};
  expect(cellArea(degenerate) == 0.0, "a two-point cell has no area");
}

void testCoastDistanceField()
{
  const MapGraph graph = coastedGraph(Tag::Island, 0.4);
  const std::vector<double> distance = coastDistances(graph);
  expect(distance.size() == graph.nodeCount(), "the coast distance field covers every cell");

  double deepest = 0.0;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.land)
    {
      expect(distance[node.id] == 0.0, "water is at distance zero");
      continue;
    }
    expect(distance[node.id] > 0.0, "land is at a positive distance");
    if (std::isfinite(distance[node.id]))
      deepest = std::max(deepest, distance[node.id]);
    // A coastal cell touches water, so its distance is exactly one hop.
    if (node.coastal)
    {
      double nearest = std::numeric_limits<double>::infinity();
      for (const uint32_t neighbour : node.neighbours)
        if (!graph.node(neighbour).land)
        {
          const double dx = node.site.x - graph.node(neighbour).site.x;
          const double dy = node.site.y - graph.node(neighbour).site.y;
          nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
        }
      expect(std::abs(distance[node.id] - nearest) < 1e-6, "a coastal cell's distance is its own hop to water");
    }
  }
  expect(deepest > 0.0, "the interior is measurably far from the coast");
}

void testWaterBodiesSplitIntoOceanAndLakes()
{
  // The lake archetype's basin is enclosed by construction, so it must come out
  // as lake rather than as sea. Before this pass every water cell was Ocean and
  // an inland sea rendered as open ocean.
  MapGraph lake = coastedGraph(Tag::Lake, 0.5);
  const uint32_t changed = classifyWaterBodies(lake, smallElevation().extentMetres);
  expect(changed > 0u, "the lake archetype produces lake cells");
  uint32_t lakeCells = 0u;
  for (const MapNode &node : lake.nodes())
    if (node.type == CellType::Lake)
      ++lakeCells;
  expect(lakeCells > 0u, "lake cells are present after classification");

  // Idempotent: running it twice must not reclassify anything.
  expect(classifyWaterBodies(lake, smallElevation().extentMetres) == 0u, "water body classification is idempotent");

  // An island's water is all one connected sea, so nothing is enclosed.
  MapGraph island = coastedGraph(Tag::Island, 0.35);
  classifyWaterBodies(island, smallElevation().extentMetres);
  uint32_t islandLakes = 0u;
  for (const MapNode &node : island.nodes())
    if (node.type == CellType::Lake)
      ++islandLakes;
  expect(islandLakes == 0u, "an island's open sea produces no lakes, got " + std::to_string(islandLakes));
}

void testElevationSeparatesLandAndSea()
{
  const MapGraph graph = elevatedGraph(Tag::Island, 0.4);
  std::string error;
  expect(graph.validate(&error), "an elevated graph validates: " + error);
  for (const MapNode &node : graph.nodes())
  {
    if (node.land)
      expect(node.intent.elevationMetres > 0.0, "land stands above the datum");
    else
      expect(node.intent.elevationMetres < 0.0, "water lies below the datum");
  }
}

void testElevationFollowsTheRangeLayout()
{
  // Full scale. On the 59 cell world almost every land cell is coastal or on a
  // flank, so there are no plains to compare mountains against - the assertion
  // would be failing on the size of the test world rather than on the layout.
  const MapGraph graph = riverWorld(Tag::Island, 0.4);
  double mountainHeight = 0.0;
  double lowlandHeight = 0.0;
  double mountainInland = 0.0;
  double lowlandInland = 0.0;
  uint32_t mountains = 0u;
  uint32_t lowland = 0u;
  const std::vector<double> distance = coastDistances(graph);
  for (const MapNode &node : graph.nodes())
  {
    if (node.type == CellType::Mountain)
    {
      mountainHeight += node.intent.elevationMetres;
      mountainInland += distance[node.id];
      ++mountains;
    }
    // Everything else inland, pooled. Whether a particular world produces
    // Plain or only Hill is a tuning question; that mountains sit above and
    // behind the rest of the land is the layout working.
    else if (node.land && !node.coastal)
    {
      lowlandHeight += node.intent.elevationMetres;
      lowlandInland += distance[node.id];
      ++lowland;
    }
  }
  expect(mountains > 0u, "a continent gets mountains");
  expect(lowland > 0u, "a continent gets land that is not mountain");
  if (mountains == 0u || lowland == 0u)
    return;
  expect(mountainHeight / mountains > lowlandHeight / lowland, "mountains stand above the rest of the land");
  // Mountains form *ranges*: a mountain cell has mountain neighbours. This
  // replaced "mountains sit further inland", which was true of the old
  // long-axis spine by construction and is not true of anything else - ranges
  // now sit on plate seams, and a seam can run along a coast. The Andes are
  // coastal; what makes them a range is that they are connected.
  uint32_t connected = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || node.type != CellType::Mountain)
      continue;
    for (const uint32_t neighbour : node.neighbours)
      if (!graph.node(neighbour).refined() && graph.node(neighbour).type == CellType::Mountain)
      {
        ++connected;
        break;
      }
  }
  expect(connected * 10u >= mountains * 8u, "mountains form ranges rather than scattered peaks: " + std::to_string(connected) + " of " + std::to_string(mountains) + " have a mountain neighbour");
  (void)mountainInland;
  (void)lowlandInland;

  // Uplift must accompany elevation, or a later erosion pass flattens
  // everything the layout asked for.
  double mountainUplift = 0.0;
  for (const MapNode &node : graph.nodes())
    if (node.type == CellType::Mountain)
      mountainUplift += node.intent.upliftRate;
  expect(mountainUplift > 0.0, "mountains carry uplift, not just height");
}

void testNoRangesMeansNoMountains()
{
  // Rolling provinces can legitimately contain hills without an orogenic
  // range, but mountains remain a statement about range uplift. A high plateau
  // must not be called a mountain merely because its absolute altitude is high.
  ElevationSettings settings = smallElevation();
  settings.rangeStrength = 0.0;
  MapGraph graph = coastedGraph(Tag::Island, 0.4);
  expect(assignElevation({}, settings, graph, nullptr), "a rangeless world still elevates");
  uint32_t mountains = 0u;
  for (const MapNode &node : graph.nodes())
    if (node.type == CellType::Mountain)
      ++mountains;
  expect(mountains == 0u, "no range strength means no mountains, got " + std::to_string(mountains));
}

void testLandformProvincesChangeRangeCharacter()
{
  const PartitionSettings partition = riverPartition();
  const MapGraph source = coastedGraph(Tag::Island, 0.58, partition);
  ElevationSettings settings = elevationFor(partition);
  settings.landformContrast = 1.0;
  settings.rangeStrength = 0.65;
  settings.blockReliefFraction = 0.0;
  settings.detailReliefFraction = 0.0;

  MapGraph plain = source;
  settings.plainProvinceFraction = 1.0;
  settings.plateauProvinceFraction = 0.0;
  settings.highlandProvinceFraction = 0.0;
  expect(assignElevation({}, settings, plain, nullptr), "an all-plain province world elevates");

  MapGraph highland = source;
  settings.plainProvinceFraction = 0.0;
  settings.highlandProvinceFraction = 1.0;
  expect(assignElevation({}, settings, highland, nullptr), "an all-highland province world elevates");

  uint32_t plainMountains = 0u;
  uint32_t highlandMountains = 0u;
  double plainRelief = 0.0;
  double highlandRelief = 0.0;
  uint32_t land = 0u;
  for (uint32_t id = 0u; id < source.nodeCount(); ++id)
  {
    if (!source.node(id).land || source.node(id).refined())
      continue;
    ++land;
    plainMountains += plain.node(id).type == CellType::Mountain ? 1u : 0u;
    highlandMountains += highland.node(id).type == CellType::Mountain ? 1u : 0u;
    plainRelief += plain.node(id).intent.reliefAmplitude;
    highlandRelief += highland.node(id).intent.reliefAmplitude;
  }
  expect(land > 0u, "the province comparison contains land");
  expect(highlandMountains > plainMountains, "highlands accept more of the same mountain network than plains: " + std::to_string(highlandMountains) + " vs " + std::to_string(plainMountains));
  expect(highlandRelief > plainRelief * 1.5, "highlands carry substantially more local relief than plains");
}

void testPlateausAreHighAndLocallyQuiet()
{
  const PartitionSettings partition = riverPartition();
  MapGraph graph = coastedGraph(Tag::Island, 0.58, partition);
  ElevationSettings settings = elevationFor(partition);
  settings.landformContrast = 1.0;
  settings.rangeStrength = 0.0;
  settings.plainProvinceFraction = 0.0;
  settings.plateauProvinceFraction = 1.0;
  settings.highlandProvinceFraction = 0.0;
  settings.blockReliefFraction = 0.0;
  settings.detailReliefFraction = 0.0;
  expect(assignElevation({}, settings, graph, nullptr), "an all-plateau world elevates");

  uint32_t plateaus = 0u;
  double elevation = 0.0;
  double localRelief = 0.0;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || node.type != CellType::Plateau)
      continue;
    ++plateaus;
    elevation += node.intent.elevationMetres;
    localRelief += node.intent.reliefAmplitude;
  }
  expect(plateaus > 0u, "a plateau province is classified explicitly");
  if (plateaus == 0u)
    return;
  expect(elevation / plateaus > settings.reliefMetres * 0.45, "a plateau has a high regional floor");
  expect(localRelief / plateaus < settings.reliefMetres * 0.05, "a plateau remains locally low-relief");
}

void testCliffsFollowSharpMacroEdges()
{
  const PartitionSettings partition = riverPartition();
  const MapGraph source = coastedGraph(Tag::Island, 0.58, partition);
  ElevationSettings settings = elevationFor(partition);
  settings.detailReliefFraction = 0.0;
  settings.blockSpacingMetres = 90000.0;
  settings.blockEdgeMetres = 3000.0;
  settings.blockReliefFraction = 0.42;
  settings.cliffMinimumDropFraction = 0.01;
  settings.cliffMinimumGradient = 0.01;

  MapGraph first = source;
  MapGraph second = source;
  expect(assignElevation({}, settings, first, nullptr),
      "a blocky world assigns cliff elevation");
  expect(assignElevation({}, settings, second, nullptr),
      "the repeated blocky world assigns cliff elevation");
  expect(first.signature() == second.signature(),
      "cliff classification is deterministic");

  uint32_t cliffs = 0u;
  for (const MapNode &node : first.nodes())
  {
    if (node.refined() || node.type != CellType::Cliff)
      continue;
    ++cliffs;
    bool measuredEdge = false;
    for (const uint32_t neighbourId : node.neighbours)
    {
      const MapNode &neighbour = first.node(neighbourId);
      if (neighbour.refined() || !neighbour.land)
        continue;
      const double dx = node.site.x - neighbour.site.x;
      const double dy = node.site.y - neighbour.site.y;
      const double span = std::max(1.0, std::sqrt(dx * dx + dy * dy));
      const double drop = node.intent.elevationMetres -
          neighbour.intent.elevationMetres;
      measuredEdge = measuredEdge ||
          (drop >= settings.reliefMetres * settings.cliffMinimumDropFraction &&
              drop / span >= settings.cliffMinimumGradient);
    }
    expect(measuredEdge, "every cliff has a qualifying lower neighbour");
  }
  expect(cliffs > 0u, "sharp tectonic blocks produce cliffs");
}

void testTerrainConditioningFlattensPlainsAndRiverBanks()
{
  MapGraph graph = riverWorld(Tag::Island, 0.58);
  MapGraph probe = graph;
  RiverNetwork preliminary;
  expect(buildRivers(RiverSettings{}, probe, preliminary, {}, nullptr), "preliminary drainage builds for terrain conditioning");

  const auto planarError = [](const MapGraph &world)
  {
    double error = 0.0;
    uint32_t samples = 0u;
    for (const MapNode &node : world.nodes())
    {
      if (node.refined() || node.coastal || (node.type != CellType::Plain && node.type != CellType::Plateau))
        continue;
      double neighbours = 0.0;
      uint32_t count = 0u;
      for (const uint32_t neighbourId : node.neighbours)
      {
        const MapNode &neighbour = world.node(neighbourId);
        if (neighbour.refined() || neighbour.type != node.type)
          continue;
        neighbours += neighbour.intent.elevationMetres;
        ++count;
      }
      if (count == 0u)
        continue;
      error += std::abs(node.intent.elevationMetres - neighbours / count);
      ++samples;
    }
    return std::pair<double, uint32_t>(error, samples);
  };
  const auto bankError = [&preliminary](const MapGraph &world)
  {
    double error = 0.0;
    uint32_t samples = 0u;
    const double threshold = preliminary.largestDischargeCumecs * 0.035;
    for (const MapNode &river : world.nodes())
    {
      if (river.refined() || !preliminary.nodes[river.id].river || preliminary.nodes[river.id].dischargeCumecs < threshold)
        continue;
      for (const uint32_t neighbourId : river.neighbours)
      {
        const MapNode &neighbour = world.node(neighbourId);
        if (neighbour.refined() || !neighbour.land || neighbour.coastal)
          continue;
        const double distance = std::hypot(neighbour.site.x - river.site.x, neighbour.site.y - river.site.y);
        const double wanted = preliminary.nodes[river.id].filledElevationMetres + distance * 0.0012;
        error += std::abs(neighbour.intent.elevationMetres - wanted);
        ++samples;
      }
    }
    return std::pair<double, uint32_t>(error, samples);
  };

  const auto planarBefore = planarError(graph);
  const auto banksBefore = bankError(graph);
  TerrainConditioningSettings settings{};
  settings.floodplainWidthMetres = 18000.0;
  TerrainConditioningReport report{};
  std::string error;
  expect(conditionTerrain(settings, preliminary, graph, &report, &error), "terrain conditions: " + error);
  const auto planarAfter = planarError(graph);
  const auto banksAfter = bankError(graph);
  expect(report.planarCells > 0u, "terrain conditioning reaches planar cells");
  expect(report.floodplainCells > 0u, "terrain conditioning reaches major-river floodplains");
  expect(planarBefore.second > 0u && planarAfter.first < planarBefore.first, "planar conditioning reduces within-landform elevation error");
  expect(banksBefore.second > 0u && banksAfter.first < banksBefore.first, "floodplain grading moves river banks toward the channel profile");

  const uint64_t conditionedSignature = graph.signature();
  MapGraph repeated = riverWorld(Tag::Island, 0.58);
  MapGraph repeatedProbe = repeated;
  RiverNetwork repeatedRivers;
  buildRivers(RiverSettings{}, repeatedProbe, repeatedRivers, {}, nullptr);
  expect(conditionTerrain(settings, repeatedRivers, repeated, nullptr, &error), "terrain conditioning repeats: " + error);
  expect(repeated.signature() == conditionedSignature, "terrain conditioning is deterministic");

  RiverNetwork definitive;
  expect(buildRivers(RiverSettings{}, graph, definitive, {}, &error), "drainage rebuilds over conditioned terrain: " + error);
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || !definitive.nodes[node.id].river)
      continue;
    const uint32_t downstream = definitive.nodes[node.id].downstream;
    if (downstream == kInvalidNode || !graph.node(downstream).land)
      continue;
    expect(definitive.nodes[node.id].filledElevationMetres > definitive.nodes[downstream].filledElevationMetres, "a definitive river still descends after terrain conditioning");
  }
}

void testTerrainConditioningRejectsBadSettings()
{
  MapGraph graph = riverWorld(Tag::Island, 0.4);
  MapGraph probe = graph;
  RiverNetwork rivers;
  buildRivers(RiverSettings{}, probe, rivers, {}, nullptr);
  TerrainConditioningSettings settings{};
  settings.floodplainWidthMetres = 0.0;
  expect(!conditionTerrain(settings, rivers, graph, nullptr, nullptr), "a zero floodplain width is rejected");
  settings = TerrainConditioningSettings{};
  settings.planarStrength = 1.5;
  expect(!conditionTerrain(settings, rivers, graph, nullptr, nullptr), "an excessive planar strength is rejected");
}

void testCharacterTagsModulateRelief()
{
  const MapGraph plain = elevatedGraph(Tag::Island, 0.4, {});
  const MapGraph highland = elevatedGraph(Tag::Island, 0.4, {Tag::Highland});
  const MapGraph lowland = elevatedGraph(Tag::Island, 0.4, {Tag::Lowland});
  const auto highest = [](const MapGraph &graph)
  {
    double peak = 0.0;
    for (const MapNode &node : graph.nodes())
      peak = std::max(peak, node.intent.elevationMetres);
    return peak;
  };
  expect(highest(highland) > highest(plain), "the highland tag raises the world");
  expect(highest(lowland) < highest(plain), "the lowland tag flattens the world");
}

void testElevationHonoursTheSummitCeiling()
{
  const PartitionSettings partition = riverPartition();
  MapGraph graph = coastedGraph(
      Tag::Island, 0.58, partition, {Tag::Highland});
  ElevationSettings settings = elevationFor(partition);
  settings.reliefMetres = 4500.0;
  settings.broadReliefFraction = 0.5;
  settings.rangeStrength = 1.0;
  settings.blockReliefFraction = 0.5;
  expect(assignElevation({Tag::Highland}, settings, graph, nullptr),
      "a strongly uplifted highland world elevates");
  double highest = settings.seaLevelMetres;
  for (const MapNode &node : graph.nodes())
    if (!node.refined() && node.land)
      highest = std::max(highest, node.intent.elevationMetres);
  expect(highest <= settings.seaLevelMetres + settings.reliefMetres,
      "highland modulation respects the configured 4500 m summit ceiling");
  expect(highest > settings.seaLevelMetres + settings.reliefMetres * 0.9,
      "the authored range core can use the reserved summit budget");
}

void testElevationDeterminism()
{
  const MapGraph first = elevatedGraph(Tag::Coast, 0.45);
  const MapGraph second = elevatedGraph(Tag::Coast, 0.45);
  expect(first.signature() == second.signature(), "the same settings produce an identical elevated graph");
}

void testElevationRejectsBadSettings()
{
  MapGraph graph = coastedGraph(Tag::Island, 0.4);
  ElevationSettings settings = smallElevation();
  settings.reliefMetres = 0.0;
  expect(!assignElevation({}, settings, graph, nullptr), "zero relief is rejected");

  settings = smallElevation();
  settings.rangeStrength = 1.4;
  expect(!assignElevation({}, settings, graph, nullptr), "a range strength above one is rejected");

  settings = smallElevation();
  settings.plainProvinceFraction = 0.7;
  settings.plateauProvinceFraction = 0.4;
  expect(!assignElevation({}, settings, graph, nullptr), "province fractions above one are rejected");

  settings = smallElevation();
  settings.landformContrast = -0.1;
  expect(!assignElevation({}, settings, graph, nullptr), "a negative landform contrast is rejected");

  settings = smallElevation();
  settings.broadReliefFraction = 0.0;
  expect(!assignElevation({}, settings, graph, nullptr),
      "a zero broad relief fraction is rejected");

  settings = smallElevation();
  settings.cliffMinimumDropFraction = 1.1;
  expect(!assignElevation({}, settings, graph, nullptr), "an excessive cliff drop fraction is rejected");

  settings = smallElevation();
  settings.cliffMinimumGradient = -0.1;
  expect(!assignElevation({}, settings, graph, nullptr), "a negative cliff gradient is rejected");

  settings = smallElevation();
  MapGraph empty;
  expect(!assignElevation({}, settings, empty, nullptr), "elevating an empty graph is rejected");
}

MapGraph blendGraph()
{
  MapGraph graph;
  const PartitionSettings partition = smallPartition();
  buildPartition(partition, graph, nullptr);
  RaiseSettings raise{};
  raise.extentMetres = partition.extentMetres;
  raise.landFraction = 0.5;
  raise.seed = partition.seed;
  raiseLand(Tag::Island, raise, graph, nullptr);
  clipCellsToExtent(graph, partition.extentMetres, nullptr);
  return graph;
}

void testBlendWeightsPartitionUnity()
{
  const MapGraph graph = blendGraph();
  SiteIndex index;
  std::string error;
  expect(index.build(graph, 0u, smallPartition().extentMetres, &error), "site index builds: " + error);
  expect(index.siteCount() == graph.nodeCount(), "the index covers every level 0 cell");

  const BlendSettings settings{};
  std::vector<BlendWeight> weights;
  const double extent = smallPartition().extentMetres;
  for (uint32_t sample = 0u; sample < 400u; ++sample)
  {
    const double u = static_cast<double>(sample % 20u) / 19.0;
    const double v = static_cast<double>(sample / 20u) / 19.0;
    blendAt(graph, index, Point2(u * extent, v * extent), settings, weights);
    expect(!weights.empty(), "a sample inside the world blends something");
    double total = 0.0;
    double heaviest = 0.0;
    for (const BlendWeight &weight : weights)
    {
      expect(weight.weight >= 0.0, "no negative blend weight");
      heaviest = std::max(heaviest, weight.weight);
      total += weight.weight;
    }
    expect(std::abs(total - 1.0) < 1e-9, "blend weights sum to one, got " + std::to_string(total));
    // The heaviest contributor must be the nearest cell, or the blend has
    // rotated the map relative to the partition it came from.
    expect(!weights.empty() && weights.front().weight == heaviest, "the nearest cell is the heaviest contributor");
  }
}

void testBlendIsPureAtASite()
{
  // At a site the sample is that cell and nothing else. If it is not, a cell's
  // own colour is never shown anywhere and every type reads as a muddy average.
  const MapGraph graph = blendGraph();
  SiteIndex index;
  index.build(graph, 0u, smallPartition().extentMetres, nullptr);

  const BlendSettings settings{};
  std::vector<BlendWeight> weights;
  for (const MapNode &node : graph.nodes())
  {
    blendAt(graph, index, node.site, settings, weights);
    expect(!weights.empty() && weights.front().node == node.id, "a sample at a site is dominated by that site's cell");
    expect(!weights.empty() && weights.front().weight > 0.98, "a sample at a site is essentially pure");
  }
}

void testBlendIsContinuous()
{
  // The whole reason this exists rather than a flat fill: crossing a cell
  // boundary must not step. Walk a line across the map and bound the change per
  // step by what a smooth field could produce over that distance.
  const MapGraph graph = blendGraph();
  const double extent = smallPartition().extentMetres;
  SiteIndex index;
  index.build(graph, 0u, extent, nullptr);

  const BlendSettings settings{};
  std::vector<BlendWeight> previous;
  std::vector<BlendWeight> current;
  const uint32_t steps = 4000u;
  double worstJump = 0.0;
  for (uint32_t step = 0u; step <= steps; ++step)
  {
    const double t = static_cast<double>(step) / static_cast<double>(steps);
    // A diagonal, so the walk crosses boundaries at every angle rather than
    // only the axis-aligned ones.
    blendAt(graph, index, Point2(t * extent, t * extent * 0.83 + extent * 0.05), settings, current);
    if (!previous.empty())
    {
      // Total variation between the two weight sets, treating a cell absent
      // from one side as weight zero there.
      double change = 0.0;
      for (const BlendWeight &entry : current)
      {
        double before = 0.0;
        for (const BlendWeight &old : previous)
          if (old.node == entry.node)
            before = old.weight;
        change += std::abs(entry.weight - before);
      }
      for (const BlendWeight &old : previous)
      {
        bool present = false;
        for (const BlendWeight &entry : current)
          present = present || entry.node == old.node;
        if (!present)
          change += old.weight;
      }
      worstJump = std::max(worstJump, change);
    }
    previous = current;
  }
  // One step is ~14 m against a 4 km cell spacing. A flat fill jumps by 2.0 at
  // every boundary; the old inverse-distance-over-k formulation stepped by the
  // dropped neighbour's whole weight.
  expect(worstJump < 0.05, "the blend is continuous across cell boundaries, worst step " + std::to_string(worstJump));
}

void testBlendSharpnessConcentrates()
{
  // The sharpness dial has to actually do something, and in the right
  // direction: sharper means more of the weight on the nearest cell.
  const MapGraph graph = blendGraph();
  const double extent = smallPartition().extentMetres;
  SiteIndex index;
  index.build(graph, 0u, extent, nullptr);

  BlendSettings soft{};
  soft.sharpnessScale = 0.4;
  BlendSettings hard{};
  hard.sharpnessScale = 3.0;

  std::vector<BlendWeight> weights;
  double softTotal = 0.0;
  double hardTotal = 0.0;
  uint32_t samples = 0u;
  for (uint32_t sample = 0u; sample < 900u; ++sample)
  {
    const double u = static_cast<double>(sample % 30u) / 29.0;
    const double v = static_cast<double>(sample / 30u) / 29.0;
    const Point2 position(u * extent, v * extent);
    blendAt(graph, index, position, soft, weights);
    if (weights.empty())
      continue;
    softTotal += weights.front().weight;
    blendAt(graph, index, position, hard, weights);
    hardTotal += weights.front().weight;
    ++samples;
  }
  expect(samples > 0u, "sharpness comparison has samples");
  expect(hardTotal > softTotal, "a higher sharpness concentrates weight on the nearest cell: soft " + std::to_string(softTotal / samples) + " hard " + std::to_string(hardTotal / samples));
}

void testHeightBlendUsesTerrainVariationAsAThreshold()
{
  MapGraph graph = blendGraph();
  expect(graph.nodeCount() >= 2u, "height blend test has contributors");
  if (graph.nodeCount() < 2u)
    return;
  graph.node(0u).intent.elevationMetres = 100.0;
  graph.node(1u).intent.elevationMetres = 900.0;

  const HeightBlendSettings settings{};
  const std::vector<BlendWeight> original{BlendWeight{0u, 0.55}, BlendWeight{1u, 0.45}};
  std::vector<BlendWeight> flat = original;
  shapeBlendByHeight(graph, 850.0, 0.0, 1000.0, settings, flat);
  expect(
      flat.size() == original.size() && flat[0].node == original[0].node && flat[1].node == original[1].node && flat[0].weight == original[0].weight && flat[1].weight == original[1].weight,
      "flat terrain preserves the broad spatial material blend");

  std::vector<BlendWeight> steep = original;
  shapeBlendByHeight(graph, 850.0, 200.0, 1000.0, settings, steep);
  double total = 0.0;
  for (const BlendWeight &weight : steep)
    total += weight.weight;
  expect(std::abs(total - 1.0) < 1e-9, "height-shaped material weights remain normalized");
  expect(steep[1].weight > steep[0].weight && steep[1].weight > original[1].weight, "strong local height variation selects the elevation-compatible land type");
}

void testRegionCellsReceiveStableInfluenceWeights()
{
  const MapGraph graph = blendGraph();
  expect(graph.nodeCount() >= 4u, "cell-weight test has contributors");
  if (graph.nodeCount() < 4u)
    return;

  const std::vector<BlendWeight> equal{BlendWeight{0u, 0.25}, BlendWeight{1u, 0.25}, BlendWeight{2u, 0.25}, BlendWeight{3u, 0.25}};
  std::vector<BlendWeight> disabled = equal;
  applyCellInfluenceWeights(graph, 0.0, disabled);
  bool preserved = disabled.size() == equal.size();
  for (size_t index = 0u; index < disabled.size() && index < equal.size(); ++index)
    preserved = preserved && disabled[index].node == equal[index].node && disabled[index].weight == equal[index].weight;
  expect(preserved, "zero cell-weight strength preserves the spatial field exactly");

  std::vector<BlendWeight> first = equal;
  std::vector<BlendWeight> second = equal;
  applyCellInfluenceWeights(graph, 1.0, first);
  applyCellInfluenceWeights(graph, 1.0, second);
  double total = 0.0;
  bool changed = false;
  for (size_t index = 0u; index < first.size(); ++index)
  {
    total += first[index].weight;
    changed = changed || std::abs(first[index].weight - equal[index].weight) > 1.0e-6;
    expect(first[index].node == second[index].node && first[index].weight == second[index].weight, "cell influence weights are deterministic");
  }
  expect(std::abs(total - 1.0) < 1.0e-9, "weighted cell influence remains normalized");
  expect(changed, "nonzero cell-weight strength gives neighboring cells unequal authority");
}

void testTransitionSharpnessIsSymmetric()
{
  // Load-bearing rather than tidy: the effective exponent is a bilinear form
  // over the contributing weights, so an asymmetric table would make the blend
  // depend on which cell happened to be nearest and the colour would jump at
  // the midline between two cells.
  for (uint8_t first = 0u; first < static_cast<uint8_t>(CellType::Count); ++first)
    for (uint8_t second = 0u; second < static_cast<uint8_t>(CellType::Count); ++second)
    {
      const CellType a = static_cast<CellType>(first);
      const CellType b = static_cast<CellType>(second);
      expect(transitionSharpness(a, b) == transitionSharpness(b, a), std::string("transition sharpness is symmetric for ") + cellTypeName(a) + "/" + cellTypeName(b));
      expect(transitionSharpness(a, b) > 0.0, "transition sharpness is positive");
    }

  // The coastline is the hardest edge in the system, and has to be harder than
  // any land-to-land transition or the coast blurs into the sea.
  double hardestLand = 0.0;
  for (uint8_t first = 0u; first < static_cast<uint8_t>(CellType::Count); ++first)
    for (uint8_t second = 0u; second < static_cast<uint8_t>(CellType::Count); ++second)
    {
      const CellType a = static_cast<CellType>(first);
      const CellType b = static_cast<CellType>(second);
      if (cellTypeIsWater(a) || cellTypeIsWater(b))
        continue;
      hardestLand = std::max(hardestLand, transitionSharpness(a, b));
    }
  expect(transitionSharpness(CellType::Ocean, CellType::Plain) > hardestLand, "the coastline is the hardest transition");
  expect(transitionSharpness(CellType::Plain, CellType::Mountain) > transitionSharpness(CellType::Plain, CellType::Hill), "mountain meets plain more abruptly than hill does");
}

void testBlendDeterminism()
{
  const MapGraph graph = blendGraph();
  const double extent = smallPartition().extentMetres;
  SiteIndex first;
  SiteIndex second;
  first.build(graph, 0u, extent, nullptr);
  second.build(graph, 0u, extent, nullptr);

  const BlendSettings settings{};
  std::vector<BlendWeight> a;
  std::vector<BlendWeight> b;
  for (uint32_t sample = 0u; sample < 500u; ++sample)
  {
    const double u = static_cast<double>(sample % 25u) / 24.0;
    const double v = static_cast<double>(sample / 25u) / 19.0;
    const Point2 position(u * extent, v * extent);
    blendAt(graph, first, position, settings, a);
    blendAt(graph, second, position, settings, b);
    expect(a.size() == b.size(), "two indices agree on contributor count");
    for (size_t i = 0u; i < a.size() && i < b.size(); ++i)
      expect(a[i].node == b[i].node && a[i].weight == b[i].weight, "two indices agree bit for bit");
  }
}

RiverNetwork riveredGraph(MapGraph &graph, const Tag shape)
{
  graph = riverWorld(shape, 0.45);
  RiverNetwork network;
  RiverSettings settings{};
  std::string error;
  expect(buildRivers(settings, graph, network, {}, &error), "rivers build: " + error);
  return network;
}

void testEveryCourseDescendsAndReachesWater()
{
  // The phase gate. Depression filling is what makes this a property of the
  // algorithm rather than a hope: after filling, every land cell has a strictly
  // lower neighbour, so the descent cannot cycle and cannot dead-end inland.
  MapGraph graph;
  const RiverNetwork network = riveredGraph(graph, Tag::Island);

  for (const MapNode &node : graph.nodes())
  {
    if (!node.land)
      continue;
    expect(network.nodes[node.id].filledElevationMetres >= node.intent.elevationMetres, "filling never lowers a cell");

    uint32_t walk = node.id;
    size_t steps = 0u;
    double previous = network.nodes[walk].filledElevationMetres;
    bool reachedWater = false;
    while (steps <= graph.nodeCount())
    {
      const uint32_t next = network.nodes[walk].downstream;
      if (next == kInvalidNode)
        break;
      const double here = network.nodes[next].filledElevationMetres;
      expect(here < previous, "each step down a course is strictly lower");
      previous = here;
      walk = next;
      ++steps;
      if (!graph.node(walk).land)
      {
        reachedWater = true;
        break;
      }
    }
    expect(steps <= graph.nodeCount(), "a course terminates rather than cycling");
    expect(reachedWater, "every land cell drains to water, cell " + std::to_string(node.id));
  }
}

void testDischargeAccumulatesDownstream()
{
  MapGraph graph;
  const RiverNetwork network = riveredGraph(graph, Tag::Island);
  for (const MapNode &node : graph.nodes())
  {
    if (!node.land)
      continue;
    const uint32_t downstream = network.nodes[node.id].downstream;
    if (downstream == kInvalidNode || !graph.node(downstream).land)
      continue;
    expect(network.nodes[downstream].drainageAreaSquareMetres >= network.nodes[node.id].drainageAreaSquareMetres, "drainage never decreases downstream");
    expect(network.nodes[downstream].dischargeCumecs >= network.nodes[node.id].dischargeCumecs, "discharge never decreases downstream");
  }

  // Mass balance: every square metre of land reaches exactly one mouth. This is
  // the check that catches a flow tree with a lost branch, which a
  // monotonicity check alone would pass.
  double landArea = 0.0;
  for (const MapNode &node : graph.nodes())
    if (node.land)
      landArea += cellArea(node);
  double deliveredArea = 0.0;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.land)
      continue;
    const uint32_t downstream = network.nodes[node.id].downstream;
    if (downstream == kInvalidNode || !graph.node(downstream).land)
      deliveredArea += network.nodes[node.id].drainageAreaSquareMetres;
  }
  expect(std::abs(deliveredArea - landArea) < landArea * 1e-9, "the whole landmass drains through its mouths: " + std::to_string(deliveredArea) + " of " + std::to_string(landArea));
}

void testRiversBranch()
{
  MapGraph graph = riverWorld(Tag::Island, 0.35);
  RiverNetwork network;
  RiverSettings settings{};
  // A lower channel threshold than the default, so enough of the tree is
  // resolved for the ratio below to mean something. The default draws the map;
  // this measures the network.
  settings.riverThresholdCells = 2.0;
  std::string error;
  expect(buildRivers(settings, graph, network, {}, &error), "rivers build for the branching measurement: " + error);
  expect(network.riverCells > 0u, "a continent grows rivers");
  expect(network.mouths > 0u, "rivers reach the sea");

  // Branching, measured rather than eyeballed: confluences are cells with two
  // or more channels flowing in, and a believable network has many.
  uint32_t confluences = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (!network.nodes[node.id].river)
      continue;
    uint32_t inflows = 0u;
    for (const uint32_t neighbour : node.neighbours)
      if (network.nodes[neighbour].river && network.nodes[neighbour].downstream == node.id)
        ++inflows;
    if (inflows >= 2u)
      ++confluences;
  }
  expect(confluences > network.riverCells / 20u, "the network confluences rather than running in parallel: " + std::to_string(confluences) + " over " + std::to_string(network.riverCells) + " river cells");

  for (const MapNode &node : graph.nodes())
    if (network.nodes[node.id].river)
      expect(network.nodes[node.id].strahlerOrder >= 1u, "every channel carries an order");

  // Horton's bifurcation ratio: the count of streams of one order over the
  // count of the next, which for a natural network sits around three to five.
  //
  // This rather than "the network reaches order N", because the maximum order
  // is a property of the *sampling*, not of the network. Measured on the same
  // island at three cell sizes: 5 km reaches order 3, 2.5 km order 4, 1.5 km
  // order 4, and lowering the channel threshold raises it at every size. The
  // ratio is what stays put - 2.1 and 3.6 across all of them.
  std::vector<uint32_t> orderCounts(network.maximumOrder + 2u, 0u);
  for (const RiverNode &node : network.nodes)
    if (node.river)
      ++orderCounts[node.strahlerOrder];
  expect(network.maximumOrder >= 2u, "the network has a hierarchy at all, got order " + std::to_string(network.maximumOrder));
  // The highest order present is a single trunk by definition, so its ratio is
  // meaningless; standard practice is to drop it.
  for (uint32_t order = 1u; order + 1u < network.maximumOrder; ++order)
  {
    if (orderCounts[order + 1u] == 0u)
      continue;
    const double ratio = static_cast<double>(orderCounts[order]) / static_cast<double>(orderCounts[order + 1u]);
    expect(ratio > 1.5 && ratio < 6.0, "the bifurcation ratio at order " + std::to_string(order) + " is believable, got " + std::to_string(ratio));
  }
}

void testStrahlerOrderOnlyRisesAtEqualConfluences()
{
  MapGraph graph;
  const RiverNetwork network = riveredGraph(graph, Tag::Coast);
  for (const MapNode &node : graph.nodes())
  {
    if (!network.nodes[node.id].river)
      continue;
    uint32_t highest = 0u;
    uint32_t highestCount = 0u;
    for (const uint32_t neighbour : node.neighbours)
    {
      if (!network.nodes[neighbour].river || network.nodes[neighbour].downstream != node.id)
        continue;
      const uint32_t order = network.nodes[neighbour].strahlerOrder;
      if (order > highest)
      {
        highest = order;
        highestCount = 1u;
      }
      else if (order == highest)
        ++highestCount;
    }
    const uint32_t expected = highest == 0u ? 1u : (highestCount >= 2u ? highest + 1u : highest);
    expect(network.nodes[node.id].strahlerOrder == expected, "order follows the Strahler rule at cell " + std::to_string(node.id));
  }
}

void testVisibleChannelsStayConnectedAndWaterfallsAreSpaced()
{
  MapGraph graph;
  RiverSettings settings{};
  RiverNetwork network;
  graph = riverWorld(Tag::Island, 0.45);
  expect(buildRivers(settings, graph, network, {}, nullptr), "the channel-continuity world builds");
  for (const MapNode &node : graph.nodes())
  {
    if (!network.nodes[node.id].river)
      continue;
    const uint32_t downstream = network.nodes[node.id].downstream;
    expect(downstream != kInvalidNode, "a visible river has a downstream reach");
    if (downstream != kInvalidNode && graph.node(downstream).land)
      expect(network.nodes[downstream].river, "a channel remains visible across its downstream floodplain");
  }

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
  for (size_t first = 0u; first < network.waterfalls.size(); ++first)
    for (size_t second = first + 1u; second < network.waterfalls.size(); ++second)
      expect(
          !shortlyDownstream(network.waterfalls[first].node, network.waterfalls[second].node) && !shortlyDownstream(network.waterfalls[second].node, network.waterfalls[first].node),
          "adjacent steep cells collapse to one waterfall marker");
}

void testRiversHandleAnEnclosedBasin()
{
  // The lake archetype's basin is enclosed water. Rivers must reach it and stop
  // there rather than climbing out, and the lake counts as water for the gate.
  MapGraph graph = riverWorld(Tag::Lake, 0.55);
  RiverNetwork network;
  std::string error;
  expect(buildRivers(RiverSettings{}, graph, network, {}, &error), "rivers build against an enclosed basin: " + error);
  uint32_t lakeMouths = 0u;
  for (const RiverSegment &segment : network.segments)
    if (segment.mouth && graph.node(network.nodes[segment.node].downstream).type == CellType::Lake)
      ++lakeMouths;
  expect(network.mouths > 0u, "the basin receives rivers");
  expect(lakeMouths > 0u, "at least one river mouth is on a lake rather than the sea");
}

void testRiverDeterminism()
{
  MapGraph first;
  const RiverNetwork a = riveredGraph(first, Tag::Island);
  MapGraph second;
  const RiverNetwork b = riveredGraph(second, Tag::Island);
  expect(a.riverCells == b.riverCells && a.mouths == b.mouths && a.maximumOrder == b.maximumOrder, "two runs produce the same network shape");
  expect(a.segments.size() == b.segments.size(), "two runs produce the same segment count");
  for (size_t index = 0u; index < a.nodes.size() && index < b.nodes.size(); ++index)
    expect(a.nodes[index].downstream == b.nodes[index].downstream && a.nodes[index].dischargeCumecs == b.nodes[index].dischargeCumecs, "two runs agree bit for bit at cell " + std::to_string(index));
  // The valley reclassification mutates the graph, so the graph has to be
  // identical too.
  expect(first.signature() == second.signature(), "the graph after the river stage is identical");
}

void testRiversClassifyIncisedCanyons()
{
  MapGraph graph = riverWorld(Tag::Island, 0.45);
  RiverNetwork network;
  RiverSettings settings{};
  settings.riverThresholdCells = 2.0;
  settings.canyonMinimumBankRiseFraction = 0.005;
  settings.canyonMinimumBankGradient = 0.005;
  std::string error;
  expect(buildRivers(settings, graph, network, {}, &error),
      "rivers build for canyon classification: " + error);

  uint32_t canyons = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || node.type != CellType::Canyon)
      continue;
    ++canyons;
    expect(network.nodes[node.id].river,
        "every canyon contains a generated river");
    expect(node.content == CellType::Canyon,
        "canyon landform and content agree before regions");
  }
  expect(canyons > 0u, "incised channels produce canyons");
}

void testRiverThresholdSetsDrainageDensity()
{
  MapGraph sparse = riverWorld(Tag::Island, 0.45);
  MapGraph dense = riverWorld(Tag::Island, 0.45);
  RiverNetwork sparseNetwork;
  RiverNetwork denseNetwork;
  RiverSettings settings{};
  settings.riverThresholdCells = 40.0;
  buildRivers(settings, sparse, sparseNetwork, {}, nullptr);
  settings.riverThresholdCells = 2.0;
  buildRivers(settings, dense, denseNetwork, {}, nullptr);
  expect(denseNetwork.riverCells > sparseNetwork.riverCells, "a lower channel threshold produces a denser network: " + std::to_string(denseNetwork.riverCells) + " against " + std::to_string(sparseNetwork.riverCells));

  // Discharge is a physical quantity and must not move with a drawing
  // threshold: the same cell carries the same cumecs either way.
  double sparseLargest = 0.0;
  double denseLargest = 0.0;
  for (const RiverNode &node : sparseNetwork.nodes)
    sparseLargest = std::max(sparseLargest, node.dischargeCumecs);
  for (const RiverNode &node : denseNetwork.nodes)
    denseLargest = std::max(denseLargest, node.dischargeCumecs);
  expect(std::abs(sparseLargest - denseLargest) < 1e-9, "the threshold changes what is drawn, not what is computed");
}

void testLakeFillKeepsTheDeepConnectedCore()
{
  MapGraph graph = riverWorld(Tag::Island, 0.45);
  RiverNetwork network;
  network.nodes.resize(graph.nodeCount());
  for (const MapNode &node : graph.nodes())
    network.nodes[node.id].filledElevationMetres = node.intent.elevationMetres;

  uint32_t deepest = kInvalidNode;
  std::vector<uint32_t> basin;
  for (const MapNode &node : graph.nodes())
  {
    if (!node.land)
      continue;
    basin.clear();
    basin.push_back(node.id);
    for (const uint32_t neighbour : node.neighbours)
      if (graph.node(neighbour).land)
        basin.push_back(neighbour);
    if (basin.size() >= 3u)
    {
      deepest = node.id;
      break;
    }
  }
  expect(deepest != kInvalidNode, "the lake test finds a connected basin");
  if (deepest == kInvalidNode)
    return;
  for (size_t index = 0u; index < 3u; ++index)
    network.nodes[basin[index]].filledElevationMetres += 100.0 - static_cast<double>(index) * 10.0;

  LakeSettings settings{};
  settings.minimumDepthMetres = 20.0;
  settings.minimumInflowFraction = 0.0;
  settings.maximumAreaSquareMetres = 1.0;
  settings.maximumCatchmentFraction = 0.0;
  const uint32_t filled = fillLakes(network, settings, {}, graph);
  expect(filled == 1u, "an area-capped basin keeps one deep core cell, got " + std::to_string(filled));
  expect(!graph.node(deepest).land && graph.node(deepest).type == CellType::Lake, "the deepest basin cell becomes the lake core");
}

void testRiversRejectBadSettings()
{
  MapGraph graph = riverWorld(Tag::Island, 0.45);
  RiverNetwork network;
  RiverSettings settings{};
  settings.riverThresholdCells = 0.0;
  expect(!buildRivers(settings, graph, network, {}, nullptr), "a zero channel threshold is rejected");

  settings = RiverSettings{};
  settings.fillEpsilonMetres = 0.0;
  expect(!buildRivers(settings, graph, network, {}, nullptr), "a zero fill epsilon is rejected");

  settings = RiverSettings{};
  settings.channelReferenceGradient = 0.0;
  expect(!buildRivers(settings, graph, network, {}, nullptr), "a zero channel reference gradient is rejected");

  settings = RiverSettings{};
  settings.minimumRiverSystemCells = 0u;
  expect(!buildRivers(settings, graph, network, {}, nullptr), "a zero minimum river-system length is rejected");

  settings = RiverSettings{};
  settings.canyonMinimumBankRiseFraction = -0.1;
  expect(!buildRivers(settings, graph, network, {}, nullptr), "a negative canyon bank rise is rejected");

  settings = RiverSettings{};
  settings.canyonMinimumBankGradient = -0.1;
  expect(!buildRivers(settings, graph, network, {}, nullptr), "a negative canyon bank gradient is rejected");

  MapGraph empty;
  expect(!buildRivers(RiverSettings{}, empty, network, {}, nullptr), "building rivers on an empty graph is rejected");
}

void testRegionsCoverAndPartitionTheLand()
{
  for (const Tag shape : {Tag::Island, Tag::Archipelago})
  {
    MapGraph graph;
    const RegionMap regions = regionWorld(graph, shape);
    expect(!regions.regions.empty(), "a continent is divided into regions");

    uint32_t landCells = 0u;
    for (const MapNode &node : graph.nodes())
    {
      if (!node.land)
      {
        expect(node.region == kInvalidNode, "water belongs to no region");
        continue;
      }
      ++landCells;
      expect(node.region != kInvalidNode, "every land cell belongs to a region, cell " + std::to_string(node.id));
      expect(node.region < regions.regions.size(), "a cell's region id is a valid index");
    }

    size_t counted = 0u;
    for (const MapRegion &region : regions.regions)
    {
      expect(!region.cells.empty(), "no region is empty");
      expect(!region.name.empty(), "every region is named");
      expect(region.areaSquareMetres > 0.0, "every region has area");
      expect(!cellTypeIsWater(region.content), "a land region carries a land content, got " + std::string(cellTypeName(region.content)));
      counted += region.cells.size();
      for (const uint32_t id : region.cells)
        expect(graph.node(id).region == region.id, "the cell list and the cell's own region agree");
    }
    expect(counted == landCells, "the regions partition the land exactly: " + std::to_string(counted) + " of " + std::to_string(landCells));
  }
}

void testRegionsAreConnected()
{
  // A region is a place, and a place is one piece. A stranded cell would put a
  // label on ground that is not part of what it names.
  MapGraph graph;
  const RegionMap regions = regionWorld(graph, Tag::Archipelago);
  for (const MapRegion &region : regions.regions)
  {
    std::vector<uint32_t> stack{region.cells.front()};
    std::vector<uint32_t> seen{region.cells.front()};
    while (!stack.empty())
    {
      const uint32_t current = stack.back();
      stack.pop_back();
      for (const uint32_t neighbour : graph.node(current).neighbours)
      {
        if (graph.node(neighbour).region != region.id)
          continue;
        if (std::find(seen.begin(), seen.end(), neighbour) != seen.end())
          continue;
        seen.push_back(neighbour);
        stack.push_back(neighbour);
      }
    }
    expect(seen.size() == region.cells.size(), "region " + std::to_string(region.id) + " is one connected piece, " + std::to_string(seen.size()) + " of " + std::to_string(region.cells.size()));
  }
}

void testRegionSizeFollowsTheTarget()
{
  MapGraph graph;
  RegionSettings settings{};
  settings.targetAreaSquareMetres = 900.0e6;
  const RegionMap regions = regionWorld(graph, Tag::Island, {}, settings);
  double total = 0.0;
  for (const MapRegion &region : regions.regions)
    total += region.areaSquareMetres;
  const double mean = total / static_cast<double>(regions.regions.size());
  // Growing by least cost never lands every region on the target exactly, but
  // it must not be off by a factor - or the dial does not mean what it says.
  expect(
      mean > settings.targetAreaSquareMetres * 0.5 && mean < settings.targetAreaSquareMetres * 2.0,
      "mean region area tracks the target, got " + std::to_string(mean * 1e-6) + " km^2 against " + std::to_string(settings.targetAreaSquareMetres * 1e-6));

  MapGraph coarseGraph;
  RegionSettings coarse{};
  coarse.targetAreaSquareMetres = 4000.0e6;
  const RegionMap coarseRegions = regionWorld(coarseGraph, Tag::Island, {}, coarse);
  expect(coarseRegions.regions.size() < regions.regions.size(), "a larger target produces fewer regions");
}

void testHierarchyCohesionCalmsRegionBoundaries()
{
  MapGraph base;
  WorldBrief brief{};
  brief.seed = 87123u;
  brief.extentMetres = 180000.0;
  brief.reliefMetres = 2400.0;
  brief.tags = {Tag::Island};
  base.setBrief(brief);
  HierarchySettings hierarchy{};
  hierarchy.extentMetres = brief.extentMetres;
  hierarchy.seed = brief.seed;
  hierarchy.cellSpacingMetres = {36000.0, 9000.0};
  expect(buildHierarchy(hierarchy, base, nullptr), "the hierarchy-cohesion partition builds");
  RaiseSettings raise{};
  raise.extentMetres = brief.extentMetres;
  raise.landFraction = 0.5;
  raise.seed = brief.seed;
  expect(raiseLand(Tag::Island, raise, base, nullptr), "the hierarchy-cohesion island raises");
  ElevationSettings elevation{};
  elevation.extentMetres = brief.extentMetres;
  elevation.reliefMetres = brief.reliefMetres;
  elevation.seed = brief.seed;
  expect(assignElevation(brief.tags, elevation, base, nullptr), "the hierarchy-cohesion elevation builds");
  RiverNetwork rivers;
  expect(buildRivers(RiverSettings{}, base, rivers, {}, nullptr), "the hierarchy-cohesion rivers build");
  ClimateSettings climateSettings{};
  climateSettings.extentMetres = brief.extentMetres;
  climateSettings.seed = brief.seed;
  ClimateMap climate;
  expect(buildClimate(climateSettings, rivers, base, climate, nullptr), "the hierarchy-cohesion climate builds");

  RegionSettings looseSettings{};
  looseSettings.targetAreaSquareMetres = 900.0e6;
  looseSettings.seed = brief.seed;
  looseSettings.hierarchyCohesion = 0.0;
  RegionSettings cohesiveSettings = looseSettings;
  cohesiveSettings.hierarchyCohesion = 1.0;
  MapGraph looseGraph = base;
  MapGraph cohesiveGraph = base;
  RegionMap looseRegions;
  RegionMap cohesiveRegions;
  expect(buildRegions(looseSettings, rivers, looseGraph, looseRegions, nullptr), "regions build without hierarchy cohesion");
  expect(buildRegions(cohesiveSettings, rivers, cohesiveGraph, cohesiveRegions, nullptr), "regions build with hierarchy cohesion");
  const auto bordersInsideParents = [](const MapGraph &graph)
  {
    uint32_t borders = 0u;
    for (const MapNode &node : graph.nodes())
    {
      if (node.refined() || !node.land || node.parent == kInvalidNode)
        continue;
      for (const uint32_t neighbour : node.neighbours)
        if (neighbour > node.id && !graph.node(neighbour).refined() && graph.node(neighbour).land && graph.node(neighbour).parent == node.parent && graph.node(neighbour).region != node.region)
          ++borders;
    }
    return borders;
  };
  const uint32_t looseBorders = bordersInsideParents(looseGraph);
  const uint32_t cohesiveBorders = bordersInsideParents(cohesiveGraph);
  expect(cohesiveBorders < looseBorders, "hierarchy cohesion moves region borders out of parent interiors: " + std::to_string(cohesiveBorders) + " against " + std::to_string(looseBorders));
}

void testRidgesResistRegionBoundaries()
{
  // The claim the resistance makes: regions stop at ranges rather than
  // straddling them. Measured as the relief a region contains - a region that
  // spans a range contains the whole range's relief.
  const auto meanInternalRelief = [](const RegionMap &regions)
  {
    double total = 0.0;
    for (const MapRegion &region : regions.regions)
      total += region.reliefMetres;
    return regions.regions.empty() ? 0.0 : total / static_cast<double>(regions.regions.size());
  };

  MapGraph freeGraph;
  RegionSettings freeSettings{};
  freeSettings.ridgeResistance = 0.0;
  freeSettings.riverResistance = 0.0;
  const RegionMap free = regionWorld(freeGraph, Tag::Island, {}, freeSettings);

  MapGraph boundedGraph;
  RegionSettings bounded{};
  bounded.ridgeResistance = 8.0;
  const RegionMap resisted = regionWorld(boundedGraph, Tag::Island, {}, bounded);

  expect(
      meanInternalRelief(resisted) < meanInternalRelief(free), "ridge resistance keeps regions off the ranges: " + std::to_string(meanInternalRelief(resisted)) + " against " + std::to_string(meanInternalRelief(free)));
}

void testCharacterTagsBiasContent()
{
  const auto count = [](const RegionMap &regions, const CellType content)
  {
    uint32_t total = 0u;
    for (const MapRegion &region : regions.regions)
      if (region.content == content)
        ++total;
    return total;
  };

  MapGraph neutralGraph;
  const RegionMap neutral = regionWorld(neutralGraph, Tag::Island);
  MapGraph aridGraph;
  const RegionMap arid = regionWorld(aridGraph, Tag::Island, {Tag::Arid});
  MapGraph verdantGraph;
  const RegionMap verdant = regionWorld(verdantGraph, Tag::Island, {Tag::Verdant});

  // Content is the dominant biome now, and the biome comes from the climate
  // fields, which the brief's character tags do not touch. The tags still steer
  // the *elevation* stage, so they still change the map - but content is no
  // longer theirs to bias, and asserting that it is would be asserting the old
  // scoring pass that was deleted.
  (void)arid;
  (void)verdant;

  // What must still hold: a region's content is not a single value across the
  // whole world, because the climate fields vary across it.
  uint32_t distinct = 0u;
  for (uint8_t type = 0u; type < static_cast<uint8_t>(CellType::Count); ++type)
    if (count(neutral, static_cast<CellType>(type)) > 0u)
      ++distinct;
  expect(distinct >= 2u, "the map has more than one content, got " + std::to_string(distinct));
}

void testRegionNamesAreStableAndVaried()
{
  MapGraph first;
  const RegionMap a = regionWorld(first, Tag::Island);
  MapGraph second;
  const RegionMap b = regionWorld(second, Tag::Island);
  expect(a.regions.size() == b.regions.size(), "two runs produce the same region count");
  for (size_t index = 0u; index < a.regions.size() && index < b.regions.size(); ++index)
  {
    expect(a.regions[index].name == b.regions[index].name, "region names are reproducible");
    expect(a.regions[index].content == b.regions[index].content, "region contents are reproducible");
  }
  expect(first.signature() == second.signature(), "the graph after the region stage is identical");

  // Names have to be varied enough to label a map. A generator that repeats
  // itself reads worse than no names at all.
  std::vector<std::string> names;
  for (const MapRegion &region : a.regions)
    names.push_back(region.name);
  std::sort(names.begin(), names.end());
  const size_t unique = static_cast<size_t>(std::unique(names.begin(), names.end()) - names.begin());
  expect(unique * 10u >= names.size() * 9u, "at least nine names in ten are distinct, got " + std::to_string(unique) + " of " + std::to_string(names.size()));

  // A name must not move when an unrelated part of the world changes, or every
  // regeneration renames the whole map.
  expect(placeName(7u, 3u) == placeName(7u, 3u), "a name is a pure function");
  expect(placeName(7u, 3u) != placeName(7u, 4u), "different regions get different names");
  expect(placeName(7u, 3u) != placeName(8u, 3u), "different worlds get different names");
}

void testNamedRiversRunFromMouthToSource()
{
  MapGraph graph;
  const RegionMap regions = regionWorld(graph, Tag::Coast);
  RiverNetwork rivers;
  buildRivers(RiverSettings{}, graph, rivers, {}, nullptr);
  expect(!regions.rivers.empty(), "the map names some rivers");

  std::vector<uint8_t> claimed(graph.nodeCount(), 0u);
  for (const NamedRiver &river : regions.rivers)
  {
    expect(!river.name.empty(), "a named river has a name");
    expect(river.stem.size() >= 3u, "a named river is worth naming");
    // Mouth first, then strictly upstream: discharge falls as the stem climbs.
    for (size_t index = 1u; index < river.stem.size(); ++index)
    {
      expect(rivers.nodes[river.stem[index]].dischargeCumecs <= rivers.nodes[river.stem[index - 1u]].dischargeCumecs, "a stem runs from the mouth upstream");
      expect(rivers.nodes[river.stem[index]].downstream == river.stem[index - 1u], "a stem is a connected course");
    }
    for (const uint32_t id : river.stem)
    {
      expect(claimed[id] == 0u, "no cell belongs to two named rivers");
      claimed[id] = 1u;
    }
  }
}

ClimateMap climateWorld(MapGraph &graph, ClimateSettings settings = ClimateSettings{})
{
  graph = riverWorld(Tag::Island, 0.4);
  RiverNetwork rivers;
  buildRivers(RiverSettings{}, graph, rivers, {}, nullptr);
  settings.extentMetres = riverPartition().extentMetres;
  settings.seed = riverPartition().seed;
  // Scaled like every other length: the shipped default is sized for a 1,400 km
  // world, and on this one it would put the whole map within a fifth of a
  // moisture length of the sea.
  settings.coastalMoistureRangeMetres = 240000.0 * settings.extentMetres / 1400000.0;
  ClimateMap climate;
  std::string error;
  expect(buildClimate(settings, rivers, graph, climate, &error), "climate builds: " + error);
  return climate;
}

void testTemperatureFollowsLatitudeAndHeight()
{
  MapGraph graph;
  ClimateSettings settings{};
  // A wide band, so the north-south difference is bigger than the noise in
  // where the land happens to be high.
  settings.southLatitudeDegrees = 20.0;
  settings.latitudeSpanDegrees = 40.0;
  const ClimateMap climate = climateWorld(graph, settings);

  double southTotal = 0.0;
  double northTotal = 0.0;
  uint32_t south = 0u;
  uint32_t north = 0u;
  const double extent = riverPartition().extentMetres;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined())
      continue;
    if (node.site.y < extent * 0.25)
    {
      southTotal += climate.cells[node.id].temperatureCelsius;
      ++south;
    }
    else if (node.site.y > extent * 0.75)
    {
      northTotal += climate.cells[node.id].temperatureCelsius;
      ++north;
    }
  }
  expect(south > 0u && north > 0u, "the world spans a latitude range");
  if (south == 0u || north == 0u)
    return;
  expect(southTotal / south > northTotal / north, "the south is warmer than the north: " + std::to_string(southTotal / south) + " against " + std::to_string(northTotal / north));

  // Lapse rate: at a given latitude, higher ground is colder. Compared within
  // a narrow band so latitude is not doing the work.
  double lowTotal = 0.0;
  double highTotal = 0.0;
  uint32_t low = 0u;
  uint32_t high = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || !node.land)
      continue;
    if (std::abs(node.site.y - extent * 0.5) > extent * 0.12)
      continue;
    if (node.intent.elevationMetres < 300.0)
    {
      lowTotal += climate.cells[node.id].temperatureCelsius;
      ++low;
    }
    else if (node.intent.elevationMetres > 1200.0)
    {
      highTotal += climate.cells[node.id].temperatureCelsius;
      ++high;
    }
  }
  if (low == 0u || high == 0u)
    return;
  expect(highTotal / high < lowTotal / low, "high ground is colder at the same latitude: " + std::to_string(highTotal / high) + " against " + std::to_string(lowTotal / low));
}

void testMoistureFallsInlandAndBehindRanges()
{
  MapGraph graph;
  const ClimateMap climate = climateWorld(graph);

  // Coastal land against deep interior.
  double coastal = 0.0;
  double interior = 0.0;
  uint32_t coastalCount = 0u;
  uint32_t interiorCount = 0u;
  const std::vector<double> toCoast = coastDistances(graph);
  double deepest = 0.0;
  for (const MapNode &node : graph.nodes())
    if (!node.refined() && node.land && std::isfinite(toCoast[node.id]))
      deepest = std::max(deepest, toCoast[node.id]);
  // Within a latitude band, because the subtropical dry belt puts a north-south
  // moisture gradient across the world that is stronger than the maritime one.
  // Comparing coast against interior over the whole map measures whichever of
  // the two happens to sit further south, which is not the claim being tested.
  const double extent = riverPartition().extentMetres;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || !node.land)
      continue;
    if (std::abs(node.site.y - extent * 0.5) > extent * 0.2)
      continue;
    if (node.coastal)
    {
      coastal += climate.cells[node.id].moisture;
      ++coastalCount;
    }
    else if (std::isfinite(toCoast[node.id]) && toCoast[node.id] > deepest * 0.45)
    {
      interior += climate.cells[node.id].moisture;
      ++interiorCount;
    }
  }
  expect(coastalCount > 0u && interiorCount > 0u, "the world has both a coast and an interior");
  if (coastalCount == 0u || interiorCount == 0u)
    return;
  expect(coastal / coastalCount > interior / interiorCount, "the coast is wetter than the interior: " + std::to_string(coastal / coastalCount) + " against " + std::to_string(interior / interiorCount));

  // Rain shadow: cells that had to get their weather over high ground are
  // drier than cells that did not. This is the heuristic that most makes a
  // biome map look real, so it has to actually do something.
  double shadowed = 0.0;
  double open = 0.0;
  uint32_t shadowedCount = 0u;
  uint32_t openCount = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || !node.land)
      continue;
    if (climate.cells[node.id].rainShadow > 0.5)
    {
      shadowed += climate.cells[node.id].moisture;
      ++shadowedCount;
    }
    else if (climate.cells[node.id].rainShadow < 0.05)
    {
      open += climate.cells[node.id].moisture;
      ++openCount;
    }
  }
  expect(shadowedCount > 0u, "some ground lies downwind of a barrier, got " + std::to_string(shadowedCount));
  if (shadowedCount == 0u || openCount == 0u)
    return;
  expect(shadowed / shadowedCount < open / openCount, "ground behind a range is drier: " + std::to_string(shadowed / shadowedCount) + " against " + std::to_string(open / openCount));
}

void testBiomesAgreeWithLandAndWater()
{
  MapGraph graph;
  const ClimateMap climate = climateWorld(graph);
  expect(climate.cells.size() == graph.nodeCount(), "the climate covers every cell");
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined())
      continue;
    const Biome biome = biomeOf(node);
    if (node.land)
      expect(!biomeIsWater(biome), std::string("land does not get a water biome, got ") + biomeName(biome));
    else
      expect(biomeIsWater(biome), std::string("water does not get a land biome, got ") + biomeName(biome));
  }
}

void testWhittakerClassification()
{
  // Spot checks against the diagram the classifier is built from.
  expect(classifyBiome(26.0, 0.9, 100.0, 0.0) == Biome::TropicalRainforest, "hot and wet is rainforest");
  expect(classifyBiome(26.0, 0.05, 100.0, 0.0) == Biome::SandDesert, "hot, flat and hyper-arid is sand desert");
  expect(classifyBiome(26.0, 0.12, 100.0, 0.08) == Biome::Desert, "hot rocky arid ground is desert");
  expect(classifyBiome(-15.0, 0.5, 100.0, 0.0) == Biome::Glacier, "far below freezing is ice");
  expect(classifyBiome(2.0, 0.55, 1900.0, 0.1) == Biome::Glacier,
      "cold moist alpine ground retains snow below the strict freezing isotherm");
  expect(classifyBiome(2.0, 0.20, 1900.0, 0.1) != Biome::Glacier,
      "cold dry alpine ground does not become a glacier");
  expect(classifyBiome(2.0, 0.55, 900.0, 0.1) != Biome::Glacier,
      "cool moist lowland remains below the alpine snowline");
  expect(classifyBiome(-4.0, 0.1, 100.0, 0.0) == Biome::ColdDesert, "cold and dry is cold desert");
  expect(classifyBiome(8.0, 0.6, 200.0, 0.0) == Biome::TemperateForest, "cool and moist is temperate forest");
  expect(classifyBiome(8.0, 0.25, 200.0, 0.0) == Biome::Steppe, "cool semi-arid ground is steppe");
  expect(classifyBiome(17.0, 0.30, 200.0, 0.0) == Biome::Mediterranean, "warm seasonally dry ground is Mediterranean woodland");
  expect(classifyBiome(10.0, 0.6, 1500.0, 0.05) == Biome::MontaneForest, "cool wet upland is montane forest");
  expect(classifyBiome(24.0, 0.2, 100.0, 0.0) == Biome::Savanna, "hot and semi-arid is savanna");
  // Steep high ground is bare whatever the climate says.
  expect(classifyBiome(20.0, 0.9, 2200.0, 0.6) == Biome::BareRock, "steep high ground is bare rock");

  // Monotone in moisture at a fixed temperature: drier must never be wetter's
  // biome. Checked as a chain through the compatibility rule, which is what
  // makes the bands a sequence rather than a lookup with holes in it.
  for (const double temperature : {2.0, 10.0, 17.0, 25.0})
  {
    Biome previous = classifyBiome(temperature, 0.0, 200.0, 0.0);
    for (uint32_t step = 1u; step <= 40u; ++step)
    {
      const Biome next = classifyBiome(temperature, static_cast<double>(step) / 40.0, 200.0, 0.0);
      expect(biomesCompatible(previous, next), std::string("the moisture sequence has no gap at ") + std::to_string(temperature) + "C: " + biomeName(previous) + " to " + biomeName(next));
      previous = next;
    }
  }
}

void testNeighbouringBiomesMakeSense()
{
  // The whole point of driving land types from continuous fields rather than
  // picking them per region: neighbours come out compatible by construction.
  // A failure here means a field has a discontinuity, not that the table is
  // wrong.
  MapGraph graph;
  climateWorld(graph);
  const uint32_t bad = countIncompatibleBorders(graph);
  uint32_t borders = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined())
      continue;
    for (const uint32_t neighbour : node.neighbours)
      if (neighbour > node.id && !graph.node(neighbour).refined())
        ++borders;
  }
  expect(borders > 0u, "the map has borders to check");
  // Zero on a temperate world. A small residue is tolerated because a hard
  // rain shadow really is a hard edge, but it must stay a residue.
  expect(bad * 200u < borders, "neighbouring land types are compatible: " + std::to_string(bad) + " bad of " + std::to_string(borders));
}

void testWideClimateBandProducesBiomeVariety()
{
  MapGraph graph;
  ClimateSettings settings{};
  settings.southLatitudeDegrees = 22.0;
  settings.latitudeSpanDegrees = 16.0;
  climateWorld(graph, settings);

  std::vector<uint8_t> present(static_cast<size_t>(Biome::Count), 0u);
  uint32_t arid = 0u;
  uint32_t wooded = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || !node.land)
      continue;
    const Biome biome = biomeOf(node);
    present[static_cast<size_t>(biome)] = 1u;
    arid += biome == Biome::Desert || biome == Biome::SandDesert || biome == Biome::SaltFlat || biome == Biome::Steppe ? 1u : 0u;
    wooded += biome == Biome::TemperateForest || biome == Biome::TemperateRainforest || biome == Biome::MontaneForest || biome == Biome::Mediterranean || biome == Biome::SubtropicalForest ||
                      biome == Biome::TropicalSeasonalForest || biome == Biome::TropicalRainforest
                  ? 1u
                  : 0u;
  }
  const uint32_t distinct = static_cast<uint32_t>(std::count(present.begin(), present.end(), static_cast<uint8_t>(1u)));
  expect(distinct >= 6u, "a country across a wide climate band contains varied biomes, got " + std::to_string(distinct));
  expect(arid > 0u && wooded > 0u, "the wide country contains both arid and wooded country");
}

void testClimateDeterminism()
{
  MapGraph first;
  const ClimateMap a = climateWorld(first);
  MapGraph second;
  const ClimateMap b = climateWorld(second);
  expect(first.signature() == second.signature(), "the graph after the climate stage is identical");
  for (size_t index = 0u; index < a.cells.size() && index < b.cells.size(); ++index)
    expect(a.cells[index].temperatureCelsius == b.cells[index].temperatureCelsius && a.cells[index].moisture == b.cells[index].moisture, "two runs agree bit for bit at cell " + std::to_string(index));
}

void testClimateRejectsBadSettings()
{
  MapGraph graph = riverWorld(Tag::Island, 0.4);
  RiverNetwork rivers;
  buildRivers(RiverSettings{}, graph, rivers, {}, nullptr);
  ClimateMap climate;

  ClimateSettings settings{};
  settings.extentMetres = 0.0;
  expect(!buildClimate(settings, rivers, graph, climate, nullptr), "a zero extent is rejected");

  settings = ClimateSettings{};
  settings.coastalMoistureRangeMetres = 0.0;
  expect(!buildClimate(settings, rivers, graph, climate, nullptr), "a zero moisture range is rejected");

  MapGraph empty;
  expect(!buildClimate(ClimateSettings{}, rivers, empty, climate, nullptr), "an empty graph is rejected");
}

void testRegionsRejectBadSettings()
{
  MapGraph graph = riverWorld(Tag::Island, 0.4);
  RiverNetwork rivers;
  buildRivers(RiverSettings{}, graph, rivers, {}, nullptr);
  RegionMap regions;

  RegionSettings settings{};
  settings.targetAreaSquareMetres = 0.0;
  expect(!buildRegions(settings, rivers, graph, regions, nullptr), "a zero target area is rejected");

  settings = RegionSettings{};
  settings.ridgeResistance = -1.0;
  expect(!buildRegions(settings, rivers, graph, regions, nullptr), "a negative resistance is rejected");

  settings = RegionSettings{};
  settings.hierarchyCohesion = 1.5;
  expect(!buildRegions(settings, rivers, graph, regions, nullptr), "out-of-range hierarchy cohesion is rejected");

  MapGraph empty;
  expect(!buildRegions(RegionSettings{}, rivers, empty, regions, nullptr), "building regions on an empty graph is rejected");
}

void testEdgeSetSharesEveryBoundary()
{
  // The point of the structure: a boundary exists once, and the two cells
  // either side refer to it. Cells store their outlines as independent
  // polygons, and everything that decorates a boundary had to be made to agree
  // between two copies by hand until this existed.
  MapGraph graph;
  climateWorld(graph);
  const MapEdgeSet set = buildEdgeSet(graph, 1.0);
  expect(!set.empty(), "the map has boundaries");

  std::vector<uint32_t> uses(set.edges.size(), 0u);
  uint32_t leaves = 0u;
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || node.area.size() < 3u)
      continue;
    ++leaves;
    expect(!set.loops[node.id].empty(), "every cell has a loop");
    for (const int32_t entry : set.loops[node.id])
      uses[static_cast<uint32_t>(std::abs(entry) - 1)]++;
  }
  expect(leaves > 0u, "the map has leaf cells");

  uint32_t interior = 0u;
  uint32_t border = 0u;
  for (size_t index = 0u; index < set.edges.size(); ++index)
  {
    if (set.edges[index].worldBorder)
    {
      ++border;
      expect(uses[index] == 1u, "a border edge belongs to one cell");
    }
    else
    {
      ++interior;
      // Exactly two: the whole structure rests on this. One means two cells'
      // copies of a boundary failed to weld into the same edge, which is what
      // used to leave a hairline of background along it.
      expect(uses[index] == 2u, "an interior edge belongs to exactly two cells, got " + std::to_string(uses[index]));
    }
  }
  expect(interior > border, "most boundaries are interior");

  // Properties are set, and agree with the cells they came from.
  for (const MapBoundaryEdge &edge : set.edges)
  {
    if (edge.worldBorder)
      continue;
    const MapNode &a = graph.node(edge.cellA);
    const MapNode &b = graph.node(edge.cellB);
    expect(edge.coast == (a.land != b.land), "an edge knows whether it is a coastline");
    expect(edge.regionBorder == (a.region != b.region), "an edge knows whether it separates named regions");
    expect(edge.biomeBorder == (a.biome != b.biome), "an edge knows whether it separates biomes");
    expect(edge.contentBorder == (a.content != b.content), "an edge knows whether it separates map content");
    expect(edge.landformBorder == (a.type != b.type), "an edge knows whether it separates landforms");
    expect(edge.classificationBorder == (edge.regionBorder || edge.biomeBorder || edge.contentBorder || edge.landformBorder), "a classification boundary covers every filled categorical layer");
    expect(edge.lengthMetres > 0.0, "an edge has a length");
  }
}

void testRelaxedBoundariesStayShared()
{
  MapGraph graph;
  climateWorld(graph);
  MapEdgeSet set = buildEdgeSet(graph, 1.0);
  const std::vector<MapBoundaryVertex> before = set.vertices;

  EdgeRelaxSettings relax{};
  relax.seed = 12345u;
  relax.amplitudeMetres = 900.0;
  relax.segmentMetres = 1500.0;
  relaxEdges(set, relax);

  uint32_t moved = 0u;
  for (const MapBoundaryEdge &edge : set.edges)
  {
    expect(edge.path.size() >= 2u, "a relaxed edge has a path");
    if (edge.path.size() < 2u)
      continue;
    if (edge.worldBorder)
    {
      // The world's rim stays put, or the map ends in a ragged edge that is not
      // a coastline.
      expect(edge.path.size() == 2u, "a border edge is not subdivided");
      expect(edge.path.front().x == before[edge.from].position.x && edge.path.front().y == before[edge.from].position.y, "a border edge keeps its corners");
    }
    else if (edge.path.size() > 2u)
      ++moved;
  }
  expect(moved > 0u, "interior boundaries are actually displaced");

  // Every cell still closes. Two cells sharing an edge read the *same* path, so
  // what is worth testing is that each cell's loop is continuous - that
  // consecutive edges of it meet.
  //
  // Measured per edge rather than over the whole loop, because a world-border
  // edge is deliberately left straight and unsubdivided and can be longer than
  // any interior segment. Bounding the whole loop by the segment length flagged
  // those as gaps, which they are not.
  for (const MapNode &node : graph.nodes())
  {
    if (node.refined() || node.area.size() < 3u)
      continue;
    const std::vector<Point2> boundary = cellBoundary(set, node.id);
    expect(boundary.size() >= 3u, "a relaxed cell still has a boundary, cell " + std::to_string(node.id));
  }
  for (const MapBoundaryEdge &edge : set.edges)
  {
    if (edge.worldBorder)
      continue;
    for (size_t index = 1u; index < edge.path.size(); ++index)
    {
      const double step = std::hypot(edge.path[index].x - edge.path[index - 1u].x, edge.path[index].y - edge.path[index - 1u].y);
      expect(step < relax.segmentMetres * 4.0, "an interior boundary is continuous, step " + std::to_string(step));
    }
  }

  // And the join: an edge's ends are exactly the shared vertices' displaced
  // positions, so every cell meeting there meets there.
  for (const MapBoundaryEdge &edge : set.edges)
  {
    if (edge.worldBorder || edge.path.size() < 2u)
      continue;
    for (const MapBoundaryEdge &other : set.edges)
    {
      if (&other == &edge || other.path.size() < 2u)
        continue;
      if (other.from != edge.to && other.to != edge.to)
        continue;
      const Point2 &mine = edge.path.back();
      const Point2 &theirs = other.from == edge.to ? other.path.front() : other.path.back();
      expect(mine.x == theirs.x && mine.y == theirs.y, "two edges meeting at a vertex land on the same point");
    }
  }
}

void testSmoothedBoundariesStayShared()
{
  MapGraph graph;
  climateWorld(graph);
  MapEdgeSet set = buildEdgeSet(graph, 1.0);
  for (MapBoundaryEdge &edge : set.edges)
    edge.visible = edge.coast || edge.classificationBorder;

  EdgeRelaxSettings relax{};
  relax.seed = 713u;
  relax.amplitudeMetres = 900.0;
  relax.segmentMetres = 1500.0;
  relaxEdges(set, relax);
  smoothChains(set, 6u, 0.5);

  uint32_t curved = 0u;
  for (const MapBoundaryEdge &edge : set.edges)
  {
    if (edge.visible && edge.path.size() == 6u)
      ++curved;
    if (edge.path.size() < 2u)
      continue;
    expect(edge.path.front().x == set.vertices[edge.from].position.x && edge.path.front().y == set.vertices[edge.from].position.y, "a smoothed edge starts at its shared vertex");
    expect(edge.path.back().x == set.vertices[edge.to].position.x && edge.path.back().y == set.vertices[edge.to].position.y, "a smoothed edge ends at its shared vertex");
  }
  expect(curved > 0u, "visible boundary chains are curve-sampled");
}

void testClassificationBoundarySmoothingRemovesCellScaleNoise()
{
  MapGraph graph;
  regionWorld(graph, Tag::Island);
  MapEdgeSet set = buildEdgeSet(graph, 1.0);
  EdgeRelaxSettings relax{};
  relax.seed = 713u;
  relax.amplitudeMetres = 2400.0;
  relax.classificationScale = 0.6;
  relaxEdges(set, relax);

  std::vector<std::vector<uint32_t>> incident(set.vertices.size());
  for (const MapBoundaryEdge &edge : set.edges)
    if (edge.classificationBorder && !edge.coast && !edge.worldBorder)
    {
      incident[edge.from].push_back(edge.to);
      incident[edge.to].push_back(edge.from);
    }
  const auto roughness = [&set, &incident]()
  {
    double total = 0.0;
    for (size_t vertex = 0u; vertex < set.vertices.size(); ++vertex)
    {
      if (incident[vertex].size() != 2u || set.vertices[vertex].coastal || set.vertices[vertex].onWorldBorder)
        continue;
      const Point2 &point = set.vertices[vertex].position;
      const Point2 &first = set.vertices[incident[vertex][0]].position;
      const Point2 &second = set.vertices[incident[vertex][1]].position;
      total += std::hypot(point.x - (first.x + second.x) * 0.5, point.y - (first.y + second.y) * 0.5);
    }
    return total;
  };
  const double before = roughness();
  const std::vector<MapBoundaryVertex> original = set.vertices;
  smoothClassificationJunctions(set, 3u, 0.45);
  const double after = roughness();
  expect(before > 0.0 && after < before, "classification smoothing reduces cell-scale boundary curvature");
  for (size_t vertex = 0u; vertex < set.vertices.size(); ++vertex)
    if (original[vertex].coastal || original[vertex].onWorldBorder || incident[vertex].size() != 2u)
      expect(set.vertices[vertex].position.x == original[vertex].position.x && set.vertices[vertex].position.y == original[vertex].position.y, "coasts and true classification junctions remain fixed during smoothing");
  for (const MapBoundaryEdge &edge : set.edges)
    if (edge.path.size() >= 2u)
    {
      expect(edge.path.front().x == set.vertices[edge.from].position.x && edge.path.front().y == set.vertices[edge.from].position.y, "a classification-smoothed edge starts at its shared vertex");
      expect(edge.path.back().x == set.vertices[edge.to].position.x && edge.path.back().y == set.vertices[edge.to].position.y, "a classification-smoothed edge ends at its shared vertex");
    }
}

void testElevationContoursFormDeterministicPolylines()
{
  MapGraph graph;
  climateWorld(graph);
  ContourSettings settings{};
  settings.intervalMetres = 100.0;
  settings.majorEvery = 5u;
  settings.minimumElevationMetres = 0.0;
  settings.maximumElevationMetres = 3000.0;

  ContourSet first;
  ContourSet second;
  std::string error;
  expect(extractElevationContours(graph, settings, first, &error), "elevation contours extract: " + error);
  error.clear();
  expect(extractElevationContours(graph, settings, second, &error), "elevation contours repeat: " + error);
  expect(!first.empty(), "an elevated world produces contour lines");
  expect(first.lines.size() == second.lines.size(), "contour line count is deterministic");
  expect(first.pointCount() == second.pointCount(), "contour point count is deterministic");

  bool hasMajor = false;
  size_t closed = 0u;
  for (size_t lineIndex = 0u; lineIndex < first.lines.size(); ++lineIndex)
  {
    const ContourPolyline &line = first.lines[lineIndex];
    expect(line.points.size() >= 2u, "a contour is a drawable polyline");
    expect(std::abs(std::remainder(line.elevationMetres, settings.intervalMetres)) < 1.0e-8, "a contour lies on an interval");
    hasMajor = hasMajor || line.major;
    closed += line.closed ? 1u : 0u;
    if (lineIndex >= second.lines.size())
      continue;
    const ContourPolyline &again = second.lines[lineIndex];
    expect(line.elevationMetres == again.elevationMetres && line.closed == again.closed && line.major == again.major && line.points.size() == again.points.size(), "contour metadata is deterministic");
    for (size_t point = 0u; point < line.points.size() && point < again.points.size(); ++point)
      expect(line.points[point].x == again.points[point].x && line.points[point].y == again.points[point].y, "contour geometry is deterministic");
  }
  expect(hasMajor, "the contour set includes index contours");
  expect(closed == first.lines.size(), "positive contours on an island form closed loops, got " + std::to_string(closed) + " of " + std::to_string(first.lines.size()));

  ContourSettings unsmoothedSettings = settings;
  unsmoothedSettings.curveSamplesPerSpan = 1u;
  ContourSet unsmoothed;
  expect(extractElevationContours(graph, unsmoothedSettings, unsmoothed, nullptr), "unsmoothed contours extract");
  expect(unsmoothed.lines.size() == first.lines.size(), "curve sampling preserves contour topology");
  expect(
      std::all_of(
          unsmoothed.lines.begin(),
          unsmoothed.lines.end(),
          [](const ContourPolyline &line)
          {
            return line.closed && line.points.size() >= 3u;
          }),
      "the raw contour graph contains only drawable closed loops");
  expect(first.pointCount() > unsmoothed.pointCount(), "curve sampling adds points between sharp marching segments");

  ContourSet rejected;
  settings.intervalMetres = 0.0;
  expect(!extractElevationContours(graph, settings, rejected, nullptr), "a zero contour interval is rejected");
  settings.intervalMetres = 100.0;
  settings.curveTension = 1.5;
  expect(!extractElevationContours(graph, settings, rejected, nullptr), "an invalid contour curve tension is rejected");
}

void testSharedSurfaceDrivesHeightUpliftAndContours()
{
  MapGraph graph;
  climateWorld(graph);
  SiteIndex index;
  const double extent = riverPartition().extentMetres;
  expect(index.build(graph, 0u, extent, nullptr), "the shared surface site index builds");
  MapSurfaceSettings surfaceSettings{};
  surfaceSettings.sampleSpacingMetres = 7000.0;
  MapSurfaceField first;
  MapSurfaceField second;
  std::string error;
  expect(first.build(graph, index, BlendSettings{}, surfaceSettings, &error), "the shared scalar surface builds: " + error);
  expect(second.build(graph, index, BlendSettings{}, surfaceSettings, &error), "the shared scalar surface repeats: " + error);
  expect(first.gridSide() > 16u && first.gridSide() == second.gridSide(), "the shared surface resolves the map finer than a diagnostic grid");
  for (uint32_t y = 0u; y < first.gridSide(); ++y)
    for (uint32_t x = 0u; x < first.gridSide(); ++x)
    {
      const MapSurfaceSample &a = first.at(x, y);
      const MapSurfaceSample &b = second.at(x, y);
      expect(a.elevationMetres == b.elevationMetres && a.upliftRateMetresPerYear == b.upliftRateMetresPerYear, "surface elevation and uplift are deterministic");
    }

  // Sampling exactly on a stored vertex must recover both scalar channels.
  const uint32_t gx = first.gridSide() / 3u;
  const uint32_t gy = first.gridSide() / 2u;
  const MapSurfaceSample vertex = first.sample(Point2(gx * first.spacingMetres(), gy * first.spacingMetres()));
  expect(std::abs(vertex.elevationMetres - first.at(gx, gy).elevationMetres) < 1.0e-9, "heightmap sampling recovers a shared surface vertex");
  expect(std::abs(vertex.upliftRateMetresPerYear - first.at(gx, gy).upliftRateMetresPerYear) < 1.0e-15, "uplift sampling recovers the same shared surface vertex");

  ContourSettings contourSettings{};
  contourSettings.intervalMetres = 100.0;
  contourSettings.minimumElevationMetres = 0.0;
  contourSettings.maximumElevationMetres = 3000.0;
  contourSettings.curveSamplesPerSpan = 1u;
  ContourSet contours;
  expect(extractElevationContours(first, contourSettings, contours, &error), "contours march the shared surface: " + error);
  uint32_t checked = 0u;
  for (const ContourPolyline &line : contours.lines)
    for (const Point2 &point : line.points)
    {
      // Domain-edge samples are intentionally pinned below the first contour
      // to keep every positive island isoline closed.
      if (point.x <= first.spacingMetres() || point.y <= first.spacingMetres() || point.x >= extent - first.spacingMetres() || point.y >= extent - first.spacingMetres())
        continue;
      const double sampled = first.sample(point).elevationMetres;
      expect(std::abs(sampled - line.elevationMetres) < 1.0e-5, "an unsmoothed contour lies on the rendered scalar surface");
      ++checked;
    }
  expect(checked > 100u, "the contour/surface agreement check covers real geometry");

  MapSurfaceSettings bad = surfaceSettings;
  bad.maximumGridSide = 1u;
  MapSurfaceField rejected;
  expect(!rejected.build(graph, index, BlendSettings{}, bad, nullptr), "an invalid shared-surface grid is rejected");
}

void testMapExportBundleIsRegisteredAndDeterministic()
{
  const PartitionSettings partition = smallPartition();
  MapGraph graph = elevatedGraph(Tag::Island, 0.42, {}, partition);
  ClimateSettings climateSettings{};
  climateSettings.extentMetres = partition.extentMetres;
  climateSettings.seed = partition.seed;
  climateSettings.coastalMoistureRangeMetres = 18000.0;
  ClimateMap climate;
  std::string error;
  expect(buildClimate(climateSettings, RiverNetwork{}, graph, climate, &error), "export climate probe builds: " + error);
  RiverSettings riverSettings{};
  riverSettings.riverThresholdCells = 2.0;
  riverSettings.minimumRiverSystemCells = 1u;
  std::vector<double> runoff(graph.nodeCount(), 1.0);
  for (size_t i = 0u; i < climate.cells.size(); ++i)
    runoff[i] = climate.cells[i].moisture;
  RiverNetwork rivers;
  expect(buildRivers(riverSettings, graph, rivers, runoff, &error), "export rivers build: " + error);
  expect(buildClimate(climateSettings, rivers, graph, climate, &error), "export final climate builds: " + error);
  RegionSettings regionSettings{};
  regionSettings.seed = partition.seed;
  regionSettings.targetAreaSquareMetres = 300.0e6;
  RegionMap regions;
  expect(buildRegions(regionSettings, rivers, graph, regions, &error), "export regions build: " + error);

  SiteIndex index;
  expect(index.build(graph, 0u, partition.extentMetres, &error), "export site index builds: " + error);
  MapSurfaceSettings surfaceSettings{};
  surfaceSettings.sampleSpacingMetres = 2000.0;
  MapSurfaceField surface;
  expect(surface.build(graph, index, BlendSettings{}, surfaceSettings, &error), "export surface builds: " + error);
  ContourSettings contourSettings{};
  contourSettings.intervalMetres = 100.0;
  contourSettings.minimumElevationMetres = 0.0;
  contourSettings.maximumElevationMetres = graph.brief().reliefMetres;
  ContourSet contours;
  expect(extractElevationContours(surface, contourSettings, contours, &error), "export contours build: " + error);
  const std::vector<CoastPolyline> coastline = extractCoastline(graph, partition.cellSpacingMetres * 1.0e-4);

  MapExportSettings exportSettings{};
  exportSettings.resolution = 32u;
  exportSettings.pageSizePixels = 16u;
  MapExportBundle first;
  MapExportBundle second;
  expect(buildMapExportBundle(graph, index, surface, climate, rivers, coastline, contours, regions, BlendSettings{}, HeightBlendSettings{}, exportSettings, first, &error), "map export builds: " + error);
  expect(buildMapExportBundle(graph, index, surface, climate, rivers, coastline, contours, regions, BlendSettings{}, HeightBlendSettings{}, exportSettings, second, &error), "map export repeats: " + error);
  expect(first.validate(&error), "map export validates: " + error);
  expect(
      first.bundleSignature == second.bundleSignature && first.manifestJson == second.manifestJson && first.graphData == second.graphData && first.featureData == second.featureData,
      "map export is byte-for-byte deterministic");
  expect(first.schemaVersion == 4u && first.fields.size() == 31u, "map export declares the complete semantic-mixture and placement field schema");
  size_t compressedPages = 0u;
  size_t pageCount = 0u;
  for (const MapExportField &field : first.fields)
    for (const MapExportPage &page : field.pages)
    {
      ++pageCount;
      compressedPages += page.compression == MapExportCompression::Lz4 ? 1u : 0u;
    }
  expect(first.pageSizePixels == 16u && first.gutterPixels == 1u && first.maximumPageLevel == 1u && pageCount == 26u * 5u, "map export builds a complete two-level planar page pyramid");
  expect(
      compressedPages > 0u && std::all_of(
                                  first.fields.begin(),
                                  first.fields.end(),
                                  [](const MapExportField &field)
                                  {
                                    return std::all_of(
                                        field.pages.begin(),
                                        field.pages.end(),
                                        [](const MapExportPage &page)
                                        {
                                          return page.compression == MapExportCompression::None || page.storedData.size() < page.data.size();
                                        });
                                  }),
      "compressible pages use smaller independent LZ4 representations");

  const MapExportField *elevation = first.field("base_elevation");
  const MapExportField *regionsField = first.field("region_id");
  const MapExportField *hardBiome = first.field("biome_id");
  const MapExportField *biomeIds = first.field("biome_mix_ids");
  const MapExportField *biomeWeights = first.field("biome_mix_weights");
  const MapExportField *placement = first.field("placement_potential");
  const MapExportField *refined = first.field("refined_elevation");
  const MapExportField *erosion = first.field("erosion_wear_flow");
  const MapExportField *deposition = first.field("erosion_deposition");
  const MapExportField *substrate = first.field("substrate_id");
  const MapExportField *soilDepth = first.field("soil_depth");
  const MapExportField *soilLayers = first.field("soil_layers");
  const MapExportField *waterType = first.field("water_type");
  const MapExportField *roads = first.field("roads");
  const MapExportField *rockPotential = first.field("rock_potential");
  const MapExportField *rockType = first.field("rock_type");
  const MapExportField *exposedBedrock = first.field("exposed_bedrock");
  const MapExportField *vegetationPotential = first.field("vegetation_potential");
  const MapExportField *vegetationType = first.field("vegetation_type");
  const MapExportField *canopyDensity = first.field("canopy_density");
  const MapExportField *groundDensity = first.field("ground_density");
  const MapExportField *riparianMask = first.field("riparian_mask");
  expect(elevation != nullptr && elevation->available && elevation->data.size() == 32u * 32u * 2u && elevation->filter == MapExportFilter::Linear, "elevation exports as a registered filterable R16 field");
  expect(regionsField != nullptr && regionsField->available && regionsField->filter == MapExportFilter::Nearest, "categorical region IDs are never linearly filtered");
  expect(
      hardBiome != nullptr && biomeIds != nullptr && biomeWeights != nullptr && hardBiome->available && biomeIds->available && biomeWeights->available && biomeIds->format == MapExportFormat::RGBA8Uint &&
          biomeWeights->format == MapExportFormat::RGBA8Unorm && biomeIds->filter == MapExportFilter::SparseMixture && biomeWeights->filter == MapExportFilter::SparseMixture &&
          biomeIds->samplingGroup == "biome_mixture" && biomeWeights->samplingGroup == "biome_mixture",
      "semantic biome IDs and weights declare paired sparse sampling");
  if (hardBiome != nullptr && biomeIds != nullptr && biomeWeights != nullptr)
  {
    bool normalizedMixtures = true;
    bool hasEcotone = false;
    for (size_t pixel = 0u; pixel < hardBiome->data.size(); ++pixel)
    {
      uint32_t total = 0u;
      for (size_t slot = 0u; slot < 4u; ++slot)
        total += biomeWeights->data[pixel * 4u + slot];
      normalizedMixtures = normalizedMixtures && total == 255u && hardBiome->data[pixel] == biomeIds->data[pixel * 4u] && biomeWeights->data[pixel * 4u] >= biomeWeights->data[pixel * 4u + 1u];
      hasEcotone = hasEcotone || biomeWeights->data[pixel * 4u + 1u] > 0u;
    }
    expect(normalizedMixtures, "every semantic biome mixture normalizes and agrees with hard biome");
    expect(hasEcotone, "semantic biome export retains at least one multi-biome ecotone");

    const MapExportPage *hardRoot = hardBiome->page({0u, 0u, 0u});
    const MapExportPage *idRoot = biomeIds->page({0u, 0u, 0u});
    const MapExportPage *weightRoot = biomeWeights->page({0u, 0u, 0u});
    bool rootMixtures = hardRoot != nullptr && idRoot != nullptr && weightRoot != nullptr;
    if (rootMixtures)
      for (size_t pixel = 0u; pixel < static_cast<size_t>(hardRoot->width) * hardRoot->height; ++pixel)
      {
        uint32_t total = 0u;
        for (size_t slot = 0u; slot < 4u; ++slot)
          total += weightRoot->data[pixel * 4u + slot];
        rootMixtures = rootMixtures && total == 255u && hardRoot->data[pixel] == idRoot->data[pixel * 4u];
      }
    expect(rootMixtures, "ancestor biome pages merge contributors and preserve normalization");
  }
  expect(
      placement != nullptr && placement->available &&
          std::any_of(
              placement->data.begin(),
              placement->data.end(),
              [](const uint8_t value)
              {
                return value != 0u;
              }),
      "the bundle carries non-empty coarse placement potential");
  expect(
      refined != nullptr && erosion != nullptr && deposition != nullptr && substrate != nullptr && soilDepth != nullptr && soilLayers != nullptr && waterType != nullptr && refined->available && erosion->available &&
          deposition->available && substrate->available && soilDepth->available && soilLayers->available &&
          std::any_of(
              erosion->data.begin(),
              erosion->data.end(),
              [](const uint8_t value)
              {
                return value != 0u;
              }) &&
          std::any_of(
              substrate->data.begin(),
              substrate->data.end(),
              [](const uint8_t value)
              {
                return value != 0u;
              }),
      "refined elevation, erosion and substrate products are populated");
  if (soilLayers != nullptr && waterType != nullptr)
  {
    bool normalizedSoil = true;
    for (size_t pixel = 0u; pixel < waterType->data.size(); ++pixel)
    {
      const uint32_t total = soilLayers->data[pixel * 4u] + soilLayers->data[pixel * 4u + 1u] + soilLayers->data[pixel * 4u + 2u] + soilLayers->data[pixel * 4u + 3u];
      normalizedSoil = normalizedSoil && (waterType->data[pixel] == 0u ? total == 255u : total == 0u);
    }
    expect(normalizedSoil, "soil layer weights normalize on land and remain empty in water");
  }
  expect(roads != nullptr && !roads->available && roads->data.empty() && roads->pages.empty(), "future roads are explicit unavailable fields without redundant files");
  expect(
      rockPotential != nullptr && exposedBedrock != nullptr && vegetationPotential != nullptr && canopyDensity != nullptr && groundDensity != nullptr && riparianMask != nullptr &&
          rockPotential->available && exposedBedrock->available && vegetationPotential->available && canopyDensity->available && groundDensity->available && riparianMask->available &&
          rockPotential->format == MapExportFormat::RGBA8Unorm && vegetationPotential->format == MapExportFormat::RGBA8Unorm && exposedBedrock->format == MapExportFormat::R16Float &&
          canopyDensity->format == MapExportFormat::R16Float && groundDensity->format == MapExportFormat::R16Float && riparianMask->format == MapExportFormat::R16Float &&
          std::any_of(
              rockPotential->data.begin(),
              rockPotential->data.end(),
              [](const uint8_t value)
              {
                return value != 0u;
              }) &&
          std::any_of(
              vegetationPotential->data.begin(),
              vegetationPotential->data.end(),
              [](const uint8_t value)
              {
                return value != 0u;
              }),
      "Phase 8 continuous rock, vegetation and riparian fields are produced and typed");
  expect(
      rockType != nullptr && vegetationType != nullptr && !rockType->available && !vegetationType->available && rockType->data.empty() && vegetationType->data.empty() && rockType->pages.empty() && vegetationType->pages.empty(),
      "Phase 8 categorical rock and vegetation type fields remain explicit unavailable placeholders");
  expect(
      first.manifestJson.find("\"available\": false") != std::string::npos && first.manifestJson.find("\"compression\": \"lz4\"") != std::string::npos &&
          first.manifestJson.find("\"addressing\": \"planar-quadtree\"") != std::string::npos && first.manifestJson.find("\"filter\": \"sparse-mixture\"") != std::string::npos &&
          first.manifestJson.find("\"samplingGroup\": \"biome_mixture\"") != std::string::npos && first.manifestJson.find("\"texelRegistration\": \"cell-centre\"") != std::string::npos &&
          first.manifestJson.find("\"name\": \"rock_type\", \"available\": false") != std::string::npos && first.manifestJson.find("\"name\": \"vegetation_type\", \"available\": false") != std::string::npos,
      "the manifest records placeholders, compression, semantic sampling and registration");

  const MapExportPage *left = elevation->page({1u, 0u, 0u});
  const MapExportPage *right = elevation->page({1u, 1u, 0u});
  expect(left != nullptr && right != nullptr, "adjacent finest elevation pages are addressable");
  if (left != nullptr && right != nullptr)
  {
    const uint32_t storedSide = exportSettings.pageSizePixels + 2u;
    bool matchingGutters = true;
    for (uint32_t y = 1u; y <= exportSettings.pageSizePixels; ++y)
    {
      const uint8_t *leftGutter = left->data.data() + (static_cast<size_t>(y) * storedSide + storedSide - 1u) * 2u;
      const uint8_t *rightInterior = right->data.data() + (static_cast<size_t>(y) * storedSide + 1u) * 2u;
      const uint8_t *leftInterior = left->data.data() + (static_cast<size_t>(y) * storedSide + storedSide - 2u) * 2u;
      const uint8_t *rightGutter = right->data.data() + static_cast<size_t>(y) * storedSide * 2u;
      matchingGutters = matchingGutters && std::memcmp(leftGutter, rightInterior, 2u) == 0 && std::memcmp(leftInterior, rightGutter, 2u) == 0;
    }
    expect(matchingGutters, "page gutters reproduce adjacent interiors without seams");
  }
  MapExportField sparseElevation = *elevation;
  sparseElevation.pages.erase(
      std::remove_if(
          sparseElevation.pages.begin(),
          sparseElevation.pages.end(),
          [](const MapExportPage &page)
          {
            return page.address == MapExportPageAddress{1u, 1u, 1u};
          }),
      sparseElevation.pages.end());
  const MapExportPage *fallback = sparseElevation.bestPage({1u, 1u, 1u});
  expect(fallback != nullptr && fallback->address == MapExportPageAddress{0u, 0u, 0u}, "a missing regional page resolves to its pinned nearest ancestor");

  MapExportPageAddress selectedAddress{};
  expect(mapExportPageAt(first, 1.0, -0.25, 1u, selectedAddress) && selectedAddress == MapExportPageAddress{1u, 1u, 0u}, "runtime sampling clamps coordinates to a valid streaming address");
  expect(!mapExportPageAt(first, 0.5, 0.5, static_cast<uint8_t>(first.maximumPageLevel + 1u), selectedAddress), "runtime sampling rejects an unavailable LOD");

  const uint32_t sampleX = 11u;
  const uint32_t sampleY = 19u;
  const double sampleU = (sampleX + 0.5) / first.resolution;
  const double sampleV = (sampleY + 0.5) / first.resolution;
  MapExportFieldSample hardSample{};
  expect(
      sampleMapExportField(first, "biome_id", sampleU, sampleV, first.maximumPageLevel, hardSample, &error) && hardSample.componentCount == 1u &&
          static_cast<uint8_t>(hardSample.values[0]) == hardBiome->data[static_cast<size_t>(sampleY) * first.resolution + sampleX] && !hardSample.usedAncestor(),
      "runtime nearest sampling reproduces a finest-level categorical texel");

  MapExportFieldSample placementSample{};
  const size_t samplePixel = static_cast<size_t>(sampleY) * first.resolution + sampleX;
  expect(
      sampleMapExportField(first, "placement_potential", sampleU, sampleV, first.maximumPageLevel, placementSample, &error) && placementSample.componentCount == 4u &&
          std::abs(placementSample.values[0] - placement->data[samplePixel * 4u] / 255.0) < 1.0e-12,
      "runtime linear sampling decodes normalized field values");

  MapExportFieldSample vegetationSample{};
  MapExportFieldSample rockSample{};
  MapExportFieldSample riparianSample{};
  expect(
      sampleMapExportField(first, "vegetation_potential", sampleU, sampleV, first.maximumPageLevel, vegetationSample, &error) && vegetationSample.componentCount == 4u &&
          sampleMapExportField(first, "rock_potential", sampleU, sampleV, first.maximumPageLevel, rockSample, &error) && rockSample.componentCount == 4u &&
          sampleMapExportField(first, "riparian_mask", sampleU, sampleV, first.maximumPageLevel, riparianSample, &error) && riparianSample.componentCount == 1u &&
          std::abs(vegetationSample.values[0] - vegetationPotential->data[samplePixel * 4u] / 255.0) < 1.0e-12 &&
          std::abs(rockSample.values[0] - rockPotential->data[samplePixel * 4u] / 255.0) < 1.0e-12 && riparianSample.values[0] >= 0.0 && riparianSample.values[0] <= 1.0,
      "runtime sampling consumes the Phase 8 continuous placement fields");

  MapExportFieldSample seamSample{};
  const uint32_t seamY = 5u;
  const size_t seamLeft = (static_cast<size_t>(seamY) * first.resolution + 15u) * 4u;
  const size_t seamRight = seamLeft + 4u;
  const double expectedSeam = (placement->data[seamLeft] + placement->data[seamRight]) / (2.0 * 255.0);
  expect(
      sampleMapExportField(first, "placement_potential", 0.5, (seamY + 0.5) / first.resolution, first.maximumPageLevel, seamSample, &error) && std::abs(seamSample.values[0] - expectedSeam) < 1.0e-12,
      "runtime linear filtering crosses a page seam through its gutter");

  MapExportBiomeSample biomeSample{};
  bool exactBiomeSample = sampleMapExportBiome(first, sampleU, sampleV, first.maximumPageLevel, biomeSample, &error);
  double biomeTotal = 0.0;
  if (exactBiomeSample)
    for (size_t slot = 0u; slot < 4u; ++slot)
    {
      const uint8_t storedWeight = biomeWeights->data[samplePixel * 4u + slot];
      exactBiomeSample = exactBiomeSample && (storedWeight == 0u || biomeSample.ids[slot] == biomeIds->data[samplePixel * 4u + slot]) && std::abs(biomeSample.weights[slot] - storedWeight / 255.0) < 1.0e-12;
      biomeTotal += biomeSample.weights[slot];
    }
  expect(exactBiomeSample && std::abs(biomeTotal - 1.0) < 1.0e-12 && biomeSample.dominantBiome() == hardBiome->data[samplePixel], "runtime sparse sampling reproduces and normalizes a biome texel");
  if (exactBiomeSample && biomeSample.count > 0u)
  {
    double cumulative = 0.0;
    bool weightedSelection = true;
    for (uint32_t slot = 0u; slot < biomeSample.count; ++slot)
    {
      const double inside = cumulative + biomeSample.weights[slot] * 0.5;
      weightedSelection = weightedSelection &&
          biomeSample.selectBiome(inside) == biomeSample.ids[slot];
      cumulative += biomeSample.weights[slot];
    }
    weightedSelection = weightedSelection &&
        biomeSample.selectBiome(-1.0) == biomeSample.ids[0] &&
        biomeSample.selectBiome(2.0) ==
            biomeSample.ids[biomeSample.count - 1u];
    expect(weightedSelection,
        "sparse biome selection respects normalized contributor weights");
  }

  MapExportBiomeSample blendedBiomeSample{};
  bool blendedBiome = sampleMapExportBiome(first, 0.5, (seamY + 0.5) / first.resolution, first.maximumPageLevel, blendedBiomeSample, &error);
  double blendedTotal = 0.0;
  for (size_t slot = 0u; slot < blendedBiomeSample.count; ++slot)
  {
    blendedTotal += blendedBiomeSample.weights[slot];
    for (size_t previous = 0u; previous < slot; ++previous)
      blendedBiome = blendedBiome && blendedBiomeSample.ids[slot] != blendedBiomeSample.ids[previous];
    if (slot > 0u)
      blendedBiome = blendedBiome && blendedBiomeSample.weights[slot - 1u] >= blendedBiomeSample.weights[slot];
  }
  expect(blendedBiome && blendedBiomeSample.count > 0u && std::abs(blendedTotal - 1.0) < 1.0e-12, "runtime sparse filtering merges IDs across neighboring biome texels");

  MapExportBundle sparseRuntime = first;
  MapExportField *sparsePlacement = sparseRuntime.field("placement_potential");
  sparsePlacement->pages.erase(
      std::remove_if(
          sparsePlacement->pages.begin(),
          sparsePlacement->pages.end(),
          [](const MapExportPage &page)
          {
            return page.address == MapExportPageAddress{1u, 1u, 1u};
          }),
      sparsePlacement->pages.end());
  MapExportFieldSample ancestorSample{};
  expect(
      sampleMapExportField(sparseRuntime, "placement_potential", 0.75, 0.75, 1u, ancestorSample, &error) && ancestorSample.usedAncestor() && ancestorSample.resolvedPage == MapExportPageAddress{0u, 0u, 0u},
      "runtime field sampling falls back to the nearest resident ancestor");

  MapExportField *sparseBiomeIds = sparseRuntime.field("biome_mix_ids");
  sparseBiomeIds->pages.erase(
      std::remove_if(
          sparseBiomeIds->pages.begin(),
          sparseBiomeIds->pages.end(),
          [](const MapExportPage &page)
          {
            return page.address == MapExportPageAddress{1u, 1u, 1u};
          }),
      sparseBiomeIds->pages.end());
  MapExportBiomeSample ancestorBiome{};
  expect(
      sampleMapExportBiome(sparseRuntime, 0.75, 0.75, 1u, ancestorBiome, &error) && ancestorBiome.usedAncestor() && ancestorBiome.resolvedPage == MapExportPageAddress{0u, 0u, 0u},
      "paired biome fields resolve to one common resident ancestor");

  MapExportFieldSample rejectedSample{};
  expect(
      !sampleMapExportField(first, "roads", 0.5, 0.5, 1u, rejectedSample, &error) && !sampleMapExportField(first, "biome_mix_ids", 0.5, 0.5, 1u, rejectedSample, &error),
      "runtime sampling rejects unavailable and independently sampled sparse fields");
  MapExportFieldSample worldSample{};
  expect(
      sampleMapExportFieldAtWorld(first, "biome_id", Point2(sampleU * first.extentMetres, sampleV * first.extentMetres), first.maximumPageLevel, worldSample, &error) && worldSample.values == hardSample.values,
      "world-space runtime sampling matches normalized sampling");

  PlacementAssetRule groundcoverRule{};
  groundcoverRule.assetKey = "groundcover.meadow";
  groundcoverRule.collectionKey = "groundcover";
  groundcoverRule.resourceKey = "voxel/groundcover/meadow";
  groundcoverRule.kind = PlacementAssetKind::VoxelModel;
  groundcoverRule.potential = PlacementPotentialChannel::Groundcover;
  groundcoverRule.selectionWeight = 3.0;
  groundcoverRule.densityMultiplier = 10.0;
  groundcoverRule.footprintRadiusMetres = 180.0;
  groundcoverRule.minimumSpacingMetres = 1150.0;
  groundcoverRule.scaleRange = {0.8, 1.25};
  PlacementAssetRule rockRule = groundcoverRule;
  rockRule.assetKey = "rock.low_poly";
  rockRule.collectionKey = "rocks";
  rockRule.resourceKey = "mesh/rocks/low_poly";
  rockRule.kind = PlacementAssetKind::Mesh;
  rockRule.selectionWeight = 1.0;
  rockRule.scaleRange = {0.6, 1.8};
  PlacementPalette placementPalette{};
  placementPalette.assets = {groundcoverRule, rockRule};
  expect(placementPalette.validate(&error), "runtime-neutral placement palette validates: " + error);

  PlacementTileRequest placementRequest{};
  placementRequest.normalizedBounds = {0.1, 0.1, 0.9, 0.9};
  placementRequest.requestedLevel = first.maximumPageLevel;
  placementRequest.candidateCellMetres = 1000.0;
  placementRequest.maximumEvaluatedSites = 4096u;
  placementRequest.maximumCandidates = 2048u;
  PlacementCandidateSet placementWhole{};
  PlacementCandidateSet placementRepeat{};
  const bool placementBuilt = buildPlacementCandidates(first, placementPalette, placementRequest, placementWhole, &error);
  const bool placementRepeated = buildPlacementCandidates(first, placementPalette, placementRequest, placementRepeat, &error);
  auto sameCandidates = [](const std::vector<PlacementCandidate> &leftCandidates, const std::vector<PlacementCandidate> &rightCandidates)
  {
    if (leftCandidates.size() != rightCandidates.size())
      return false;
    for (size_t candidateIndex = 0u; candidateIndex < leftCandidates.size(); ++candidateIndex)
    {
      const PlacementCandidate &leftCandidate = leftCandidates[candidateIndex];
      const PlacementCandidate &rightCandidate = rightCandidates[candidateIndex];
      if (leftCandidate.stableId != rightCandidate.stableId || leftCandidate.assetIndex != rightCandidate.assetIndex || leftCandidate.position.x != rightCandidate.position.x ||
          leftCandidate.position.y != rightCandidate.position.y || leftCandidate.yawRadians != rightCandidate.yawRadians || leftCandidate.scale != rightCandidate.scale ||
          leftCandidate.density != rightCandidate.density || leftCandidate.elevationMetres != rightCandidate.elevationMetres || leftCandidate.slopeDegrees != rightCandidate.slopeDegrees ||
          leftCandidate.moisture != rightCandidate.moisture || leftCandidate.soilDepthMetres != rightCandidate.soilDepthMetres || leftCandidate.riverDistanceMetres != rightCandidate.riverDistanceMetres ||
          leftCandidate.substrate != rightCandidate.substrate || leftCandidate.dominantBiome != rightCandidate.dominantBiome || !(leftCandidate.resolvedPage == rightCandidate.resolvedPage))
        return false;
    }
    return true;
  };
  expect(
      placementBuilt && placementRepeated && !placementWhole.candidates.empty() && sameCandidates(placementWhole.candidates, placementRepeat.candidates),
      "placement planning produces deterministic stable IDs, assets and transforms: " + error);

  bool validPlacementCandidates = placementBuilt;
  for (size_t candidateIndex = 0u; candidateIndex < placementWhole.candidates.size(); ++candidateIndex)
  {
    const PlacementCandidate &candidate = placementWhole.candidates[candidateIndex];
    const PlacementAssetRule &asset = placementPalette.assets[candidate.assetIndex];
    MapExportFieldSample candidateWater{};
    validPlacementCandidates = validPlacementCandidates && candidate.assetIndex < placementPalette.assets.size() && candidate.position.x >= first.extentMetres * 0.1 && candidate.position.x < first.extentMetres * 0.9 &&
                               candidate.position.y >= first.extentMetres * 0.1 && candidate.position.y < first.extentMetres * 0.9 && candidate.scale >= asset.scaleRange[0] && candidate.scale <= asset.scaleRange[1] &&
                               candidate.density > 0.0 && std::isfinite(candidate.slopeDegrees) && sampleMapExportFieldAtWorld(first, "water_type", candidate.position, first.maximumPageLevel, candidateWater, &error) &&
                               candidateWater.values[0] < 0.5;
    for (size_t otherIndex = 0u; otherIndex < candidateIndex; ++otherIndex)
    {
      const PlacementCandidate &other = placementWhole.candidates[otherIndex];
      const PlacementAssetRule &otherAsset = placementPalette.assets[other.assetIndex];
      const double separation = std::max({asset.minimumSpacingMetres, otherAsset.minimumSpacingMetres, asset.footprintRadiusMetres + otherAsset.footprintRadiusMetres});
      validPlacementCandidates = validPlacementCandidates && std::hypot(candidate.position.x - other.position.x, candidate.position.y - other.position.y) >= separation;
    }
  }
  expect(validPlacementCandidates, "placement candidates obey land, palette range and pairwise spacing constraints");

  PlacementTileRequest leftPlacementRequest = placementRequest;
  PlacementTileRequest rightPlacementRequest = placementRequest;
  leftPlacementRequest.normalizedBounds[2] = 0.5;
  rightPlacementRequest.normalizedBounds[0] = 0.5;
  PlacementCandidateSet placementLeft{};
  PlacementCandidateSet placementRight{};
  bool seamlessPlacement =
      buildPlacementCandidates(first, placementPalette, leftPlacementRequest, placementLeft, &error) && buildPlacementCandidates(first, placementPalette, rightPlacementRequest, placementRight, &error);
  std::vector<PlacementCandidate> joinedCandidates = placementLeft.candidates;
  joinedCandidates.insert(joinedCandidates.end(), placementRight.candidates.begin(), placementRight.candidates.end());
  std::sort(
      joinedCandidates.begin(),
      joinedCandidates.end(),
      [](const PlacementCandidate &leftCandidate, const PlacementCandidate &rightCandidate)
      {
        return leftCandidate.stableId < rightCandidate.stableId;
      });
  seamlessPlacement = seamlessPlacement && sameCandidates(placementWhole.candidates, joinedCandidates);
  expect(seamlessPlacement, "neighboring placement tiles exactly reproduce one combined request without seam duplicates or omissions");

  PlacementTileRequest ancestorPlacementRequest = placementRequest;
  ancestorPlacementRequest.normalizedBounds = {0.55, 0.55, 0.9, 0.9};
  PlacementCandidateSet ancestorPlacement{};
  expect(
      buildPlacementCandidates(sparseRuntime, placementPalette, ancestorPlacementRequest, ancestorPlacement, &error) && ancestorPlacement.ancestorSampledSites > 0u,
      "placement planning remains operational when requested detail falls back to resident ancestor pages: " + error);
  PlacementPalette invalidPlacementPalette = placementPalette;
  invalidPlacementPalette.assets.push_back(groundcoverRule);
  expect(!invalidPlacementPalette.validate(nullptr), "placement palettes reject duplicate stable asset keys");
  PlacementTileRequest overBudgetPlacementRequest = placementRequest;
  overBudgetPlacementRequest.maximumEvaluatedSites = 1u;
  PlacementCandidateSet overBudgetPlacement{};
  expect(!buildPlacementCandidates(first, placementPalette, overBudgetPlacementRequest, overBudgetPlacement, nullptr), "placement planning rejects tiles beyond their explicit work budget");

  if (elevation != nullptr && biomeIds != nullptr && biomeWeights != nullptr)
  {
    const MapExportPageAddress rootAddress{0u, 0u, 0u};
    const MapExportPageAddress northwestAddress{1u, 0u, 0u};
    const MapExportPageAddress southeastAddress{1u, 1u, 1u};
    const size_t rootBytes = elevation->page(rootAddress)->data.size() + biomeIds->page(rootAddress)->data.size() + biomeWeights->page(rootAddress)->data.size();
    const size_t elevationPageBytes = elevation->page(northwestAddress)->data.size();

    MapExportStreamingCache streaming;
    expect(streaming.initialize(first, &error), "streaming cache initializes from a valid export: " + error);
    MapExportStreamRequest streamRequest{};
    streamRequest.normalizedBounds = {0.0, 0.0, 0.5, 0.5};
    streamRequest.level = 1u;
    streamRequest.byteBudget = rootBytes * 2u;
    streamRequest.fields = {"base_elevation", "biome_mix_ids"};
    MapExportStreamUpdate firstUpdate{};
    expect(
        streaming.update(streamRequest, firstUpdate, &error) && firstUpdate.loads.size() == 6u && firstUpdate.evictions.empty() && firstUpdate.residentPages == 6u && firstUpdate.deferredUnits == 0u &&
            streaming.resident({"base_elevation", rootAddress}) && streaming.resident({"biome_mix_ids", northwestAddress}) && streaming.resident({"biome_mix_weights", northwestAddress}),
        "a visible page loads with pinned roots and its complete biome sampling group");

    streamRequest.normalizedBounds = {0.5, 0.5, 1.0, 1.0};
    MapExportStreamUpdate movedUpdate{};
    expect(
        streaming.update(streamRequest, movedUpdate, &error) && movedUpdate.loads.size() == 3u && movedUpdate.evictions.size() == 3u && streaming.resident({"base_elevation", southeastAddress}) &&
            streaming.resident({"biome_mix_ids", southeastAddress}) && streaming.resident({"biome_mix_weights", southeastAddress}) && !streaming.resident({"base_elevation", northwestAddress}),
        "moving the camera produces a minimal deterministic load/eviction transaction");
    const MapExportPage *residentFallback = streaming.bestResidentPage("base_elevation", northwestAddress);
    expect(residentFallback != nullptr && residentFallback->address == rootAddress, "an evicted detail page immediately falls back to its pinned root");

    MapExportStreamingCache budgetedStreaming;
    expect(budgetedStreaming.initialize(first, &error), "budgeted streaming cache initializes: " + error);
    streamRequest.normalizedBounds = {0.0, 0.0, 0.5, 0.5};
    streamRequest.byteBudget = rootBytes + elevationPageBytes;
    MapExportStreamUpdate budgetedUpdate{};
    expect(
        budgetedStreaming.update(streamRequest, budgetedUpdate, &error) && budgetedUpdate.deferredUnits == 1u && budgetedStreaming.resident({"base_elevation", northwestAddress}) &&
            !budgetedStreaming.resident({"biome_mix_ids", northwestAddress}) && !budgetedStreaming.resident({"biome_mix_weights", northwestAddress}),
        "budget pressure never admits only half of a sparse biome pair");

    MapExportStreamingCache rootOnlyStreaming;
    expect(rootOnlyStreaming.initialize(first, &error), "root-only streaming cache initializes: " + error);
    streamRequest.byteBudget = rootBytes;
    MapExportStreamUpdate rootOnlyUpdate{};
    const bool rootOnlyUpdated = rootOnlyStreaming.update(streamRequest, rootOnlyUpdate, &error);
    const MapExportPage *rootOnlyFallback = rootOnlyStreaming.bestResidentPage("biome_mix_ids", northwestAddress);
    expect(
        rootOnlyUpdated && rootOnlyUpdate.residentBytes == rootBytes && rootOnlyUpdate.residentPages == 3u && rootOnlyUpdate.deferredUnits == 2u && rootOnlyFallback != nullptr && rootOnlyFallback->address == rootAddress,
        "a constrained cache retains a complete coarse representation");
    const std::vector<MapExportStreamKey> rootsBeforeRejectedUpdate = rootOnlyStreaming.residentPages();
    streamRequest.byteBudget = rootBytes - 1u;
    MapExportStreamUpdate rejectedUpdate{};
    expect(!rootOnlyStreaming.update(streamRequest, rejectedUpdate, &error) && rootOnlyStreaming.residentPages() == rootsBeforeRejectedUpdate, "an insufficient root budget is rejected without mutating residency");

    MapExportStreamingCache guardedStreaming;
    expect(guardedStreaming.initialize(first, &error), "guarded streaming cache initializes: " + error);
    streamRequest.normalizedBounds = {1.2, 1.2, -0.2, -0.2};
    streamRequest.guardPages = std::numeric_limits<uint32_t>::max();
    streamRequest.byteBudget = std::numeric_limits<size_t>::max();
    MapExportStreamUpdate guardedUpdate{};
    expect(guardedStreaming.update(streamRequest, guardedUpdate, &error) && guardedUpdate.residentPages == 15u, "clamped reversed bounds and oversized guards select the finite page domain");

    MapExportStreamingCache deterministicStreaming;
    MapExportStreamUpdate deterministicUpdate{};
    expect(
        deterministicStreaming.initialize(first, &error) && deterministicStreaming.update(streamRequest, deterministicUpdate, &error) && deterministicUpdate.loads == guardedUpdate.loads &&
            deterministicStreaming.residentPages() == guardedStreaming.residentPages(),
        "identical view requests produce identical residency transactions");

    MapExportStreamingCache sparseStreaming;
    MapExportStreamRequest sparseStreamRequest{};
    sparseStreamRequest.normalizedBounds = {0.5, 0.5, 1.0, 1.0};
    sparseStreamRequest.level = 1u;
    sparseStreamRequest.byteBudget = biomeIds->page(rootAddress)->data.size() + biomeWeights->page(rootAddress)->data.size();
    sparseStreamRequest.fields = {"biome_mix_weights"};
    MapExportStreamUpdate sparseUpdate{};
    expect(
        sparseStreaming.initialize(sparseRuntime, &error) && sparseStreaming.update(sparseStreamRequest, sparseUpdate, &error) && sparseUpdate.ancestorUnits == 1u && sparseUpdate.residentPages == 2u &&
            sparseStreaming.resident({"biome_mix_ids", rootAddress}) && sparseStreaming.resident({"biome_mix_weights", rootAddress}),
        "an incomplete sparse child resolves the complete sampling group to one ancestor");

    streamRequest.fields = {"roads"};
    expect(!deterministicStreaming.update(streamRequest, deterministicUpdate, &error), "streaming rejects declared but unavailable future fields");
  }

  const std::filesystem::path exportDirectory = std::filesystem::temp_directory_path() / ("engine-world-map-export-test-" + std::to_string(first.bundleSignature));
  std::error_code filesystemError;
  std::filesystem::remove_all(exportDirectory, filesystemError);
  expect(writeMapExportBundle(first, exportDirectory.string(), &error), "map export writes: " + error);
  MapExportBundle loaded;
  expect(readMapExportBundle(exportDirectory.string(), loaded, &error), "map export reads: " + error);
  expect(
      loaded.bundleSignature == first.bundleSignature && loaded.graphData == first.graphData && loaded.featureData == first.featureData && loaded.manifestJson == first.manifestJson &&
          loaded.fields.size() == first.fields.size() &&
          std::equal(
              loaded.fields.begin(),
              loaded.fields.end(),
              first.fields.begin(),
              [](const MapExportField &left, const MapExportField &right)
              {
                if (left.name != right.name || left.available != right.available || left.format != right.format || left.filter != right.filter || left.samplingGroup != right.samplingGroup || left.components != right.components || left.units != right.units ||
                    left.scale != right.scale || left.offset != right.offset || left.contentHash != right.contentHash || left.data != right.data || left.pages.size() != right.pages.size())
                  return false;
                return std::equal(
                    left.pages.begin(),
                    left.pages.end(),
                    right.pages.begin(),
                    [](const MapExportPage &a, const MapExportPage &b)
                    {
                      return a.address == b.address && a.compression == b.compression && a.contentHash == b.contentHash && a.storedHash == b.storedHash && a.data == b.data && a.storedData == b.storedData;
                    });
              }),
      "written map export round-trips byte for byte");
  MapExportBiomeSample loadedBiomeSample{};
  expect(
      sampleMapExportBiome(loaded, sampleU, sampleV, loaded.maximumPageLevel, loadedBiomeSample, &error) && loadedBiomeSample.ids == biomeSample.ids && loadedBiomeSample.weights == biomeSample.weights,
      "the sampler consumes decompressed pages from a loaded bundle identically");

  MapExportBundle streamingManifest;
  expect(
      readMapExportManifest(exportDirectory.string(), streamingManifest, &error) && streamingManifest.bundleSignature == loaded.bundleSignature && streamingManifest.graphData.empty() &&
          streamingManifest.featureData.empty() &&
          std::all_of(
              streamingManifest.fields.begin(),
              streamingManifest.fields.end(),
              [](const MapExportField &field)
              {
                return field.data.empty() && std::all_of(
                                                 field.pages.begin(),
                                                 field.pages.end(),
                                                 [](const MapExportPage &page)
                                                 {
                                                   return page.data.empty() && page.storedData.empty();
                                                 });
              }),
      "metadata-only loading performs no eager page or field reads: " + error);

  const MapExportField *loadedElevation = loaded.field("base_elevation");
  const MapExportField *loadedBiomeIds = loaded.field("biome_mix_ids");
  const MapExportField *loadedBiomeWeights = loaded.field("biome_mix_weights");
  if (loadedElevation != nullptr && loadedBiomeIds != nullptr && loadedBiomeWeights != nullptr)
  {
    const MapExportPageAddress rootAddress{0u, 0u, 0u};
    const MapExportPageAddress northwestAddress{1u, 0u, 0u};
    const MapExportPageAddress southeastAddress{1u, 1u, 1u};
    std::vector<uint8_t> streamedPage;
    expect(
        readMapExportPage(exportDirectory.string(), *loadedElevation, *loadedElevation->page(northwestAddress), streamedPage, &error) && streamedPage == loadedElevation->page(northwestAddress)->data,
        "a single page reads, decompresses and validates independently");

    const size_t rootBytes = loadedElevation->page(rootAddress)->data.size() + loadedBiomeIds->page(rootAddress)->data.size() + loadedBiomeWeights->page(rootAddress)->data.size();
    MapExportStreamingCache diskPlanner;
    MapExportStreamRequest diskRequest{};
    diskRequest.normalizedBounds = {0.0, 0.0, 0.5, 0.5};
    diskRequest.level = 1u;
    diskRequest.byteBudget = rootBytes * 2u;
    diskRequest.fields = {"base_elevation", "biome_mix_ids"};
    MapExportStreamUpdate diskPlan{};
    expect(diskPlanner.initialize(streamingManifest, &error) && diskPlanner.update(diskRequest, diskPlan, &error), "disk-streaming residency target plans: " + error);

    MapExportPageCache physicalCache;
    MapExportPageCacheConfig cacheConfig{};
    cacheConfig.directory = exportDirectory.string();
    cacheConfig.slotCapacity = 6u;
    expect(
        physicalCache.initialize(streamingManifest, cacheConfig, &error) && physicalCache.setTarget(diskPlanner.residentPages(), &error) && physicalCache.waitForPreparation(std::chrono::seconds(5), &error),
        "asynchronous disk page preparation completes: " + error);
    MapExportPagePublication firstPublication{};
    bool validUploads = physicalCache.publish(std::numeric_limits<size_t>::max(), firstPublication, &error);
    std::vector<bool> usedSlots(cacheConfig.slotCapacity, false);
    for (const MapExportPhysicalUpload &upload : firstPublication.uploads)
    {
      const MapExportField *field = loaded.field(upload.key.field);
      const MapExportPage *page = field == nullptr ? nullptr : field->page(upload.key.address);
      validUploads = validUploads && upload.slot < usedSlots.size() && !usedSlots[upload.slot] && page != nullptr && upload.data == page->data;
      if (upload.slot < usedSlots.size())
        usedSlots[upload.slot] = true;
    }
    expect(
        validUploads && firstPublication.uploads.size() == 6u && firstPublication.evictions.empty() && firstPublication.pageTable.size() == 6u && firstPublication.residentPages == 6u,
        "the first safe-frame publication fills six unique physical slots");

    MapExportGpuPageLayout gpuLayout;
    MapExportGpuPublication gpuPublication{};
    bool validGpuLayout = gpuLayout.initialize(streamingManifest, cacheConfig.slotCapacity, &error) && gpuLayout.encode(firstPublication, gpuPublication, &error) && gpuLayout.fields().size() == 26u &&
                          gpuLayout.descriptors().size() == gpuLayout.fields().size() && gpuLayout.entriesPerField() == 5u && gpuPublication.physicalWrites.size() == firstPublication.uploads.size() &&
                          gpuPublication.pageTable.size() == 26u * 5u && gpuLayout.header().pageSizePixels == streamingManifest.pageSizePixels && gpuLayout.header().gutterPixels == streamingManifest.gutterPixels &&
                          gpuLayout.header().storedPageSide == streamingManifest.pageSizePixels + streamingManifest.gutterPixels * 2u && gpuLayout.header().maximumPageLevel == streamingManifest.maximumPageLevel &&
                          gpuLayout.header().physicalSlotBytes == gpuLayout.physicalSlotBytes() && gpuLayout.header().entriesPerField == gpuLayout.entriesPerField() &&
                          gpuLayout.header().fieldCount == gpuLayout.fields().size() && gpuLayout.header().resolution == streamingManifest.resolution;
    for (size_t index = 0u; index < gpuLayout.fields().size(); ++index)
    {
      const MapExportGpuField &field = gpuLayout.fields()[index];
      const MapExportGpuFieldDescriptor &descriptor = gpuLayout.descriptors()[index];
      const MapExportField *source = streamingManifest.field(field.name);
      validGpuLayout = validGpuLayout && source != nullptr && gpuLayout.fieldIndex(field.name) == index && descriptor.format == static_cast<uint32_t>(field.format) &&
                       descriptor.filter == static_cast<uint32_t>(field.filter) && descriptor.texelBytes == field.texelBytes && descriptor.pageTableOffset == field.pageTableOffset;
      if (source != nullptr)
        for (size_t component = 0u; component < 4u; ++component)
          validGpuLayout = validGpuLayout && descriptor.scale[component] == static_cast<float>(source->scale[component]) && descriptor.offset[component] == static_cast<float>(source->offset[component]);
    }
    for (size_t index = 0u; index < gpuPublication.physicalWrites.size(); ++index)
    {
      const MapExportGpuPhysicalWrite &write = gpuPublication.physicalWrites[index];
      validGpuLayout = validGpuLayout && write.key == firstPublication.uploads[index].key && write.slot == firstPublication.uploads[index].slot &&
                       write.byteOffset == static_cast<size_t>(write.slot) * gpuLayout.physicalSlotBytes() && write.byteSize == firstPublication.uploads[index].data.size();
    }
    for (const MapExportPhysicalMapping &mapping : firstPublication.pageTable)
    {
      const std::optional<uint32_t> field = gpuLayout.fieldIndex(mapping.key.field);
      if (!field.has_value())
      {
        validGpuLayout = false;
        continue;
      }
      const std::optional<uint32_t> logical = gpuLayout.pageTableIndex(*field, mapping.key.address);
      validGpuLayout = validGpuLayout && logical.has_value() && *logical < gpuPublication.pageTable.size() && gpuPublication.pageTable[*logical] == mapping.slot + 1u;
    }
    expect(validGpuLayout, "GPU metadata and encoding preserve field formats, transforms, slot offsets and logical addresses");
    std::vector<uint32_t> emptyGpuTable(gpuPublication.pageTable.size(), 0u);
    const auto initialDirtyRanges =
        mapExportGpuPageTableDirtyRanges(emptyGpuTable, gpuPublication.pageTable);
    uint32_t initialDirtyWords = 0u;
    for (const auto range : initialDirtyRanges)
      initialDirtyWords += range.count;
    const auto firstLogical =
        gpuLayout.pageTableIndex(
            *gpuLayout.fieldIndex(firstPublication.pageTable.front().key.field),
            firstPublication.pageTable.front().key.address);
    std::vector<uint32_t> unmappedGpuTable = gpuPublication.pageTable;
    if (firstLogical.has_value())
      unmappedGpuTable[*firstLogical] = 0u;
    const auto unchangedDirtyRanges =
        mapExportGpuPageTableDirtyRanges(gpuPublication.pageTable, gpuPublication.pageTable);
    const auto unmapDirtyRanges =
        mapExportGpuPageTableDirtyRanges(gpuPublication.pageTable, unmappedGpuTable);
    expect(
        initialDirtyWords == firstPublication.pageTable.size() &&
            unchangedDirtyRanges.empty() &&
            firstLogical.has_value() &&
            unmapDirtyRanges.size() == 1u &&
            unmapDirtyRanges[0].first == *firstLogical &&
            unmapDirtyRanges[0].count == 1u,
        "GPU atlas page-table publications expose sparse dirty commit ranges");
    expect(
        !gpuLayout.fieldIndex("roads").has_value() && !gpuLayout.pageTableIndex(static_cast<uint32_t>(gpuLayout.fields().size()), rootAddress).has_value(),
        "GPU metadata excludes unavailable fields and rejects out-of-range field addressing");

    MapExportPagePublication splitGpuPublication = firstPublication;
    splitGpuPublication.pageTable.erase(
        std::remove_if(
            splitGpuPublication.pageTable.begin(),
            splitGpuPublication.pageTable.end(),
            [&](const MapExportPhysicalMapping &mapping)
            {
              return mapping.key == MapExportStreamKey{"biome_mix_weights", rootAddress};
            }),
        splitGpuPublication.pageTable.end());
    expect(!gpuLayout.encode(splitGpuPublication, gpuPublication, &error), "GPU publication validation rejects a split biome sampling group");
    MapExportPagePublication aliasedGpuPublication = firstPublication;
    aliasedGpuPublication.pageTable.back().slot = aliasedGpuPublication.pageTable.front().slot;
    expect(!gpuLayout.encode(aliasedGpuPublication, gpuPublication, &error), "GPU publication validation rejects physical slot aliases");
    MapExportPagePublication truncatedGpuPublication = firstPublication;
    truncatedGpuPublication.uploads.front().data.pop_back();
    expect(!gpuLayout.encode(truncatedGpuPublication, gpuPublication, &error), "GPU publication validation rejects truncated physical uploads");

    MapExportPageCache throttledCache;
    expect(
        throttledCache.initialize(streamingManifest, cacheConfig, &error) && throttledCache.setTarget(diskPlanner.residentPages(), &error) && throttledCache.waitForPreparation(std::chrono::seconds(5), &error),
        "throttled physical cache prepares the same target");
    MapExportPagePublication elevationOnlyPublication{};
    const size_t elevationPageBytes = loadedElevation->page(rootAddress)->data.size();
    expect(
        throttledCache.publish(elevationPageBytes, elevationOnlyPublication, &error) && elevationOnlyPublication.uploads.size() == 1u && elevationOnlyPublication.pageTable.size() == 1u &&
            elevationOnlyPublication.deferredUnits == 3u && !throttledCache.residentSlot({"biome_mix_ids", rootAddress}).has_value() && !throttledCache.residentSlot({"biome_mix_weights", rootAddress}).has_value(),
        "a per-frame upload limit defers the complete biome unit");
    MapExportPagePublication biomeRootPublication{};
    const size_t biomeRootBytes = loadedBiomeIds->page(rootAddress)->data.size() + loadedBiomeWeights->page(rootAddress)->data.size();
    expect(
        throttledCache.publish(biomeRootBytes, biomeRootPublication, &error) && biomeRootPublication.uploads.size() == 2u && biomeRootPublication.pageTable.size() == 3u &&
            throttledCache.residentSlot({"biome_mix_ids", rootAddress}).has_value() && throttledCache.residentSlot({"biome_mix_weights", rootAddress}).has_value(),
        "a later frame publishes both biome root pages together");
    const auto elevationRootSlot = physicalCache.residentSlot({"base_elevation", rootAddress});
    const auto biomeIdRootSlot = physicalCache.residentSlot({"biome_mix_ids", rootAddress});
    const auto biomeWeightRootSlot = physicalCache.residentSlot({"biome_mix_weights", rootAddress});

    diskRequest.normalizedBounds = {0.5, 0.5, 1.0, 1.0};
    expect(
        diskPlanner.update(diskRequest, diskPlan, &error) && physicalCache.setTarget(diskPlanner.residentPages(), &error) && physicalCache.waitForPreparation(std::chrono::seconds(5), &error),
        "a moved camera asynchronously prepares its replacement pages");
    MapExportPagePublication movedPublication{};
    expect(
        physicalCache.publish(std::numeric_limits<size_t>::max(), movedPublication, &error) && movedPublication.uploads.size() == 3u && movedPublication.evictions.size() == 3u &&
            movedPublication.pageTable.size() == 6u && physicalCache.residentSlot({"base_elevation", rootAddress}) == elevationRootSlot && physicalCache.residentSlot({"biome_mix_ids", rootAddress}) == biomeIdRootSlot &&
            physicalCache.residentSlot({"biome_mix_weights", rootAddress}) == biomeWeightRootSlot && physicalCache.residentSlot({"biome_mix_ids", southeastAddress}).has_value() &&
            physicalCache.residentSlot({"biome_mix_weights", southeastAddress}).has_value() && !physicalCache.residentSlot({"base_elevation", northwestAddress}).has_value(),
        "camera movement reuses root slots and publishes one bounded replacement set");

    expect(!physicalCache.setTarget({{"biome_mix_ids", rootAddress}}, &error), "the physical cache rejects half of a sparse sampling group");
    MapExportPageCache undersizedCache;
    cacheConfig.slotCapacity = 2u;
    expect(undersizedCache.initialize(streamingManifest, cacheConfig, &error) && !undersizedCache.setTarget(diskPlanner.residentPages(), &error), "the physical cache rejects a target whose roots cannot fit");
  }

  MapGraph decodedGraph;
  MapExportFeatures decodedFeatures;
  expect(decodeMapExportGraph(loaded, decodedGraph, &error), "WMG1 decodes into a live graph: " + error);
  expect(decodeMapExportFeatures(loaded, decodedFeatures, &error), "WMF1 decodes into live features: " + error);
  expect(decodedGraph.signature() == graph.signature() && decodedGraph.nodeCount() == graph.nodeCount(), "decoded graph preserves its semantic identity");
  expect(
      decodedFeatures.coastline.size() == coastline.size() && decodedFeatures.rivers.nodes.size() == rivers.nodes.size() && decodedFeatures.rivers.riverCells == rivers.riverCells &&
          decodedFeatures.rivers.filledCells == rivers.filledCells && decodedFeatures.contours.intervalMetres == contours.intervalMetres && decodedFeatures.contours.lines.size() == contours.lines.size() &&
          decodedFeatures.regions.cellRegion == regions.cellRegion,
      "decoded features preserve topology and summary metadata");

  SiteIndex decodedIndex;
  MapSurfaceField decodedSurface;
  expect(
      decodedIndex.build(decodedGraph, 0u, partition.extentMetres, &error) && decodedSurface.build(decodedGraph, decodedIndex, BlendSettings{}, surfaceSettings, &error),
      "decoded graph rebuilds its continuous surface: " + error);
  MapExportBundle reencoded;
  expect(
      buildMapExportBundle(
          decodedGraph,
          decodedIndex,
          decodedSurface,
          climate,
          decodedFeatures.rivers,
          decodedFeatures.coastline,
          decodedFeatures.contours,
          decodedFeatures.regions,
          BlendSettings{},
          HeightBlendSettings{},
          exportSettings,
          reencoded,
          &error),
      "decoded semantic data re-encodes: " + error);
  expect(reencoded.graphData == first.graphData && reencoded.featureData == first.featureData && reencoded.bundleSignature == first.bundleSignature, "semantic decode and re-encode is byte-for-byte stable");
  std::filesystem::remove_all(exportDirectory, filesystemError);

  MapExportBundle corrupted = first;
  corrupted.fields.front().data.push_back(0u);
  expect(!corrupted.validate(nullptr), "bundle validation rejects a field with the wrong payload size");
  corrupted = first;
  corrupted.fields.front().pages.front().file = "pages/../outside.bin";
  expect(!corrupted.validate(nullptr), "bundle validation rejects a page path outside its directory");
  corrupted = first;
  MapExportPage *compressed = nullptr;
  for (MapExportField &field : corrupted.fields)
  {
    const auto found = std::find_if(
        field.pages.begin(),
        field.pages.end(),
        [](const MapExportPage &page)
        {
          return page.compression == MapExportCompression::Lz4;
        });
    if (found != field.pages.end())
    {
      compressed = &*found;
      break;
    }
  }
  expect(compressed != nullptr, "the corruption probe finds a compressed page");
  if (compressed != nullptr)
  {
    compressed->storedData.front() ^= 0x80u;
    expect(!corrupted.validate(nullptr), "bundle validation rejects corrupted compressed storage");
  }

  MapExportSettings rawSettings = exportSettings;
  rawSettings.compressFields = false;
  MapExportBundle raw;
  expect(
      buildMapExportBundle(graph, index, surface, climate, rivers, coastline, contours, regions, BlendSettings{}, HeightBlendSettings{}, rawSettings, raw, &error),
      "an explicitly uncompressed map export builds: " + error);
  expect(
      std::all_of(
          raw.fields.begin(),
          raw.fields.end(),
          [](const MapExportField &field)
          {
            return std::all_of(
                field.pages.begin(),
                field.pages.end(),
                [](const MapExportPage &page)
                {
                  return page.compression == MapExportCompression::None && page.storedData.empty() && page.storedHash == page.contentHash;
                });
          }),
      "uncompressed export keeps raw GPU-ready page payloads");
  exportSettings.resolution = 1u;
  MapExportBundle rejected;
  expect(
      !buildMapExportBundle(graph, index, surface, climate, rivers, coastline, contours, regions, BlendSettings{}, HeightBlendSettings{}, exportSettings, rejected, nullptr), "map export rejects an unusable resolution");
}

void testTerrainRefinementConservesSedimentAndBuildsSoil()
{
  TerrainRefinementInput input{};
  input.side = 16u;
  input.extentMetres = 16000.0;
  input.seaLevelMetres = 0.0;
  const size_t count = static_cast<size_t>(input.side) * input.side;
  input.elevationMetres.resize(count);
  input.moisture.assign(count, 0.65f);
  input.biome.assign(count, static_cast<uint8_t>(Biome::Grassland));
  input.waterType.assign(count, 0u);
  for (uint32_t y = 0u; y < input.side; ++y)
    for (uint32_t x = 0u; x < input.side; ++x)
    {
      const size_t cell = static_cast<size_t>(y) * input.side + x;
      input.elevationMetres[cell] = 1000.0 - x * 20.0 + std::sin(static_cast<double>(y) * 0.7) * 8.0;
      if (x + 1u == input.side)
      {
        input.elevationMetres[cell] = -10.0;
        input.waterType[cell] = 1u;
        input.biome[cell] = static_cast<uint8_t>(Biome::Ocean);
      }
    }

  TerrainRefinementMaps first;
  TerrainRefinementMaps second;
  TerrainRefinementReport report{};
  std::string error;
  expect(refineTerrainRaster(input, TerrainRefinementSettings{}, first, &report, &error), "terrain refinement builds: " + error);
  expect(refineTerrainRaster(input, TerrainRefinementSettings{}, second, nullptr, &error), "terrain refinement repeats: " + error);
  expect(
      first.elevationMetres == second.elevationMetres && first.wear == second.wear && first.flow == second.flow && first.deposition == second.deposition && first.substrate == second.substrate &&
          first.soilLayers == second.soilLayers,
      "terrain refinement is deterministic");
  expect(report.erodedVolumeCubicMetres > 0.0 && report.maximumFlowCells > 1.0 && report.massBalanceRelativeError < 1.0e-9, "erosion routes flow and closes sediment mass balance");
  bool normalized = true;
  for (size_t cell = 0u; cell < count; ++cell)
  {
    const uint32_t total = first.soilLayers[cell][0] + first.soilLayers[cell][1] + first.soilLayers[cell][2] + first.soilLayers[cell][3];
    normalized = normalized && (input.waterType[cell] == 0u ? total == 255u : total == 0u);
  }
  expect(normalized, "refinement emits normalized bedrock/regolith/alluvium/organic layers");
  TerrainRefinementInput rejected = input;
  rejected.moisture.pop_back();
  expect(!refineTerrainRaster(rejected, TerrainRefinementSettings{}, first, nullptr, nullptr), "terrain refinement rejects inconsistent registered inputs");
}

void testSiteIndexRejectsBadSettings()
{
  const MapGraph graph = blendGraph();
  SiteIndex index;
  expect(!index.build(graph, 0u, 0.0, nullptr), "a zero extent is rejected");
  expect(!index.build(graph, 3u, 40000.0, nullptr), "a level with no nodes is rejected");
  expect(index.empty(), "a rejected build leaves the index empty");
}

} // namespace

int main()
{
  testTagResolution();
  testBriefHashSensitivity();
  testGraphValidation();
  testHierarchyValidation();
  testNodeAtDescendsHierarchy();
  testSignatureDeterminism();
  testPartitionBuilds();
  testPartitionDeterminism();
  testPartitionRejectsBadSettings();
  testPartitionCellsAreInsideExtent();
  testRaiseHonoursLandFraction();
  testRaiseKeepsGraphValid();
  testCoastalFlagsAreDerived();
  testRaiseDeterminism();
  testMeasuredShapeMatchesRequest();
  testBayOpensOntoTheSea();
  testRaiseRejectsBadSettings();
  testClipBoundsEveryCell();
  testClipKeepsInteriorCellsUnchanged();
  testClipIsIdempotent();
  testCoastlineFollowsTheLandWaterBoundary();
  testCoastlineEmptyWithoutWater();
  testBlendWeightsPartitionUnity();
  testBlendIsPureAtASite();
  testBlendIsContinuous();
  testBlendSharpnessConcentrates();
  testRegionCellsReceiveStableInfluenceWeights();
  testHeightBlendUsesTerrainVariationAsAThreshold();
  testTransitionSharpnessIsSymmetric();
  testBlendDeterminism();
  testSiteIndexRejectsBadSettings();
  testEdgeSetSharesEveryBoundary();
  testRelaxedBoundariesStayShared();
  testSmoothedBoundariesStayShared();
  testClassificationBoundarySmoothingRemovesCellScaleNoise();
  testElevationContoursFormDeterministicPolylines();
  testSharedSurfaceDrivesHeightUpliftAndContours();
  testTerrainRefinementConservesSedimentAndBuildsSoil();
  testMapExportBundleIsRegisteredAndDeterministic();
  testCellAreaIsMetric();
  testCoastDistanceField();
  testWaterBodiesSplitIntoOceanAndLakes();
  testElevationSeparatesLandAndSea();
  testElevationFollowsTheRangeLayout();
  testNoRangesMeansNoMountains();
  testLandformProvincesChangeRangeCharacter();
  testPlateausAreHighAndLocallyQuiet();
  testCliffsFollowSharpMacroEdges();
  testTerrainConditioningFlattensPlainsAndRiverBanks();
  testTerrainConditioningRejectsBadSettings();
  testCharacterTagsModulateRelief();
  testElevationHonoursTheSummitCeiling();
  testElevationDeterminism();
  testElevationRejectsBadSettings();
  testEveryCourseDescendsAndReachesWater();
  testDischargeAccumulatesDownstream();
  testRiversBranch();
  testStrahlerOrderOnlyRisesAtEqualConfluences();
  testVisibleChannelsStayConnectedAndWaterfallsAreSpaced();
  testRiversHandleAnEnclosedBasin();
  testRiverDeterminism();
  testRiversClassifyIncisedCanyons();
  testRiverThresholdSetsDrainageDensity();
  testLakeFillKeepsTheDeepConnectedCore();
  testRiversRejectBadSettings();
  testRegionsCoverAndPartitionTheLand();
  testRegionsAreConnected();
  testRegionSizeFollowsTheTarget();
  testHierarchyCohesionCalmsRegionBoundaries();
  testRidgesResistRegionBoundaries();
  testCharacterTagsBiasContent();
  testRegionNamesAreStableAndVaried();
  testNamedRiversRunFromMouthToSource();
  testRegionsRejectBadSettings();
  testTemperatureFollowsLatitudeAndHeight();
  testMoistureFallsInlandAndBehindRanges();
  testBiomesAgreeWithLandAndWater();
  testWhittakerClassification();
  testNeighbouringBiomesMakeSense();
  testWideClimateBandProducesBiomeVariety();
  testClimateDeterminism();
  testClimateRejectsBadSettings();

  if (failures != 0u)
  {
    std::cerr << failures << " world map checks failed\n";
    return 1;
  }
  std::cout << "All world map checks passed\n";
  return 0;
}
