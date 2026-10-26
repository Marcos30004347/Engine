// Every generator, in one chain, on ground the world actually produced.
//
// The other gallery runs each generator on a shape chosen to be easy to judge.
// This one gives up that luxury: it generates a world, picks a real land cell
// out of it - whatever shape the Voronoi partition happened to make - and runs
// the settlement hierarchy on it against the world's own terrain.
//
// That is the integration the separate tests cannot cover. A generator that
// quietly assumes its input is convex, axis aligned, a sensible size, or on
// flat ground passes every isolated test and fails here.

#include "procedural/world/WorldPipeline.hpp"
#include "procedural/world/hierarchy/Generators.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

using namespace procedural;
using namespace procedural::world;
namespace hier = procedural::world::hierarchy;

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

std::ostringstream document;
size_t panels = 0, layers = 0;

void beginPanel(const std::string &name, const std::string &note)
{
  document << (panels++ ? ",\n" : "") << "    {\"name\": \"" << name << "\", \"note\": \"" << note << "\", \"layers\": [\n";
  layers = 0;
}

void endPanel()
{
  document << "\n    ]}";
}

void beginLayer(const std::string &kind, const std::string &colour, const double width)
{
  document << (layers++ ? ",\n" : "") << "      {\"kind\": \"" << kind << "\", \"colour\": \"" << colour << "\", \"width\": " << width << ", \"shapes\": [";
}

void endLayer()
{
  document << "]}";
}

void emitMetres(const std::vector<Point2> &ring, bool &first)
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

void emitGrid(const geom2::Ring &ring, bool &first)
{
  std::vector<Point2> metres;

  for (const auto &p : ring)
  {
    metres.push_back({geom2::toMetres(p.x), geom2::toMetres(p.y)});
  }

  emitMetres(metres, first);
}

} // namespace

int main(int argc, char **argv)
{
  std::cout << "integration gallery\n";

  // ------------------------------------------------------------------
  // 1. A world
  // ------------------------------------------------------------------

  WorldPipelineSettings settings;
  settings.brief.seed = 4271;
  settings.brief.extentMetres = 120000;
  settings.brief.reliefMetres = 1800;
  settings.cellSpacingMetres = 1500;
  settings.hierarchyLevels = 3;
  settings.landFraction = .6;
  settings.grownLandmasses = true;
  settings.landmassCores = 6;
  settings.rivers.riverThresholdCells = 12;
  settings.regions.targetAreaSquareMetres = 1500.0e6;

  GeneratedWorld world;
  std::string error;

  if (!generateWorld(settings, world, {}, &error))
  {
    std::cerr << "world generation failed: " << error << "\n";

    return 1;
  }

  std::cout << "Stage 1: the world\n";
  check(world.landCells > 100 && !world.rivers.segments.empty() && !world.regions.regions.empty(), "a world with land, rivers and countries");

  // ------------------------------------------------------------------
  // 2. A real cell out of it
  // ------------------------------------------------------------------
  //
  // An inland cell with some relief across it, not the flattest one.
  //
  // The first version picked the flattest, which is where a settlement would
  // really go - and then the slope rule refused nothing, so the panel that
  // exists to show the rule working showed a rule doing nothing. A hillside is
  // the more testing case: part of it should be buildable and part of it
  // should not.
  //
  // Nothing about the cell's shape is chosen. It is whatever the partition made.

  constexpr double kWantedRelief = 90.0;

  const MapNode *chosen = nullptr;
  double bestRelief = 1e30;
  double chosenDistance = 1e30;

  for (const auto &node : world.graph.nodes())
  {
    if (node.refined() || !node.land || node.coastal || node.area.size() < 4 || node.type == CellType::Lake)
    {
      continue;
    }

    double lowest = 1e30, highest = -1e30;

    for (const auto &p : node.area)
    {
      const double h = world.surface.sample({p.x, p.y}).elevationMetres;
      lowest = std::min(lowest, h);
      highest = std::max(highest, h);
    }

    const double relief = highest - lowest;

    if (std::abs(relief - kWantedRelief) < chosenDistance)
    {
      chosenDistance = std::abs(relief - kWantedRelief);
      bestRelief = relief;
      chosen = &node;
    }
  }

  std::cout << "Stage 2: a real cell\n";
  check(chosen != nullptr, "the world offered an inland cell to settle");

  if (chosen == nullptr)
  {
    return 1;
  }

  geom2::Ring site;

  if (!geom2::toRing(
          [&] {
            std::vector<Point2> pts;

            for (const auto &p : chosen->area)
            {
              pts.push_back(p);
            }

            return pts;
          }(),
          site))
  {
    std::cerr << "cell outside the representable grid\n";

    return 1;
  }

  if (!geom2::counterClockwise(site))
  {
    geom2::reverse(site);
  }

  check(geom2::ringFault(site).empty(), "and its outline is a valid ring");
  check(geom2::areaMetres(site) > 100000, "of a size worth settling");

  std::cout << "  cell " << chosen->id << ": " << site.size() << " vertices, " << geom2::areaMetres(site) * 1e-6 << " km2, relief " << bestRelief << " m, convex=" << (geom2::convex(site) ? "yes" : "no") << "\n";

  // ------------------------------------------------------------------
  // 3. The settlement hierarchy on it, against the world's own terrain
  // ------------------------------------------------------------------

  hier::Registry registry;
  registry.bind(std::make_shared<hier::GridStreets>(hier::GridStreets::Settings{260, 9, .4}), &error);
  registry.bind(std::make_shared<hier::RadialStreets>(), &error);
  registry.bind(std::make_shared<hier::StripLots>(hier::StripLots::Settings{4200, .35}), &error);
  registry.bind(std::make_shared<hier::RingLots>(), &error);
  registry.bind(std::make_shared<hier::HouseFootprints>(hier::HouseFootprints::Settings{6, 400, 3, .3}), &error);
  registry.bind(std::make_shared<hier::SlopeField>(hier::SlopeField::Settings{.06}), &error);

  hier::RegionTree tree;
  hier::Region root = hier::makeRegion(tree.nextRegionId(), hier::kNoRegion, [&] {
    geom2::Polygon p;
    p.outer = site;

    return p;
  }());
  root.cover = hier::Cover::Urban;
  root.producer = "integration";
  const hier::RegionId rootId = tree.add(std::move(root));

  hier::Context context;
  context.worldSeed = settings.brief.seed;

  // The terrain the generators see is the world's own published surface. Not a
  // flat plane, not a synthetic hill: whatever the map stages actually left.
  context.terrain = [&world](const double x, const double y) {
    return world.surface.sample({x, y}).elevationMetres;
  };

  hier::Plan plan;
  plan.policy.defaults[hier::Role::Streets] = "streets.grid";
  plan.policy.defaults[hier::Role::Lots] = "lots.strip";
  plan.policy.defaults[hier::Role::Terrain] = "terrain.slope";
  plan.policy.defaults[hier::Role::Buildings] = "buildings.setback";
  plan.steps.push_back({hier::Role::Streets, {}, 0});
  plan.steps.push_back(
      {hier::Role::Lots,
       [](const hier::Region &r) {
         return r.cover != hier::Cover::Road;
       },
       0});
  plan.steps.push_back({hier::Role::Terrain, {}, 0});
  plan.steps.push_back(
      {hier::Role::Buildings,
       [](const hier::Region &r) {
         return r.cover != hier::Cover::Road && r.attributes.buildable;
       },
       0});

  hier::Composer composer(registry, context);
  hier::Ledger ledger;
  hier::RunStats stats;

  std::cout << "Stage 3: the settlement\n";
  check(composer.run(plan, tree, rootId, ledger, stats, &error), "the whole chain runs on a real cell: " + error);

  double leaves = 0;
  size_t roads = 0, lots = 0, steep = 0;

  for (const auto &region : tree.regions())
  {
    if (!tree.childrenOf(region.id).empty())
    {
      continue;
    }

    leaves += region.areaMetres();

    if (region.cover == hier::Cover::Road)
    {
      ++roads;
    }
    else
    {
      ++lots;
    }

    if (!region.attributes.buildable)
    {
      ++steep;
    }
  }

  check(std::abs(leaves - geom2::areaMetres(site)) < 1.0, "and its leaves tile the cell the world gave it");
  check(roads > 0 && lots > 4, "with streets and lots");
  check(!composer.objects().empty(), "and buildings on the lots");

  bool valid = true;

  for (const auto &region : tree.regions())
  {
    if (!geom2::polygonFault(region.boundary).empty())
    {
      valid = false;
    }
  }

  check(valid, "every region produced is a valid polygon");

  // The slope came from the real surface, so it must vary: a constant slope
  // means the terrain callback was never really consulted.
  double lowestSlope = 1e30, highestSlope = 0;

  for (const auto &region : tree.regions())
  {
    if (tree.childrenOf(region.id).empty() && region.attributes.slope > 0)
    {
      lowestSlope = std::min(lowestSlope, region.attributes.slope);
      highestSlope = std::max(highestSlope, region.attributes.slope);
    }
  }

  check(highestSlope > 0 && highestSlope > lowestSlope * 1.5, "the terrain field read real ground, not a flat plane");

  bool placedWell = true;

  for (const auto &object : composer.objects())
  {
    const hier::Region *region = tree.find(object.region);

    if (region == nullptr || region->cover == hier::Cover::Road || !region->attributes.buildable)
    {
      placedWell = false;
    }
  }

  check(placedWell, "and nothing was built on a road or on ground too steep for it");

  // The point of choosing a hillside: the rule must actually refuse something,
  // or the panel showing it is a panel of nothing.
  check(steep > 0, "and the slope rule refused part of the hillside");
  check(steep < lots, "but not all of it");

  std::cout << "  " << roads << " street regions, " << lots << " lots, " << steep << " too steep, " << composer.objects().size() << " buildings\n";

  // ------------------------------------------------------------------
  // Render
  // ------------------------------------------------------------------

  document << "{\n  \"panels\": [\n";

  beginPanel("1-world", "the world: land, rivers and the cell chosen to settle");
  {
    beginLayer("fill", "#e7e2d6", 1);
    bool first = true;

    for (const auto &node : world.graph.nodes())
    {
      if (!node.refined() && node.land && node.area.size() >= 3)
      {
        emitMetres(node.area, first);
      }
    }

    endLayer();

    beginLayer("line", "#2f6fae", 1.1);
    first = true;

    for (const auto &segment : world.rivers.segments)
    {
      std::vector<Point2> path{segment.from, segment.to};
      emitMetres(path, first);
    }

    endLayer();

    beginLayer("fill", "#c4453a", 1);
    first = true;
    emitMetres(chosen->area, first);
    endLayer();

    beginLayer("line", "#20303c", 1.4);
    first = true;

    for (const auto &line : world.coastline)
    {
      emitMetres(line.points, first);
    }

    endLayer();
  }

  endPanel();

  const auto settlementPanel = [&](const std::string &name, const std::string &note, const bool withLots, const bool withBuildings, const bool bySlope) {
    beginPanel(name, note);

    beginLayer("fill", "#e6ded2", 1);
    bool first = true;
    emitGrid(site, first);
    endLayer();

    if (withLots)
    {
      if (bySlope)
      {
        // Buildable ground against ground the slope rule refused.
        for (const bool buildable : {true, false})
        {
          beginLayer("fill", buildable ? "#cfd8c9" : "#c2a6a0", 1);
          first = true;

          for (const auto &region : tree.regions())
          {
            if (!tree.childrenOf(region.id).empty() || region.cover == hier::Cover::Road || region.attributes.buildable != buildable)
            {
              continue;
            }

            emitGrid(region.boundary.outer, first);
          }

          endLayer();
        }
      }
      else
      {
        beginLayer("fill", "#cfd8c9", 1);
        first = true;

        for (const auto &region : tree.regions())
        {
          if (tree.childrenOf(region.id).empty() && region.cover != hier::Cover::Road)
          {
            emitGrid(region.boundary.outer, first);
          }
        }

        endLayer();
      }

      beginLayer("line", "#6b7a63", 0.7);
      first = true;

      for (const auto &region : tree.regions())
      {
        if (tree.childrenOf(region.id).empty() && region.cover != hier::Cover::Road)
        {
          emitGrid(region.boundary.outer, first);
        }
      }

      endLayer();
    }

    beginLayer("fill", "#9c9384", 1);
    first = true;

    for (const auto &region : tree.regions())
    {
      if (tree.childrenOf(region.id).empty() && region.cover == hier::Cover::Road)
      {
        emitGrid(region.boundary.outer, first);
      }
    }

    endLayer();

    if (withBuildings)
    {
      beginLayer("fill", "#8c5a40", 1);
      first = true;

      for (const auto &object : composer.objects())
      {
        emitGrid(object.footprint.outer, first);
      }

      endLayer();
    }

    beginLayer("line", "#2f4858", 1.6);
    first = true;
    emitGrid(site, first);
    endLayer();

    endPanel();
  };

  settlementPanel("2-streets", "the cell cut by streets: roads are regions, not lines", false, false, false);
  settlementPanel("3-lots", "blocks allocated into lots", true, false, false);
  settlementPanel("4-terrain", "the slope rule on real ground: red is refused", true, false, true);
  settlementPanel("5-buildings", "buildings on the lots the terrain allowed", true, true, true);

  document << "\n  ]\n}\n";

  const std::string path = argc > 1 ? argv[1] : "integration-gallery.json";
  std::ofstream file(path);
  file << document.str();

  std::cout << "wrote " << panels << " panels to " << path << "\n";

  if (failures != 0)
  {
    std::cerr << failures << " integration gallery test(s) failed\n";

    return 1;
  }

  std::cout << "All integration gallery tests passed\n";

  return 0;
}
