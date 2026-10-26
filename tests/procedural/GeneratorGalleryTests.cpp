// Every generator, run on a shape simple enough to judge by eye.
//
// The generators are hard to test by numbers alone: "142 lots" is true of a
// good subdivision and of a bad one. So each case here runs one generator on a
// deliberately plain input - a square, a hexagon, an L, a concave comb - checks
// the invariants that must hold whatever the shape, and writes the result out
// as JSON for the renderer, so the geometry can also be looked at.
//
// The shapes are plain on purpose. A hexagon has no right angles to accidentally
// satisfy an axis-aligned assumption; an L and a comb have reflex corners; a
// square is the case every implementation gets right, so it catches nothing and
// is worth exactly one panel.

#include "procedural/geom2/Offset.hpp"
#include "procedural/geom2/Partition.hpp"
#include "procedural/geom2/Sample.hpp"
#include "procedural/world/hierarchy/Generators.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace procedural;
using namespace procedural::geom2;
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

// ---------------------------------------------------------------------------
// The render document.
//
// Deliberately dumb: a list of panels, each a list of layers of filled
// polygons, lines and points. The renderer knows nothing about generators, so
// a new case never needs a new renderer.
// ---------------------------------------------------------------------------

struct Layer
{
  std::string kind; // fill | line | point
  std::string colour;
  double width = 1;
  std::vector<std::vector<Point>> shapes;
};

struct Panel
{
  std::string name;
  std::string note;
  std::vector<Layer> layers;
};

std::vector<Panel> gallery;

Panel &panel(const std::string &name, const std::string &note)
{
  gallery.push_back({name, note, {}});

  return gallery.back();
}

void fill(Panel &p, const std::string &colour, const std::vector<Ring> &rings)
{
  p.layers.push_back({"fill", colour, 1, rings});
}

void fillPolys(Panel &p, const std::string &colour, const std::vector<Polygon> &polys)
{
  std::vector<Ring> rings;

  for (const auto &polygon : polys)
  {
    rings.push_back(polygon.outer);
  }

  p.layers.push_back({"fill", colour, 1, rings});

  std::vector<Ring> holes;

  for (const auto &polygon : polys)
  {
    for (const auto &hole : polygon.holes)
    {
      holes.push_back(hole);
    }
  }

  if (!holes.empty())
  {
    p.layers.push_back({"hole", colour, 1, holes});
  }
}

void stroke(Panel &p, const std::string &colour, const double width, const std::vector<std::vector<Point>> &paths)
{
  p.layers.push_back({"line", colour, width, paths});
}

void dots(Panel &p, const std::string &colour, const double radius, const std::vector<Point> &points)
{
  std::vector<std::vector<Point>> wrapped;

  for (const auto &q : points)
  {
    wrapped.push_back({q});
  }

  p.layers.push_back({"point", colour, radius, wrapped});
}

// ---------------------------------------------------------------------------
// The shapes every case is judged on.
// ---------------------------------------------------------------------------

Point at(const double x, const double y)
{
  return {toUnit(x), toUnit(y)};
}

Ring square(const double size)
{
  return {at(0, 0), at(size, 0), at(size, size), at(0, size)};
}

// A regular hexagon. No right angles, so nothing here can pass by accidentally
// agreeing with an axis.
Ring hexagon(const double radius, const double cx, const double cy)
{
  Ring ring;

  for (int k = 0; k < 6; ++k)
  {
    const double a = 3.14159265358979323846 * (2.0 * double(k) / 6.0 + 1.0 / 6.0);
    ring.push_back(at(cx + std::cos(a) * radius, cy + std::sin(a) * radius));
  }

  return ring;
}

// One reflex corner.
Ring ell(const double size)
{
  return {at(0, 0), at(size, 0), at(size, size * .4), at(size * .4, size * .4), at(size * .4, size), at(0, size)};
}

// Three reflex pairs: the shape that catches a subdivision assuming convexity.
Ring comb(const double size)
{
  const double t = size / 5;

  return {at(0, 0), at(size, 0), at(size, t * 3), at(t * 4, t * 3), at(t * 4, t), at(t * 3, t), at(t * 3, t * 3), at(t * 2, t * 3), at(t * 2, t), at(t, t), at(t, t * 3), at(0, t * 3)};
}

Polygon asPolygon(Ring ring)
{
  Polygon p;
  p.outer = std::move(ring);

  return p;
}

// How closely two areas of these shapes may be expected to agree.
//
// Quantisation moves each vertex by at most half a micrometre, so the error in
// an area is bounded by the perimeter times that - on the order of 1e-4 m2 for
// the hundred-metre shapes here. A square millimetre is comfortably above that
// and far below anything a generator could mean.
constexpr double kAreaTolerance = 1.0e-3;

double totalArea(const std::vector<Polygon> &polys)
{
  double area = 0;

  for (const auto &p : polys)
  {
    area += areaMetres(p);
  }

  return area;
}

bool allValid(const std::vector<Polygon> &polys)
{
  for (const auto &p : polys)
  {
    if (!polygonFault(p).empty())
    {
      return false;
    }
  }

  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

namespace {

void booleanCases()
{
  std::cout << "Booleans on plain shapes\n";

  const Operand hex = toOperand(hexagon(40, 50, 50));
  const Operand box = toOperand(square(60));

  std::vector<Polygon> un, in, diff;
  std::string error;

  check(unionOf({hex, box}, un, &error) && allValid(un), "hexagon union square: " + error);
  check(intersectionOf({hex, box}, in, &error) && allValid(in), "hexagon intersect square");
  check(differenceOf(hex, {box}, diff, &error) && allValid(diff), "hexagon minus square");

  // The one statement that fails if any face is counted twice or dropped.
  check(std::abs(totalArea(un) + totalArea(in) - (areaMetres(hexagon(40, 50, 50)) + areaMetres(square(60)))) < kAreaTolerance, "areas are conserved across union and intersection");

  auto &p = panel("boolean", "hexagon and square: union, intersection, difference");
  fill(p, "#dbe7f3", {square(60)});
  fill(p, "#e8dcf0", {hexagon(40, 50, 50)});
  fillPolys(p, "#7fa8d4", in);
  stroke(p, "#2f4858", 2, {square(60), hexagon(40, 50, 50)});
}

void offsetCases()
{
  std::cout << "Offsetting\n";

  auto &p = panel("offset", "comb grown and shrunk: notches close, arms part");

  const Polygon subject = asPolygon(comb(60));
  std::vector<Polygon> grown, shrunk;
  std::string error;

  check(offsetPolygon(subject, 3.0, {}, grown, &error) && allValid(grown), "comb grows: " + error);
  check(offsetPolygon(subject, -2.0, {}, shrunk, &error) && allValid(shrunk), "comb shrinks: " + error);
  check(totalArea(grown) > areaMetres(subject) && totalArea(shrunk) < areaMetres(subject), "growing adds area and shrinking removes it");

  // A comb shrunk past half its tooth width loses its teeth, which is a
  // topology change and the case a per-edge offset folds over.
  std::vector<Polygon> far;
  check(offsetPolygon(subject, -7.0, {}, far, &error) && allValid(far), "comb shrinks past its tooth width");
  check(far.size() <= 1, "and its teeth are gone rather than folded");

  fillPolys(p, "#cfe3d0", grown);
  fillPolys(p, "#8fbf9a", {subject});
  fillPolys(p, "#3f7d54", shrunk);
  stroke(p, "#20402c", 1.5, {subject.outer});
}

void riverCases()
{
  std::cout << "A river dividing a cell\n";

  // The case the whole non-convex baseline exists for: a meandering corridor
  // crossing a cell, leaving two concave banks and a water ribbon between them.
  const Polygon cell = asPolygon(hexagon(45, 50, 50));

  const std::vector<Point> centreline{at(-10, 30), at(20, 38), at(45, 24), at(70, 40), at(105, 34)};

  std::vector<Polygon> water;
  std::string error;

  check(offsetPolyline(centreline, 3.5, {}, water, &error) && allValid(water), "the river becomes a ribbon: " + error);

  Operand cellOperand = toOperand(cell);
  Operand waterOperand;

  for (const auto &w : water)
  {
    for (const auto &ring : toOperand(w).rings)
    {
      waterOperand.rings.push_back(ring);
    }
  }

  std::vector<Polygon> banks, wet;

  check(differenceOf(cellOperand, {waterOperand}, banks, &error) && allValid(banks), "the river cuts the cell: " + error);
  check(banks.size() == 2, "into exactly two banks");
  check(intersectionOf({cellOperand, waterOperand}, wet, &error) && allValid(wet), "and the wet ground is its own region");
  check(std::abs(totalArea(banks) + totalArea(wet) - areaMetres(cell)) < kAreaTolerance, "bank and water areas sum to the cell");

  bool concave = false;

  for (const auto &b : banks)
  {
    if (!convex(b.outer))
    {
      concave = true;
    }
  }

  check(concave, "and at least one bank is concave, as a real bank is");

  auto &p = panel("river", "a meander cuts a hexagonal cell into two concave banks");
  fillPolys(p, "#d8cfa8", banks);
  fillPolys(p, "#4f86c6", wet);
  stroke(p, "#2f4858", 1.5, {cell.outer});
  stroke(p, "#274b73", 1, {centreline});
}

void partitionCases()
{
  std::cout << "Voronoi and power diagrams\n";

  const Polygon region = asPolygon(hexagon(45, 50, 50));

  std::vector<Site> sites;

  for (int k = 0; k < 7; ++k)
  {
    const double a = 3.14159265358979323846 * 2.0 * double(k) / 7.0;
    sites.push_back({at(50 + std::cos(a) * 24, 50 + std::sin(a) * 24), 0});
  }

  sites.push_back({at(50, 50), 0});

  std::vector<PartitionCell> cells;
  std::string error;

  check(partitionRegion(region, sites, {}, cells, &error) && !cells.empty(), "sites partition a hexagon: " + error);

  double total = 0;

  for (const auto &c : cells)
  {
    total += areaMetres(c.polygon);
  }

  check(std::abs(total - areaMetres(region)) < kAreaTolerance, "and the cells tile it exactly");

  auto &p = panel("voronoi", "eight sites over a hexagon: cells tile it exactly");

  for (size_t i = 0; i < cells.size(); ++i)
  {
    static const char *palette[] = {"#e8c9b0", "#c9d8e8", "#d8e8c9", "#e8d8c9", "#c9e8e0", "#e0c9e8", "#e8e0c9", "#c9c9e8"};
    fillPolys(p, palette[i % 8], {cells[i].polygon});
  }

  for (const auto &c : cells)
  {
    stroke(p, "#4a5a68", 1, {c.polygon.outer});
  }

  std::vector<Point> centres;

  for (const auto &site : sites)
  {
    centres.push_back(site.position);
  }

  dots(p, "#2f4858", 2.5, centres);

  // The weighted diagram: the same sites, one of them twice as hungry.
  std::vector<Site> weighted = sites;
  weighted.back().weight = 600;

  std::vector<PartitionCell> weightedCells;

  check(partitionRegion(region, weighted, {}, weightedCells, &error), "a weighted site claims more ground: " + error);

  double centreArea = 0, plainCentreArea = 0;

  for (const auto &c : weightedCells)
  {
    if (c.site == weighted.size() - 1)
    {
      centreArea += areaMetres(c.polygon);
    }
  }

  for (const auto &c : cells)
  {
    if (c.site == sites.size() - 1)
    {
      plainCentreArea += areaMetres(c.polygon);
    }
  }

  check(centreArea > plainCentreArea, "so a district's cell can be sized by its demand");

  auto &q = panel("power", "the same sites, the centre one weighted: its cell grows");

  for (size_t i = 0; i < weightedCells.size(); ++i)
  {
    static const char *palette[] = {"#e8c9b0", "#c9d8e8", "#d8e8c9", "#e8d8c9", "#c9e8e0", "#e0c9e8", "#e8e0c9", "#b03030"};
    fillPolys(q, palette[weightedCells[i].site % 8], {weightedCells[i].polygon});
  }

  for (const auto &c : weightedCells)
  {
    stroke(q, "#4a5a68", 1, {c.polygon.outer});
  }

  dots(q, "#2f4858", 2.5, centres);
}

// Runs a settlement plan over one region and returns the tree, so the street,
// block and lot cases can each render the same run at a different depth.
bool runSettlement(const Ring &outline, const std::string &streets, const std::string &lots, hier::RegionTree &tree, hier::RegionId &root, std::vector<hier::Object> &objects, std::string &error)
{
  hier::Registry registry;
  registry.bind(std::make_shared<hier::GridStreets>(hier::GridStreets::Settings{34, 2.5, 0.35}), &error);
  registry.bind(std::make_shared<hier::RadialStreets>(hier::RadialStreets::Settings{5, .5, 2.5}), &error);
  registry.bind(std::make_shared<hier::StripLots>(hier::StripLots::Settings{260, .3}), &error);
  registry.bind(std::make_shared<hier::RingLots>(hier::RingLots::Settings{10, 130}), &error);
  registry.bind(std::make_shared<hier::BisectionLots>(hier::BisectionLots::craftsmen()), &error);
  registry.bind(std::make_shared<hier::HouseFootprints>(), &error);
  registry.bind(std::make_shared<hier::SlopeField>(), &error);

  hier::Region seed = hier::makeRegion(tree.nextRegionId(), hier::kNoRegion, asPolygon(outline));
  seed.cover = hier::Cover::Urban;
  seed.producer = "gallery";
  root = tree.add(std::move(seed));

  hier::Plan plan;
  plan.policy.defaults[hier::Role::Streets] = streets;
  plan.policy.defaults[hier::Role::Lots] = lots;
  plan.policy.defaults[hier::Role::Buildings] = "buildings.setback";
  plan.steps.push_back({hier::Role::Streets, {}, 0});
  plan.steps.push_back(
      {hier::Role::Lots,
       [](const hier::Region &r) {
         return r.cover != hier::Cover::Road;
       },
       0});
  plan.steps.push_back(
      {hier::Role::Buildings,
       [](const hier::Region &r) {
         return r.cover != hier::Cover::Road;
       },
       0});

  hier::Context context;
  context.worldSeed = 20260914;

  hier::Composer composer(registry, context);
  hier::Ledger ledger;
  hier::RunStats stats;

  if (!composer.run(plan, tree, root, ledger, stats, &error))
  {
    return false;
  }

  objects = composer.objects();

  return true;
}

void drawSettlement(Panel &p, const hier::RegionTree &tree, const hier::RegionId root, const std::vector<hier::Object> &objects)
{
  std::vector<Ring> roads, lots;

  for (const auto &region : tree.regions())
  {
    if (region.id == root || !tree.childrenOf(region.id).empty())
    {
      continue;
    }

    (region.cover == hier::Cover::Road ? roads : lots).push_back(region.boundary.outer);
  }

  fill(p, "#e6ded2", {tree.find(root)->boundary.outer});
  fill(p, "#cfd8c9", lots);
  fill(p, "#9c9384", roads);

  for (const auto &lot : lots)
  {
    stroke(p, "#6b7a63", 0.8, {lot});
  }

  std::vector<Ring> shells;

  for (const auto &object : objects)
  {
    shells.push_back(object.footprint.outer);
  }

  fill(p, "#8c5a40", shells);
  stroke(p, "#3a2a20", 0.6, shells);
}

void settlementCases()
{
  std::cout << "Streets, blocks, lots and buildings\n";

  struct Case
  {
    const char *name;
    const char *note;
    Ring outline;
    const char *streets;
    const char *lots;
  };

  const std::vector<Case> cases = {
    {"settlement-grid-strip", "square, grid streets, strip lots", square(150), "streets.grid", "lots.strip"},
    {"settlement-radial-strip", "hexagon, radial streets, strip lots", hexagon(75, 80, 80), "streets.radial", "lots.strip"},
    {"settlement-grid-ring", "hexagon, grid streets, ring lots around a court", hexagon(75, 80, 80), "streets.grid", "lots.ring"},
    {"settlement-concave", "L-shaped town: concave all the way down", ell(150), "streets.grid", "lots.strip"},
  };

  for (const auto &c : cases)
  {
    hier::RegionTree tree;
    hier::RegionId root = 0;
    std::vector<hier::Object> objects;
    std::string error;

    check(runSettlement(c.outline, c.streets, c.lots, tree, root, objects, error), std::string(c.name) + " plans: " + error);

    // The invariant that holds whatever the shape: the leaves tile the site.
    double leaves = 0;

    for (const auto &region : tree.regions())
    {
      if (tree.childrenOf(region.id).empty())
      {
        leaves += region.areaMetres();
      }
    }

    check(std::abs(leaves - areaMetres(c.outline)) < 0.05, std::string(c.name) + ": leaves tile the site exactly");

    size_t roads = 0, lots = 0;

    for (const auto &region : tree.regions())
    {
      if (!tree.childrenOf(region.id).empty())
      {
        continue;
      }

      (region.cover == hier::Cover::Road ? roads : lots)++;
    }

    check(roads > 0, std::string(c.name) + ": streets came back as their own regions");
    check(lots > 2, std::string(c.name) + ": the blocks were allocated into lots");
    check(!objects.empty(), std::string(c.name) + ": buildings were placed on the lots");

    bool housesOffRoads = true;

    for (const auto &object : objects)
    {
      const hier::Region *region = tree.find(object.region);

      if (region == nullptr || region->cover == hier::Cover::Road)
      {
        housesOffRoads = false;
      }
    }

    check(housesOffRoads, std::string(c.name) + ": no building stands on a road");

    auto &p = panel(c.name, c.note);
    drawSettlement(p, tree, root, objects);
  }
}

void wardCharacterCases()
{
  std::cout << "Ward character: the same block, five parameter sets\n";

  struct Character
  {
    const char *name;
    const char *note;
    hier::BisectionLots::Settings settings;
  };

  const std::vector<Character> characters = {
    {"ward-craftsmen", "craftsmen: small to large, moderately regular", hier::BisectionLots::craftsmen()},
    {"ward-patriciate", "patriciate: large plots, a fifth left as garden", hier::BisectionLots::patriciate()},
    {"ward-administration", "administration: large and deliberately regular", hier::BisectionLots::administration()},
    {"ward-slum", "slum: small and chaotic", hier::BisectionLots::slum()},
  };

  for (const auto &character : characters)
  {
    hier::Registry registry;
    std::string error;
    registry.bind(std::make_shared<hier::BisectionLots>(character.settings), &error);
    registry.bind(std::make_shared<hier::HouseFootprints>(), &error);

    hier::RegionTree tree;
    hier::Region seed = hier::makeRegion(tree.nextRegionId(), hier::kNoRegion, asPolygon(square(90)));
    seed.cover = hier::Cover::Urban;
    const hier::RegionId root = tree.add(std::move(seed));

    hier::Plan plan;
    plan.policy.defaults[hier::Role::Lots] = "lots.bisection";
    plan.policy.defaults[hier::Role::Buildings] = "buildings.setback";
    plan.steps.push_back({hier::Role::Lots, {}, 0});
    plan.steps.push_back(
        {hier::Role::Buildings,
         [](const hier::Region &r) {
           return r.cover != hier::Cover::Road;
         },
         0});

    hier::Context context;
    context.worldSeed = 99;

    hier::Composer composer(registry, context);
    hier::Ledger ledger;
    hier::RunStats stats;

    check(composer.run(plan, tree, root, ledger, stats, &error), std::string(character.name) + " plans: " + error);

    double leaves = 0;
    size_t lots = 0;

    for (const auto &region : tree.regions())
    {
      if (tree.childrenOf(region.id).empty())
      {
        leaves += region.areaMetres();

        if (region.cover != hier::Cover::Road)
        {
          ++lots;
        }
      }
    }

    check(std::abs(leaves - areaMetres(square(90))) < kAreaTolerance * 100, std::string(character.name) + ": lots and alleys tile the block");
    check(lots > 3, std::string(character.name) + ": the block was subdivided");

    auto &p = panel(character.name, character.note);
    drawSettlement(p, tree, root, composer.objects());
  }
}

void samplingCases()
{
  std::cout << "Blue noise in a region\n";

  const Polygon region = asPolygon(comb(80));

  SampleSettings settings;
  settings.spacingMetres = 6;
  settings.seed = 11;

  std::vector<Point> points;
  std::string error;

  check(samplePolygon(region, settings, points, &error) && points.size() > 10, "a comb samples: " + error);

  bool inside = true;

  for (const auto &q : points)
  {
    if (!contains(region, q))
    {
      inside = false;
    }
  }

  check(inside, "and every sample lands inside it, teeth and notches included");

  auto &p = panel("sampling", "blue noise in a comb: nothing lands in a notch");
  fillPolys(p, "#dfe4ea", {region});
  stroke(p, "#2f4858", 1.2, {region.outer});
  dots(p, "#b04a3a", 1.6, points);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void writeGallery(const std::string &path)
{
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(4);

  out << "{\n  \"panels\": [\n";

  for (size_t i = 0; i < gallery.size(); ++i)
  {
    const auto &p = gallery[i];
    out << "    {\"name\": \"" << p.name << "\", \"note\": \"" << p.note << "\", \"layers\": [\n";

    for (size_t l = 0; l < p.layers.size(); ++l)
    {
      const auto &layer = p.layers[l];
      out << "      {\"kind\": \"" << layer.kind << "\", \"colour\": \"" << layer.colour << "\", \"width\": " << layer.width << ", \"shapes\": [";

      for (size_t sIndex = 0; sIndex < layer.shapes.size(); ++sIndex)
      {
        out << "[";

        for (size_t v = 0; v < layer.shapes[sIndex].size(); ++v)
        {
          out << "[" << toMetres(layer.shapes[sIndex][v].x) << "," << toMetres(layer.shapes[sIndex][v].y) << "]";

          if (v + 1 < layer.shapes[sIndex].size())
          {
            out << ",";
          }
        }

        out << "]";

        if (sIndex + 1 < layer.shapes.size())
        {
          out << ",";
        }
      }

      out << "]}";
      out << (l + 1 < p.layers.size() ? ",\n" : "\n");
    }

    out << "    ]}";
    out << (i + 1 < gallery.size() ? ",\n" : "\n");
  }

  out << "  ]\n}\n";

  std::ofstream file(path);
  file << out.str();
}

} // namespace

int main(int argc, char **argv)
{
  std::cout << "generator gallery\n";

  booleanCases();
  offsetCases();
  riverCases();
  partitionCases();
  settlementCases();
  wardCharacterCases();
  samplingCases();

  const std::string path = argc > 1 ? argv[1] : "generator-gallery.json";
  writeGallery(path);
  std::cout << "wrote " << gallery.size() << " panels to " << path << "\n";

  if (failures != 0)
  {
    std::cerr << failures << " gallery test(s) failed\n";

    return 1;
  }

  std::cout << "All gallery tests passed\n";

  return 0;
}
