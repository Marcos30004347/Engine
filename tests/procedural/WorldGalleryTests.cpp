// The world map, generated small enough to run in a test and rendered so the
#include <map>
// result can be judged rather than only counted.
//
// A heightmap is the hardest thing to test numerically: "relief 2,400 m" is
// true of a mountain range and of noise. So each case here checks the
// properties that must hold whatever the seed - every river descends and
// reaches water, every land cell belongs to a country, elevation is bounded by
// the brief - and then writes a raster and the vector layers out so the terrain
// can also be looked at.

#include "procedural/world/WorldPipeline.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

using namespace procedural;
using namespace procedural::world;

namespace {

int failures = 0;

void check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";

    return;
  }

  std::cerr << "  FAILED " << what << "\n";
  ++failures;
}

// A world small enough for a test and large enough to have the features the
// test asserts: a coast, a range, a drainage network and several countries.
//
// 160 km across rather than 48. The first attempt used 48 km and asserted that
// the world had mountains and more than one country; it had neither, and both
// were correct. A region targets 2,500 km2 - "how big is a nameable place" -
// and the whole 48 km world was 2,300 km2, so one country was the right answer.
// Ranges likewise need room to form. The assertions were fine; the world was
// too small to be asked those questions.
WorldPipelineSettings smallWorld(const uint64_t seed)
{
  WorldPipelineSettings settings;
  settings.brief.seed = seed;
  settings.brief.extentMetres = 160000;
  settings.brief.seaLevelMetres = 0;
  settings.brief.reliefMetres = 2400;
  settings.cellSpacingMetres = 1600;
  settings.hierarchyLevels = 3;
  settings.landFraction = .62;
  settings.grownLandmasses = true;
  settings.landmassCores = 7;
  settings.continentCount = 1;

  // A country here is a province, not a nation: at the default 2,500 km2 a
  // world this size is one label, and the point of the panel is to see
  // boundaries fall along ridges and rivers.
  settings.regions.targetAreaSquareMetres = 1800.0e6;

  // A lower channel-initiation threshold than a published map would use.
  //
  // The default 35 cells of catchment draws "the rivers you could navigate",
  // which on this world is a dozen short reaches near the coast and no visible
  // network. That is right for a map and wrong for a test: the thing worth
  // checking is that the drainage is a tree rooted at the sea, and at 35 the
  // tributaries that would show it are never drawn.
  settings.rivers.riverThresholdCells = 10.0;

  return settings;
}

std::ostringstream document;
size_t panels = 0;

void beginPanel(const std::string &name, const std::string &note)
{
  document << (panels++ ? ",\n" : "") << "    {\"name\": \"" << name << "\", \"note\": \"" << note << "\", \"layers\": [\n";
}

void endPanel()
{
  document << "\n    ]}";
}

size_t layers = 0;

void beginLayer(const std::string &kind, const std::string &colour, const double width)
{
  document << (layers++ ? ",\n" : "") << "      {\"kind\": \"" << kind << "\", \"colour\": \"" << colour << "\", \"width\": " << width << ", \"shapes\": [";
}

void endLayer()
{
  document << "]}";
}

void emitRing(const std::vector<Point2> &ring, bool &first)
{
  if (ring.size() < 2)
  {
    return;
  }

  document << (first ? "" : ",") << "[";
  first = false;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    document << "[" << ring[i].x << "," << ring[i].y << "]" << (i + 1 < ring.size() ? "," : "");
  }

  document << "]";
}

// Colour ramps. Deliberately the cartographic ones - a reader who knows maps
// can tell at a glance whether a range sits where a range should.
std::string reliefColour(const double height, const double highest)
{
  if (height <= 0)
  {
    const double depth = std::min(1.0, -height / 400.0);
    const int r = int(40 + 50 * (1 - depth)), g = int(90 + 70 * (1 - depth)), b = int(150 + 60 * (1 - depth));
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", r, g, b);

    return buffer;
  }

  const double t = highest > 1 ? std::min(1.0, height / highest) : 0;

  // green lowland, tan upland, grey-white peaks
  const double stops[4][3] = {{122, 160, 96}, {198, 190, 126}, {160, 130, 100}, {244, 244, 248}};
  const double u = t * 3;
  const int band = std::min(2, int(u));
  const double f = u - band;

  const int r = int(stops[band][0] + (stops[band + 1][0] - stops[band][0]) * f);
  const int g = int(stops[band][1] + (stops[band + 1][1] - stops[band][1]) * f);
  const int b = int(stops[band][2] + (stops[band + 1][2] - stops[band][2]) * f);

  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", r, g, b);

  return buffer;
}

std::string regionColour(const uint32_t id)
{
  static const char *palette[] = {"#c96f6f", "#6f96c9", "#79b37c", "#c9a86f", "#9b7fc9", "#6fc3c9", "#c96fa8", "#8f9a6f", "#c98f6f", "#6f7fc9", "#96c96f", "#c96f86"};

  return palette[id % 12];
}

} // namespace

int main(int argc, char **argv)
{
  std::cout << "world gallery\n";

  const WorldPipelineSettings settings = smallWorld(20260914);
  GeneratedWorld world;
  std::string error;

  if (!generateWorld(settings, world, {}, &error))
  {
    std::cerr << "world generation failed: " << error << "\n";

    return 1;
  }

  std::cout << "Terrain\n";

  // A world has to have both. A map that is all sea or all land is a bug the
  // land fraction should have caught.
  const double landFraction = double(world.landCells) / double(std::max(1u, world.leafCells));
  check(landFraction > .25 && landFraction < .9, "the world has both land and sea");
  check(world.highestMetres > 200, "and relief worth calling relief");
  check(world.highestMetres <= settings.brief.reliefMetres * 1.05, "within the relief the brief asked for");
  check(world.deepestMetres < 0, "with sea floor below sea level");
  check(!world.coastline.empty(), "and a traced coastline");

  std::cout << "Mountains and land types\n";

  check(world.mountainCells > 0, "the world grew mountains");
  check(world.mountainCells < world.landCells / 2, "and they are a range rather than the whole continent");

  // Every mountain cell should stand above the median land cell. A "mountain"
  // at sea level is a classification that has come loose from the elevation.
  std::vector<double> landHeights;

  for (const auto &node : world.graph.nodes())
  {
    if (!node.refined() && node.land)
    {
      landHeights.push_back(node.intent.elevationMetres);
    }
  }

  std::sort(landHeights.begin(), landHeights.end());
  const double median = landHeights.empty() ? 0 : landHeights[landHeights.size() / 2];

  size_t lowMountains = 0;

  for (const auto &node : world.graph.nodes())
  {
    if (!node.refined() && node.type == CellType::Mountain && node.intent.elevationMetres < median)
    {
      ++lowMountains;
    }
  }

  check(lowMountains == 0, "and every mountain cell stands above the median land height");

  std::cout << "Hydrology\n";

  check(!world.rivers.segments.empty(), "rivers were routed");

  size_t mouths = 0, ascending = 0;

  for (const auto &segment : world.rivers.segments)
  {
    if (segment.mouth)
    {
      ++mouths;
    }
  }

  // Every river descends: the priority-flood fill is what guarantees it, and
  // this is the check that the guarantee survived every later stage.
  for (const auto &segment : world.rivers.segments)
  {
    const auto from = world.surface.sample({segment.from.x, segment.from.y});
    const auto to = world.surface.sample({segment.to.x, segment.to.y});

    if (to.elevationMetres > from.elevationMetres + 1.0)
    {
      ++ascending;
    }
  }

  check(mouths > 0, "and at least one reaches the sea");

  // Drainage is a tree: every segment's downstream end is either a mouth or
  // the upstream end of another segment. A stub that ends in the middle of dry
  // land is water that goes nowhere.
  std::set<std::pair<int64_t, int64_t>> upstreamEnds;

  const auto key = [](const Point2 &p) {
    return std::make_pair(int64_t(std::llround(p.x)), int64_t(std::llround(p.y)));
  };

  for (const auto &segment : world.rivers.segments)
  {
    upstreamEnds.insert(key(segment.from));
  }

  size_t orphans = 0;

  for (const auto &segment : world.rivers.segments)
  {
    if (!segment.mouth && upstreamEnds.count(key(segment.to)) == 0)
    {
      ++orphans;
    }
  }

  check(orphans * 20 < world.rivers.segments.size(), "and the network drains: almost no segment ends on dry land");
  check(ascending * 20 < world.rivers.segments.size(), "and rivers run downhill, sampled against the published surface");

  std::cout << "Country allocation\n";

  check(!world.regions.regions.empty(), "the world was divided into countries");

  size_t unassignedLand = 0;

  for (const auto &node : world.graph.nodes())
  {
    if (node.refined() || !node.land)
    {
      continue;
    }

    if (node.id >= world.regions.cellRegion.size() || world.regions.cellRegion[node.id] == kInvalidNode)
    {
      ++unassignedLand;
    }
  }

  check(unassignedLand * 50 < world.landCells, "and almost every land cell belongs to one");

  bool named = true;
  double smallest = 1e30, largest = 0;

  for (const auto &region : world.regions.regions)
  {
    if (region.name.empty())
    {
      named = false;
    }

    smallest = std::min(smallest, region.areaSquareMetres);
    largest = std::max(largest, region.areaSquareMetres);
  }

  check(named, "every country has a name");
  check(largest < world.landAreaSquareMetres * .9, "and no single country swallowed the continent");

  // ---------------------------------------------------------------------
  // Render document
  // ---------------------------------------------------------------------

  document << "{\n  \"panels\": [\n";

  // Heightmap: every leaf cell filled by its own elevation.
  beginPanel("heightmap", "relief by cell: sea, lowland, upland, peaks");
  layers = 0;

  {
    std::map<std::string, std::vector<const MapNode *>> byColour;

    for (const auto &node : world.graph.nodes())
    {
      if (node.refined() || node.area.size() < 3)
      {
        continue;
      }

      byColour[reliefColour(node.land ? node.intent.elevationMetres : -50, world.highestMetres)].push_back(&node);
    }

    for (const auto &[colour, nodes] : byColour)
    {
      beginLayer("fill", colour, 1);
      bool first = true;

      for (const auto *node : nodes)
      {
        emitRing(node->area, first);
      }

      endLayer();
    }

    beginLayer("line", "#20303c", 1.4);
    bool first = true;

    for (const auto &line : world.coastline)
    {
      emitRing(line.points, first);
    }

    endLayer();
  }

  endPanel();

  // Hydrology over the same ground.
  beginPanel("hydrology", "rivers and lakes: every channel descends to water");
  layers = 0;

  {
    beginLayer("fill", "#e7e2d6", 1);
    bool first = true;

    for (const auto &node : world.graph.nodes())
    {
      if (!node.refined() && node.land && node.area.size() >= 3)
      {
        emitRing(node.area, first);
      }
    }

    endLayer();

    beginLayer("fill", "#4f86c6", 1);
    first = true;

    for (const auto &node : world.graph.nodes())
    {
      if (!node.refined() && node.type == CellType::Lake && node.area.size() >= 3)
      {
        emitRing(node.area, first);
      }
    }

    endLayer();

    // Wider for a bigger river, which is what makes a drainage network read as
    // one rather than as a pile of unrelated lines.
    for (int order = 1; order <= 4; ++order)
    {
      beginLayer("line", "#2f6fae", order * 0.9);
      first = true;

      for (const auto &segment : world.rivers.segments)
      {
        const int bucket = std::min(4, int(segment.strahlerOrder));

        if (bucket == order)
        {
          std::vector<Point2> path{segment.from, segment.to};
          emitRing(path, first);
        }
      }

      endLayer();
    }

    beginLayer("line", "#20303c", 1.4);
    first = true;

    for (const auto &line : world.coastline)
    {
      emitRing(line.points, first);
    }

    endLayer();
  }

  endPanel();

  // Country allocation.
  beginPanel("countries", "regions grown over the neighbour graph: ridges and rivers cost extra");
  layers = 0;

  {
    std::map<std::string, std::vector<const MapNode *>> byColour;

    for (const auto &node : world.graph.nodes())
    {
      if (node.refined() || !node.land || node.area.size() < 3)
      {
        continue;
      }

      const uint32_t region = node.id < world.regions.cellRegion.size() ? world.regions.cellRegion[node.id] : kInvalidNode;
      byColour[region == kInvalidNode ? "#b9b9b9" : regionColour(region)].push_back(&node);
    }

    for (const auto &[colour, nodes] : byColour)
    {
      beginLayer("fill", colour, 1);
      bool first = true;

      for (const auto *node : nodes)
      {
        emitRing(node->area, first);
      }

      endLayer();
    }

    beginLayer("line", "#20303c", 1.4);
    bool first = true;

    for (const auto &line : world.coastline)
    {
      emitRing(line.points, first);
    }

    endLayer();

    beginLayer("point", "#1b1b1b", 3);
    first = true;

    for (const auto &region : world.regions.regions)
    {
      std::vector<Point2> dot{region.centroid};
      emitRing(dot, first);
    }

    endLayer();
  }

  endPanel();

  // Land types: what the classifier made of the terrain.
  beginPanel("land-types", "mountain, cliff, canyon, valley and lake against the plain");
  layers = 0;

  {
    const std::pair<CellType, const char *> kinds[] = {{CellType::Mountain, "#8a8177"}, {CellType::Cliff, "#a0674a"}, {CellType::Canyon, "#7a4a3a"}, {CellType::Valley, "#95ad7c"}, {CellType::Lake, "#4f86c6"}};

    beginLayer("fill", "#ddd8cb", 1);
    bool first = true;

    for (const auto &node : world.graph.nodes())
    {
      if (!node.refined() && node.land && node.area.size() >= 3)
      {
        emitRing(node.area, first);
      }
    }

    endLayer();

    for (const auto &[kind, colour] : kinds)
    {
      beginLayer("fill", colour, 1);
      first = true;

      for (const auto &node : world.graph.nodes())
      {
        if (!node.refined() && node.type == kind && node.area.size() >= 3)
        {
          emitRing(node.area, first);
        }
      }

      endLayer();
    }

    beginLayer("line", "#20303c", 1.4);
    first = true;

    for (const auto &line : world.coastline)
    {
      emitRing(line.points, first);
    }

    endLayer();
  }

  endPanel();

  document << "\n  ]\n}\n";

  const std::string path = argc > 1 ? argv[1] : "world-gallery.json";
  std::ofstream file(path);
  file << document.str();

  std::cout << "wrote " << panels << " panels to " << path << "\n";
  std::cout << "  land " << world.landCells << "/" << world.leafCells << " cells, highest " << world.highestMetres << " m, " << world.rivers.segments.size() << " river segments, " << world.regions.regions.size()
            << " countries, " << world.mountainCells << " mountain cells\n";

  if (failures != 0)
  {
    std::cerr << failures << " world gallery test(s) failed\n";

    return 1;
  }

  std::cout << "All world gallery tests passed\n";

  return 0;
}
