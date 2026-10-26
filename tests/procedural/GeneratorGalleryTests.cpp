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
#include "procedural/world/generator/ComplexDwellingPlanner.hpp"
#include "procedural/world/hierarchy/Generators.hpp"

#include <array>
#include <cmath>
#include <map>
#include <set>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace procedural;
using namespace procedural::geom2;
namespace hier = procedural::world::hierarchy;
namespace gen = procedural::world::generator;

namespace {

constexpr double kPi = 3.14159265358979323846;

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
std::string townSolids;

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


// Does this polygon have an edge lying along the ring? Collinear overlap, not
// shared corners - the same test the generator uses to tell a court from a lot.
bool liesAlongTest(const Polygon &poly, const Ring &ring)
{
  for (size_t i = 0; i < poly.outer.size(); ++i)
  {
    const double px = toMetres(poly.outer[i].x), py = toMetres(poly.outer[i].y);
    const double qx = toMetres(poly.outer[(i + 1) % poly.outer.size()].x), qy = toMetres(poly.outer[(i + 1) % poly.outer.size()].y);

    if (std::hypot(qx - px, qy - py) < 1.2)
    {
      continue;
    }

    for (size_t j = 0; j < ring.size(); ++j)
    {
      const double ax = toMetres(ring[j].x), ay = toMetres(ring[j].y);
      const double bx = toMetres(ring[(j + 1) % ring.size()].x), by = toMetres(ring[(j + 1) % ring.size()].y);
      const double sx = bx - ax, sy = by - ay;
      const double length = std::hypot(sx, sy);

      if (length < 1e-9 || std::abs(sx * (py - ay) - sy * (px - ax)) / length > .25 || std::abs(sx * (qy - ay) - sy * (qx - ax)) / length > .25)
      {
        continue;
      }

      const double t0 = ((px - ax) * sx + (py - ay) * sy) / length;
      const double t1 = ((qx - ax) * sx + (qy - ay) * sy) / length;

      if (std::min(length, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1)) >= 1.2)
      {
        return true;
      }
    }
  }

  return false;
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

  // Drawn by kind, because a plot is not only its house: a garden, a paddock
  // and a barn are ground the plan has decided something about, and a render
  // that shows them all as buildings cannot be judged.
  std::vector<Ring> houses, gardens, paddocks, yards, paths, sheds;

  for (const auto &object : objects)
  {
    if (object.kind == "garden")
    {
      gardens.push_back(object.footprint.outer);
    }
    else if (object.kind == "paddock")
    {
      paddocks.push_back(object.footprint.outer);
    }
    else if (object.kind == "yard")
    {
      yards.push_back(object.footprint.outer);
    }
    else if (object.kind == "path")
    {
      paths.push_back(object.footprint.outer);
    }
    else if (object.kind == "outbuilding" || object.kind == "barn")
    {
      sheds.push_back(object.footprint.outer);
    }
    else if (object.kind == "house")
    {
      houses.push_back(object.footprint.outer);
    }
  }

  fill(p, "#b9c79a", gardens);
  fill(p, "#d6cfa4", paddocks);
  fill(p, "#e6e2d3", yards);
  fill(p, "#cfc4ad", paths);

  for (const auto &object : objects)
  {
    if (object.kind == "fence")
    {
      stroke(p, "#6f5b3e", 1.0, {object.footprint.outer});
    }
  }

  fill(p, "#8c5a40", houses);
  stroke(p, "#3a2a20", 0.6, houses);
  fill(p, "#6f5340", sheds);
  stroke(p, "#3a2a20", 0.5, sheds);
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
    plan.policy.defaults[hier::Role::Lots] = "lots.bisection." + character.settings.ward;
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


// Does this lot have an edge lying along an edge of some road?
//
// Testing for shared *corners* looked obvious and was wrong: it called lots
// that plainly sit on a street landlocked, because two regions that share a
// boundary in the arrangement need not agree on where its vertices fall once
// either side has been split by something else. The render is what showed it -
// the highlighted lots were all fronting streets. Collinear overlap is the
// question actually being asked.
bool frontsAStreet(const hier::RegionTree &tree, const hier::Region &lot)
{
  const auto &ring = lot.boundary.outer;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const double px = toMetres(ring[i].x), py = toMetres(ring[i].y);
    const double qx = toMetres(ring[(i + 1) % ring.size()].x), qy = toMetres(ring[(i + 1) % ring.size()].y);
    const double ex = qx - px, ey = qy - py;
    const double length = std::hypot(ex, ey);

    if (length < 1.2)
    {
      continue;
    }

    for (const auto &road : tree.regions())
    {
      if (road.cover != hier::Cover::Road || road.boundary.outer.size() < 3u)
      {
        continue;
      }

      for (size_t j = 0; j < road.boundary.outer.size(); ++j)
      {
        const double ax = toMetres(road.boundary.outer[j].x), ay = toMetres(road.boundary.outer[j].y);
        const double bx = toMetres(road.boundary.outer[(j + 1) % road.boundary.outer.size()].x), by = toMetres(road.boundary.outer[(j + 1) % road.boundary.outer.size()].y);
        const double sx = bx - ax, sy = by - ay;
        const double slength = std::hypot(sx, sy);

        if (slength < 1e-9)
        {
          continue;
        }

        if (std::abs(sx * (py - ay) - sy * (px - ax)) / slength > .25 || std::abs(sx * (qy - ay) - sy * (qx - ax)) / slength > .25)
        {
          continue;
        }

        const double t0 = ((px - ax) * sx + (py - ay) * sy) / slength;
        const double t1 = ((qx - ax) * sx + (qy - ay) * sy) / slength;

        if (std::min(slength, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1)) >= 1.2)
        {
          return true;
        }
      }
    }
  }

  return false;
}

// A Watabou town: patches for the street pattern, createAlleys inside each one.
//
// The town generator has no imposed grid. It lays a relaxed Voronoi patch layer
// and the streets *are* the shared edges of those patches, so the street
// pattern inherits the irregularity of the patch layer instead of the patch
// layer inheriting the regularity of a grid. The lots then come from
// Ward.createAlleys within each block. Both halves already existed here and had
// never been put together.
void voronoiTownCases()
{
  std::cout << "A town on a Voronoi patch layer\n";

  hier::Registry registry;
  std::string error;

  // Watabou's scale, read from the source. A patch is one ward of ten to
  // twenty buildings; REGULAR_STREET is one unit (2.5 m) and MAIN_STREET two.
  // The patches here were three times his size and the streets twice his
  // width, and the render read as a Voronoi diagram because that is what a
  // town drawn at the wrong scale looks like.
  hier::VoronoiBlocks::Settings patches;
  patches.spacingMetres = 20;
  patches.halfWidthMetres = 1.25;
  patches.arteryHalfWidthMetres = 3.2;
  patches.arteries = 5;
  patches.relaxIterations = 3;
  // Softer and wider than Watabou's single pass at weight 3: his A* routes
  // run over many more patches than a block layer this size produces, so the
  // same kernel applied once barely bends ours.
  patches.smoothingWeight = 1.6;
  patches.smoothingPasses = 3;

  registry.bind(std::make_shared<hier::VoronoiBlocks>(patches), &error);

  // Every ward class Watabou's createWards can hand a patch, bound at once so
  // the policy can choose between them. Rectangular by construction, except
  // the slum, whose whole character is that it is not.
  for (hier::BisectionLots::Settings ward : {hier::BisectionLots::craftsmen(), hier::BisectionLots::merchant(), hier::BisectionLots::patriciate(), hier::BisectionLots::administration(), hier::BisectionLots::slum()})
  {
    ward.orthogonal = ward.ward != "slum";
    registry.bind(std::make_shared<hier::BisectionLots>(ward), &error);
  }

  // Outside the walls the ground is cheap, so the plots are large and few.
  {
    hier::BisectionLots::Settings outskirts = hier::BisectionLots::craftsmen();
    outskirts.ward = "outskirts";
    outskirts.orthogonal = true;
    outskirts.minimumAreaMetres = 260;
    outskirts.minimumAreaSpreadMetres = 220;
    outskirts.emptyProbability = .12;

    registry.bind(std::make_shared<hier::BisectionLots>(outskirts), &error);
  }

  // Perimeter blocks, at three sizes. The rule is one rule - lots around the
  // edge, common ground in the middle - and the ward is the size of the lots.
  // A patrician house on a garden court is not a cathedral cloister cell.
  for (const auto &[ward, depth, target] : {std::tuple<const char *, double, double>{"temple", 7.0, 55.0}, {"patriciate", 11.0, 150.0}, {"administration", 12.0, 190.0}})
  {
    hier::RingLots::Settings ring;
    ring.ward = ward;
    ring.depthMetres = depth;
    ring.targetAreaMetres = target;

    registry.bind(std::make_shared<hier::RingLots>(ring), &error);
  }

  // What a plot does with the ground its house does not cover, by ward.
  //
  // A poor plot builds over all of it - a slum where every house has a garden
  // is not a slum - and a rich one does the opposite: the house takes the
  // frontage, the rest is garden, and on the largest plots the service
  // buildings stand in it. That is the same distinction Watabou's mansion
  // generator draws at a larger scale, and what separates them is how much of
  // the plot is deliberately not built on.
  {
    // Each ward as a plot standing. The numbers - how much of the lot the
    // house takes, storeys, garden, outbuildings, fence - live in the lot
    // planner's standing table; a ward here only says which standing it is.
    struct Plot
    {
      const char *ward;
      gen::PlotStanding standing;
    };

    for (const Plot &plot : {
           Plot{"slum", gen::PlotStanding::Slum},
           Plot{"craftsmen", gen::PlotStanding::Modest},
           Plot{"merchant", gen::PlotStanding::Comfortable},
           Plot{"patriciate", gen::PlotStanding::Wealthy},
           Plot{"administration", gen::PlotStanding::Grand},
           Plot{"temple", gen::PlotStanding::Comfortable},
           // Outside the walls: a cottage on a large plot, most of it a fenced
           // pen with a barn in it.
           Plot{"outskirts", gen::PlotStanding::Rural},
         })
    {
      hier::HouseFootprints::Settings houses;
      houses.ward = plot.ward;
      houses.setbackMetres = 1.5;
      houses.minimumAreaMetres = 11;
      houses.maximumCoverage = 0; // the standing's own share
      houses.standing = plot.standing;

      registry.bind(std::make_shared<hier::HouseFootprints>(houses), &error);
    }

    // ...and the plain binding the other panels ask for by name.
    hier::HouseFootprints::Settings plain;
    plain.setbackMetres = 1.0;
    plain.minimumAreaMetres = 11;
    plain.maximumCoverage = 0;
    plain.standing = gen::PlotStanding::Modest;

    registry.bind(std::make_shared<hier::HouseFootprints>(plain), &error);
  }

  hier::RegionTree tree;
  hier::Region seed = hier::makeRegion(tree.nextRegionId(), hier::kNoRegion, asPolygon(square(230)));
  seed.cover = hier::Cover::Urban;
  const hier::RegionId root = tree.add(std::move(seed));

  // Where things go: Model.createWards as a function of the block layer, so
  // there is one assignment and one rule per ward rather than a set of
  // distance bands and id modulos to get wrong.
  geom2::Point townCentre;
  centroid(asPolygon(square(230)), townCentre);

  hier::WardAssignment wards;

  // A ward is a property of a *block*, and the buildings step runs on lots.
  //
  // Asking wardOf about a lot returns the default, because the assignment only
  // knows blocks - so every lot in the town came back "craftsmen" and no plot
  // strategy but that one ever ran. Resolving to the block first is what makes
  // a rule usable at either level.
  const auto wardAt = [&](const hier::Region &r) {
    const hier::Region *block = tree.find(r.parent);
    const bool isBlock = block == nullptr || block->id == root;

    return wards.wardOf(tree, root, isBlock ? r : *block, townCentre, 20260915);
  };

  const auto is = [&](const char *name) {
    return [&, name](const hier::Region &r) {
      return wardAt(r) == name;
    };
  };

  hier::Plan plan;
  plan.policy.defaults[hier::Role::Streets] = "blocks.voronoi";
  plan.policy.defaults[hier::Role::Lots] = "lots.bisection.craftsmen";
  plan.policy.defaults[hier::Role::Buildings] = "buildings.setback";

  // Perimeter blocks: lots around the edge, common ground in the middle.
  //
  // Every lot fronts the street by construction, which is what a block of this
  // size needs - subdividing it by chords alone leaves interior lots reachable
  // only through their neighbours. A patrician or administrative block is a
  // court with houses around it in any case, and the temple's is a cloister.
  // Each ward's plots do something different with the ground the house does
  // not cover: a slum builds over all of it, a patrician plot keeps most of it
  // as garden with the service buildings standing in it.
  for (const char *ward : {"slum", "merchant", "patriciate", "administration", "temple", "outskirts"})
  {
    plan.policy.rules.push_back({hier::Role::Buildings, std::string("buildings.setback.") + ward, is(ward), std::string(ward) + " plots"});
  }

  plan.policy.rules.push_back({hier::Role::Lots, "lots.ring.temple", is("temple"), "temple: a cloister of lots around a court, beside the plaza"});
  plan.policy.rules.push_back({hier::Role::Lots, "lots.ring.patriciate", is("patriciate"), "patriciate: houses around a garden court"});
  plan.policy.rules.push_back({hier::Role::Lots, "lots.ring.administration", is("administration"), "administration: ranges around a court"});
  plan.policy.rules.push_back({hier::Role::Lots, "lots.bisection.merchant", is("merchant"), "merchant: nearest the plaza"});
  plan.policy.rules.push_back({hier::Role::Lots, "lots.bisection.slum", is("slum"), "slum: farthest from the plaza"});
  plan.policy.rules.push_back({hier::Role::Lots, "lots.bisection.outskirts", is("outskirts"), "outskirts: few large plots, packed loosely"});

  const auto buildable = [&](const hier::Region &r) {
    return r.cover != hier::Cover::Road && wards.wardOf(tree, root, r, townCentre, 20260915) != "plaza";
  };

  plan.steps.push_back({hier::Role::Streets, {}, 0});
  plan.steps.push_back({hier::Role::Lots, buildable, 0});
  plan.steps.push_back({hier::Role::Buildings, buildable, 0});

  hier::Context context;
  context.worldSeed = 20260915;

  hier::Composer composer(registry, context);
  hier::Ledger ledger;
  hier::RunStats stats;

  check(composer.run(plan, tree, root, ledger, stats, &error), "voronoi town plans: " + error);

  size_t blocks = 0, lots = 0, roads = 0;

  for (const auto &region : tree.regions())
  {
    if (region.producer == "blocks.voronoi" || region.cover == hier::Cover::Road)
    {
      roads += region.cover == hier::Cover::Road ? 1u : 0u;
    }

    if (!tree.childrenOf(region.id).empty())
    {
      blocks += region.cover != hier::Cover::Road ? 1u : 0u;
      continue;
    }

    lots += region.cover != hier::Cover::Road ? 1u : 0u;
  }

  std::cout << "  town: " << roads << " street regions, " << blocks << " subdivided blocks, " << lots << " lots\n";

  check(roads > 4, "the patch layer's shared edges became streets");
  check(lots > 40, "and the blocks between them were cut into lots");

  // The point of the patch layer: the streets must not be a grid. A grid has
  // two edge directions; a patch layer has as many as it has edges. Measured as
  // the spread of street bearings, folded into [0, pi) so opposite directions
  // count once.
  std::vector<double> bearings;

  for (const auto &region : tree.regions())
  {
    if (region.cover != hier::Cover::Road || region.boundary.outer.size() < 3u)
    {
      continue;
    }

    const auto &ring = region.boundary.outer;
    double longest = -1, bearing = 0;

    for (size_t i = 0; i < ring.size(); ++i)
    {
      const double ax = toMetres(ring[i].x), ay = toMetres(ring[i].y);
      const double bx = toMetres(ring[(i + 1) % ring.size()].x), by = toMetres(ring[(i + 1) % ring.size()].y);
      const double length = std::hypot(bx - ax, by - ay);

      if (length > longest)
      {
        longest = length;
        bearing = std::atan2(by - ay, bx - ax);
      }
    }

    while (bearing < 0)
    {
      bearing += kPi;
    }

    bearings.push_back(std::fmod(bearing, kPi));
  }

  // Sixteen buckets over the half circle. A grid fills two of them.
  std::vector<int> buckets(16, 0);

  for (const double bearing : bearings)
  {
    ++buckets[std::min<size_t>(15, size_t(bearing / kPi * 16.0))];
  }

  size_t occupied = 0;

  for (const int count : buckets)
  {
    occupied += count > 0 ? 1u : 0u;
  }

  std::cout << "  town: street bearings occupy " << occupied << " of 16 directions\n";
  std::cout << "  town: " << composer.objects().size() << " buildings on " << lots << " lots\n";

  {
    std::map<std::string, std::array<double, 3>> byWard; // house area, garden area, outbuildings

    for (const auto &object : composer.objects())
    {
      const hier::Region *lot = tree.find(object.region);

      if (lot == nullptr)
      {
        continue;
      }

      const std::string ward = wardAt(*lot);
      auto &entry = byWard[ward];

      entry[0] += object.kind == "house" ? areaMetres(object.footprint) : 0.0;
      entry[1] += (object.kind == "garden" || object.kind == "paddock" || object.kind == "yard") ? areaMetres(object.footprint) : 0.0;
      entry[2] += (object.kind == "outbuilding" || object.kind == "barn" || object.kind == "fence") ? 1.0 : 0.0;
    }

    for (const auto &[ward, entry] : byWard)
    {
      const double total = entry[0] + entry[1];
      std::printf("    plots %-15s houses %6.0f m2, open %6.0f m2 (%2.0f%%), %d fences/sheds\n", ward.c_str(), entry[0], entry[1], total > 1 ? 100.0 * entry[1] / total : 0.0, int(entry[2]));
    }

    // The claim: a plot's openness rises with the wealth of its ward. A slum
    // builds over everything it has; a patrician plot keeps most of it.
    const auto openShare = [&](const char *ward) {
      const auto it = byWard.find(ward);

      if (it == byWard.end())
      {
        return 0.0;
      }

      const double both = it->second[0] + it->second[1];

      return both > 1 ? it->second[1] / both : 0.0;
    };

    // The claim is the standing table: each ward's open share is what its
    // standing leaves after the house, within a band for what the lots'
    // shapes allow. "Poor plots are built over entirely" was the old claim
    // and it was wrong as a design - a craftsman's house has a yard behind
    // it, and a quarter where every lot is roof is a warehouse district.
    //
    // Sheds and fences stand on the open ground and are not counted as open,
    // so the wards that have them sit a little under their standing.
    const auto near = [&](const char *ward, const double expected, const double band) {
      const double got = openShare(ward);
      const bool ok = std::abs(got - expected) <= band;

      if (!ok)
      {
        std::printf("    %s: open %.0f%%, standing says %.0f%%\n", ward, got * 100, expected * 100);
      }

      return ok;
    };

    check(near("slum", 1 - .78, .12) && near("craftsmen", 1 - .62, .12), "poor plots keep the share their standing leaves them and no more");
    check(near("merchant", 1 - .48, .14) && near("patriciate", 1 - .34, .14) && near("administration", 1 - .26, .14), "and rich plots keep the garden their standing says");
    check(openShare("slum") < openShare("craftsmen") && openShare("craftsmen") < openShare("merchant") && openShare("merchant") < openShare("patriciate"), "and openness rises with standing");

    // Outside the walls a plot is mostly working ground, and that is the
    // difference between a village plot and a town one.
    //
    // The band against the standing is the claim; the ordering is the
    // sanity check. A fixed margin over the craftsmen was the old bar and it
    // was a number with no reason behind it - a barn and a fence stand on a
    // rural plot's open ground and are not counted as open, which is exactly
    // where the measured share lands below the standing.
    check(near("outskirts", 1 - .22, .14) && openShare("outskirts") > openShare("craftsmen"), "and plots outside the walls are mostly yard");
  }

  {
    std::map<std::string, int> wards;

    for (const auto &decision : stats.decisions)
    {
      if (decision.rfind("lots: ", 0) == 0)
      {
        ++wards[decision.substr(6, decision.find(' ', 6) - 6)];
      }
    }

    std::cout << "  town: wards";

    for (const auto &[ward, count] : wards)
    {
      std::cout << " " << ward << "=" << count;
    }

    std::cout << "\n";
  }

  check(composer.objects().size() * 2 > lots, "and most lots carry a building rather than standing empty");

  // Why a lot has no building, by the area it had to offer. A lot is only a
  // lot if a building fits on it; below that it is a sliver the cuts left, and
  // the cut that made it should not have been made.
  {
    std::set<hier::RegionId> built;

    for (const auto &object : composer.objects())
    {
      built.insert(object.region);
    }

    std::map<std::string, int> reasons;

    for (const auto &entry : ledger.entries())
    {
      if (entry.message.rfind("buildings", 0) == 0)
      {
        ++reasons[entry.message];
      }
    }

    const double bands[] = {0, 11, 25, 40, 60, 100, 1e9};
    std::vector<int> withBuilding(6, 0), without(6, 0);
    std::map<hier::RegionId, std::vector<double>> perBlock;

    for (const auto &region : tree.regions())
    {
      if (region.cover == hier::Cover::Road || !tree.childrenOf(region.id).empty() || region.boundary.outer.size() < 3u || region.parent == hier::kNoRegion)
      {
        continue;
      }

      const double a = region.areaMetres();
      size_t band = 0;

      while (band + 1 < 6 && a >= bands[band + 1])
      {
        ++band;
      }

      (built.count(region.id) ? withBuilding : without)[band]++;
      perBlock[region.parent].push_back(a);
    }

    std::cout << "  town: lot area bands (m2)      <11   11-25  25-40  40-60  60-100  100+\n";
    std::printf("  town: with a building      %6d %6d %6d %6d %6d %6d\n", withBuilding[0], withBuilding[1], withBuilding[2], withBuilding[3], withBuilding[4], withBuilding[5]);
    std::printf("  town: without a building   %6d %6d %6d %6d %6d %6d\n", without[0], without[1], without[2], without[3], without[4], without[5]);

    for (const auto &[reason, count] : reasons)
    {
      std::cout << "  town: " << count << "  " << reason << "\n";
    }

    // Lot shape: how much of its own bounding rectangle each lot fills,
    // measured on its longest edge, and how narrow that rectangle is. A
    // rectangle fills 1, a triangle 0.5; a sliver fills well and is still a
    // sliver, so both are needed.
    {
      std::vector<double> fills, aspects;
      int thin = 0, poorFill = 0, terraces = 0, measured = 0;

      for (const auto &region : tree.regions())
      {
        if (region.cover == hier::Cover::Road || !tree.childrenOf(region.id).empty() || region.boundary.outer.size() < 3u || region.parent == hier::kNoRegion)
        {
          continue;
        }

        const auto &ring = region.boundary.outer;
        double ux = 1, uy = 0, longest = -1;

        for (size_t i = 0; i < ring.size(); ++i)
        {
          const double bx = toMetres(ring[(i + 1) % ring.size()].x - ring[i].x), by = toMetres(ring[(i + 1) % ring.size()].y - ring[i].y);
          const double length = std::hypot(bx, by);

          if (length > longest)
          {
            longest = length;
            ux = bx / length;
            uy = by / length;
          }
        }

        double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;

        for (const auto &p : ring)
        {
          const double x = toMetres(p.x), y = toMetres(p.y);
          const double a = x * ux + y * uy, b = -x * uy + y * ux;

          loU = std::min(loU, a);
          hiU = std::max(hiU, a);
          loV = std::min(loV, b);
          hiV = std::max(hiV, b);
        }

        const double w = hiU - loU, h = hiV - loV;
        const double fill = w * h > 1e-9 ? region.areaMetres() / (w * h) : 0;

        ++measured;
        fills.push_back(fill);
        aspects.push_back(std::max(w, h) > 1e-9 ? std::min(w, h) / std::max(w, h) : 0);
        thin += std::min(w, h) < 3.5 ? 1 : 0;
        poorFill += fill < .58 ? 1 : 0;
        terraces += std::max(w, h) > std::min(w, h) * 4 ? 1 : 0;
      }

      std::sort(fills.begin(), fills.end());
      std::sort(aspects.begin(), aspects.end());

      const double median = fills.empty() ? 0 : fills[fills.size() / 2];
      const double worst = fills.empty() ? 0 : fills[fills.size() / 20];
      const double aspect = aspects.empty() ? 0 : aspects[aspects.size() / 2];

      std::printf("  town: lot fill median %.2f, 5th percentile %.2f; aspect median %.2f; %d of %d thinner than 3.5 m, %d wedges, %d longer than 4:1\n", median, worst, aspect, thin, measured, poorFill, terraces);

      check(thin * 20 < measured, "and few lots are too thin to build on");
      check(poorFill * 10 < measured, "and few lots are wedges rather than plots");
      check(terraces * 5 < measured, "and few lots are terraces rather than plots");
    }

    // The courts: the piece a perimeter block's peel leaves in the middle.
    // Common ground, not a plot - so not a lot for any of the measurements
    // below, and not landlocked for having no frontage.
    std::set<hier::RegionId> courts;

    for (const auto &region : tree.regions())
    {
      if (region.producer != "lots.ring" || !tree.childrenOf(region.id).empty())
      {
        continue;
      }

      const hier::Region *parent = tree.find(region.parent);

      if (parent == nullptr || parent->producer == "lots.ring")
      {
        continue;
      }

      hier::RegionId largest = hier::kNoRegion;
      double most = 0;

      for (const hier::RegionId sibling : tree.childrenOf(region.parent))
      {
        const hier::Region *other = tree.find(sibling);

        if (other != nullptr && other->cover != hier::Cover::Road && other->areaMetres() > most)
        {
          most = other->areaMetres();
          largest = other->id;
        }
      }

      if (largest != hier::kNoRegion)
      {
        courts.insert(largest);
      }
    }

    // Street access: a lot with no edge on a road is landlocked, and a plot
    // you cannot reach is not a plot. Measured as an edge whose *both*
    // endpoints are corners of some road region - a single shared corner is a
    // touch, not a frontage.
    {
      int landlocked = 0, checked = 0;
      std::map<std::string, int> landlockedBy;

      for (const auto &region : tree.regions())
      {
        if (region.cover == hier::Cover::Road || !tree.childrenOf(region.id).empty() || region.boundary.outer.size() < 3u || region.parent == hier::kNoRegion || courts.count(region.id) != 0)
        {
          continue;
        }

        ++checked;

        if (!frontsAStreet(tree, region))
        {
          ++landlocked;
          ++landlockedBy[region.producer];
        }
      }

      std::printf("  town: %d of %d lots have no street frontage\n", landlocked, checked);

      for (const auto &[who, count] : landlockedBy)
      {
        std::printf("    landlocked %3d  %s\n", count, who.c_str());
      }

      check(landlocked * 25 < checked, "and almost every lot fronts a street");
    }

    // Lot size and the street each ward sits on: a rich ward should have the
    // larger plots and the wider street, and "richer wards avoid clusters of
    // small houses" is a claim about exactly these two numbers.
    {
      std::map<std::string, std::pair<double, int>> areaBy;

      for (const auto &region : tree.regions())
      {
        if (region.cover == hier::Cover::Road || !tree.childrenOf(region.id).empty() || region.boundary.outer.size() < 3u || region.parent == hier::kNoRegion || courts.count(region.id) != 0)
        {
          continue;
        }

        auto &entry = areaBy[region.producer];
        entry.first += region.areaMetres();
        entry.second += 1;
      }

      for (const auto &[who, entry] : areaBy)
      {
        std::printf("    ward %-32s %3d lots, mean %6.1f m2\n", who.c_str(), entry.second, entry.first / double(std::max(1, entry.second)));
      }
    }

    // Do the perimeter blocks actually have open courts? One per ring block is
    // the answer; fewer means a court was built on.
    {
      std::map<std::string, int> ringBlocks;

      for (const auto &region : tree.regions())
      {
        if (region.producer.rfind("lots.ring", 0) == 0 && !tree.childrenOf(region.id).empty())
        {
          continue;
        }

        const hier::Region *parent = tree.find(region.parent);

        if (parent != nullptr && region.producer.rfind("lots.ring", 0) == 0)
        {
          ringBlocks[region.producer];
        }
      }

      int courtsFound = 0;

      for (const auto &entry : ledger.entries())
      {
        courtsFound += entry.message.find("court of a perimeter block") != std::string::npos ? 1 : 0;
      }

      std::printf("  town: %d perimeter blocks, %zu of them left an open court\n", int(ringBlocks.size()), size_t(courtsFound));
    }

    // The gate: a lot is a lot only if a building fits on it. Slivers under
    // 25 m2 that stand unbuilt are cuts that should not have been made or
    // cells that should have been merged, and a few per cent is the most a
    // junction can be forgiven for.
    const int slivers = without[0] + without[1];
    std::cout << "  town: " << slivers << " unbuilt slivers under 25 m2 of " << lots << " lots\n";
    check(slivers * 100 < lots * 4, "and under four per cent of lots are unbuilt slivers");

    // How articulated the buildings are.
    //
    // A building footprint used to be the lot inset by its setback, so it had
    // the lot's shape - a Voronoi trapezoid - and the town read as a field of
    // wedges. Watabou's createOrthoBuilding slices the ground on two fixed
    // axes and keeps each finished cell only with probability `fill`, so an
    // outline turns corners: it is an L, or a bar with a wing, or a square
    // with a bite out of one side.
    //
    // Two things say that happened. A reflex corner is the articulation
    // itself: a convex outline cannot be an L however many vertices it has.
    // And the corners the cuts make are right angles, because the two axes are
    // perpendicular and fixed for the whole building.
    //
    // Counted per corner, not per building: the outline still contains the
    // stretches of lot boundary it was cut from, and those carry the lot's
    // angles whatever the rule does. Watabou's do too - he slices the block, he
    // does not rebuild its edges - so a building being wholly orthogonal is not
    // the thing to ask for. The share of corners that are square is.
    int articulated = 0, orthogonal = 0, buildings = 0, corners = 0;

    for (const auto &object : composer.objects())
    {
      // Only the buildings big enough for the rule to have run on them: a
      // house under a cell is passed through whole by design, and counting it
      // as "failed to turn a corner" would be counting the design.
      if (object.kind != "house" || geom2::areaMetres(object.footprint) < 100)
      {
        continue;
      }

      ++buildings;

      const auto &ring = object.footprint.outer;
      bool reflex = false;

      for (size_t i = 0; i < ring.size(); ++i)
      {
        const auto &a = ring[(i + ring.size() - 1) % ring.size()], &b = ring[i], &c = ring[(i + 1) % ring.size()];
        const double ux = geom2::toMetres(b.x - a.x), uy = geom2::toMetres(b.y - a.y);
        const double vx = geom2::toMetres(c.x - b.x), vy = geom2::toMetres(c.y - b.y);
        const double lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);

        if (lu < .2 || lv < .2)
        {
          continue;
        }

        const double cross = (ux * vy - uy * vx) / (lu * lv), dot = (ux * vx + uy * vy) / (lu * lv);

        // A vertex that barely turns is not a corner, it is the resolution of
        // the polyline it sits on. Smoothed streets leave a lot of them on a
        // lot's boundary, and counting each as a corner that failed to be a
        // right angle measures the street smoother, not the building.
        if (dot > .94)
        {
          continue;
        }

        ++corners;

        if (std::abs(dot) < .17)
        {
          ++orthogonal;
        }

        if (cross < -.17)
        {
          reflex = true;
        }
      }

      articulated += reflex ? 1 : 0;
    }

    std::cout << "  town: " << buildings << " buildings, " << articulated << " turn a corner, " << orthogonal << " of " << corners << " corners square\n";

    // A tenth of the large buildings turning a corner is the least that says
    // the rule ran at all; below that it is inert.
    check(articulated * 10 > buildings, "and a good share of the large buildings turn a corner rather than being a plain box");
    // A third. Roughly half an outline's corners are stretches of lot
    // boundary the cuts never touched, and a lot cut from a Voronoi block has
    // whatever angles the block had; only the cut corners can be square. So a
    // third is what a working rule delivers on these shapes, and it is set
    // from the measurement rather than from what would be tidy - a building
    // whose corners were *all* square would mean the outline had stopped
    // following its plot.
    check(orthogonal * 3 > corners, "and a third of their corners are right angles the cuts made");

  }

  auto &p = panel("town-voronoi", "Watabou's patch layer: streets are the shared edges, lots by createAlleys");
  drawSettlement(p, tree, root, composer.objects());

  // One perimeter block on its own, large enough to read.
  //
  // The town panel is too small to see how a court block was divided, and
  // that division is where the ring generator has been wrong twice: once
  // recombining the whole band into a single lot, once slicing the court.
  // Its lots are drawn with every one that has no street edge in red, so
  // the failure is a shape and not a count.
  {
    const hier::Region *ringBlock = nullptr;
    size_t most = 0;

    for (const hier::Region &region : tree.regions())
    {
      if (region.producer.rfind("lots.ring", 0) != 0)
      {
        continue;
      }

      const hier::Region *block = tree.find(region.parent);

      if (block == nullptr)
      {
        continue;
      }

      size_t count = 0;

      for (const hier::RegionId child : tree.childrenOf(block->id))
      {
        const hier::Region *lot = tree.find(child);
        count += lot != nullptr && lot->producer.rfind("lots.ring", 0) == 0 ? 1 : 0;
      }

      if (count > most)
      {
        most = count;
        ringBlock = block;
      }
    }

    if (ringBlock != nullptr)
    {
      auto &ring = panel("town-ring-block", "one perimeter block: its lots, its court, and in red any lot with no street edge");
      fill(ring, "#e6e2d3", {ringBlock->boundary.outer});

      std::vector<Ring> fronting, landlocked, courts;

      for (const hier::RegionId child : tree.childrenOf(ringBlock->id))
      {
        const hier::Region *lot = tree.find(child);

        if (lot == nullptr || lot->boundary.outer.size() < 3)
        {
          continue;
        }

        const bool court = !liesAlongTest(lot->boundary, ringBlock->boundary.outer);
        (court ? courts : fronting).push_back(lot->boundary.outer);

        if (court && areaMetres(lot->boundary) < 60)
        {
          landlocked.push_back(lot->boundary.outer);
        }
      }

      fill(ring, "#d9d2bf", fronting);
      fill(ring, "#b9c79a", courts);
      fill(ring, "#d46a5a", landlocked);
      stroke(ring, "#6b6152", 1.2, std::vector<std::vector<Point>>(fronting.begin(), fronting.end()));
      stroke(ring, "#6b6152", 1.2, std::vector<std::vector<Point>>(courts.begin(), courts.end()));
      stroke(ring, "#2f2a24", 2.0, {ringBlock->boundary.outer});

      std::printf("  ring block %llu: %zu lots front the street, %zu pieces do not (%zu of them under 60 m2)\n", (unsigned long long)ringBlock->id, fronting.size(), courts.size(), landlocked.size());
    }
  }

  // The town as solids.
  //
  // Read from the objects, not rebuilt from them. The layer that plans a
  // building now carries its geometry, so this only has to write it out - and
  // what the picture shows is what the plan actually says, which it was not
  // while the massing rules lived here as well as in the planner.
  {
    using procedural::world::rewrite::Position;

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << "{\n  \"buildings\": [\n";

    size_t written = 0, roofed = 0, arcaded = 0, storeyed = 0;

    const auto faces = [&](const char *key, const std::vector<hier::Solid::Face> &list, const bool last) {
      out << "\"" << key << "\": [";

      for (size_t f = 0; f < list.size(); ++f)
      {
        out << (f ? "," : "") << "[";

        for (size_t v = 0; v < list[f].size(); ++v)
        {
          out << (v ? "," : "") << "[" << list[f][v].x << "," << list[f][v].y << "," << list[f][v].z << "]";
        }

        out << "]";
      }

      out << "]" << (last ? "" : ", ");
    };

    std::vector<const hier::Object *> gardens, yards, fences;

    for (const auto &object : composer.objects())
    {
      if (object.kind == "garden")
      {
        gardens.push_back(&object);
      }
      else if (object.kind == "yard" || object.kind == "paddock")
      {
        yards.push_back(&object);
      }
      else if (object.kind == "fence")
      {
        fences.push_back(&object);
      }

      if (object.solid == nullptr || object.solid->empty())
      {
        continue;
      }

      if (written++)
      {
        out << ",\n";
      }

      out << "    {\"name\": \"" << object.kind << "\", ";
      faces("walls", object.solid->walls, false);
      faces("innerwalls", object.solid->innerWalls, false);
      faces("pillars", object.solid->pillars, false);
      faces("slopes", object.solid->slopes, false);
      faces("soffits", object.solid->soffits, false);
      faces("gables", object.solid->gables, true);
      out << "}";

      roofed += object.solid->slopes.empty() ? 0 : 1;
      arcaded += object.solid->pillars.empty() ? 0 : 1;

      // How many storeys: the wall faces divide into bands by height, and a
      // building that steps has more than one.
      std::set<int> bands;

      for (const auto &face : object.solid->walls)
      {
        double lowest = 1e30;

        for (const auto &v : face)
        {
          lowest = std::min(lowest, v.y);
        }

        bands.insert(int(std::lround(lowest * 10)));
      }

      storeyed += bands.size() > 1 ? 1 : 0;
    }

    // Fences, which are thin bands and have no massing to carry.
    for (const hier::Object *fence : fences)
    {
      std::vector<Position> ring;

      for (const auto &q : fence->footprint.outer)
      {
        ring.push_back(Position{toMetres(q.x), 0, toMetres(q.y)});
      }

      if (ring.size() < 3)
      {
        continue;
      }

      hier::Solid rail;

      for (size_t i = 0; i < ring.size(); ++i)
      {
        const Position &a = ring[i], &c = ring[(i + 1) % ring.size()];

        if (std::hypot(c.x - a.x, c.z - a.z) < 1e-6)
        {
          continue;
        }

        rail.walls.push_back({{a.x, 0, a.z}, {c.x, 0, c.z}, {c.x, fence->heightMetres, c.z}, {a.x, fence->heightMetres, a.z}});
      }

      if (rail.walls.empty())
      {
        continue;
      }

      out << ",\n    {\"name\": \"fence\", ";
      faces("walls", rail.walls, true);
      out << "}";
    }

    // The ground the plot is not built on, flat and just above zero.
    const auto ground = [&](const char *key, const std::vector<const hier::Object *> &pieces, const double y, const bool last) {
      out << "\"" << key << "\": [";

      for (size_t f = 0; f < pieces.size(); ++f)
      {
        out << (f ? "," : "") << "[";

        for (size_t v = 0; v < pieces[f]->footprint.outer.size(); ++v)
        {
          const auto &q = pieces[f]->footprint.outer[v];
          out << (v ? "," : "") << "[" << toMetres(q.x) << "," << y << "," << toMetres(q.y) << "]";
        }

        out << "]";
      }

      out << "]" << (last ? "" : ", ");
    };

    out << ",\n    {\"name\": \"ground\", ";
    ground("garden", gardens, .02, false);
    ground("yard", yards, .01, true);
    out << "}";

    out << "\n  ]\n}\n";

    std::cout << "  town: " << written << " solids carried from their plans, " << roofed << " roofed, " << storeyed << " stepping in height, " << arcaded << " with posts; ground " << gardens.size() << " gardens, " << yards.size() << " yards, " << fences.size() << " fences\n";
    check(written > 300 && roofed == written, "and every building carries the geometry its plan gave it");
    check(!gardens.empty() && !yards.empty(), "and the open ground of every plot is in the model, not only its house");

    townSolids = out.str();
  }

}

// A village is not a town at a smaller size.
//
// Watabou's Village Generator grows roads first and hangs buildings off them,
// with each house square to its own lane and a driveway back to it. The town
// path subdivides the faces of a road graph instead, which gives lots their
// orientation from the face and leaves access implied rather than drawn. The
// gate for this panel is the one the plan states: it has to read as houses
// along a lane, not as a small town.
void villageCases()
{
  std::cout << "A village along its lanes\n";

  hier::Registry registry;
  std::string error;

  registry.bind(std::make_shared<hier::GridStreets>(hier::GridStreets::Settings{55, 3.5, .18}), &error);
  registry.bind(std::make_shared<hier::RoadsideHouses>(), &error);

  hier::RegionTree tree;
  hier::Region seed = hier::makeRegion(tree.nextRegionId(), hier::kNoRegion, asPolygon(square(150)));
  seed.cover = hier::Cover::Urban;
  const hier::RegionId root = tree.add(std::move(seed));

  hier::Plan plan;
  plan.policy.defaults[hier::Role::Streets] = "streets.grid";
  plan.policy.defaults[hier::Role::Buildings] = "buildings.roadside";
  plan.steps.push_back({hier::Role::Streets, {}, 0});
  plan.steps.push_back(
      {hier::Role::Buildings,
       [](const hier::Region &r) {
         return r.cover != hier::Cover::Road;
       },
       0});

  hier::Context context;
  context.worldSeed = 4242;

  hier::Composer composer(registry, context);
  hier::Ledger ledger;
  hier::RunStats stats;

  check(composer.run(plan, tree, root, ledger, stats, &error), "village plans: " + error);

  size_t houses = 0, driveways = 0;

  for (const auto &object : composer.objects())
  {
    houses += object.kind == "house" ? 1u : 0u;
    driveways += object.kind == "driveway" ? 1u : 0u;
  }

  check(houses > 8, "the village placed houses along its lanes");

  // The rule that separates a village from a town: every house has its own
  // access, rather than access being implied by which block it fell in.
  check(driveways == houses, "and every house has a driveway back to its road");

  // Each house is square to the lane it stands on. Measured rather than
  // assumed: a footprint that is square to its road has two edge directions,
  // and the grid here is laid at an angle, so an axis-aligned house would show
  // up as one whose edges run along the world axes instead.
  bool squareToRoad = true;

  for (const auto &object : composer.objects())
  {
    if (object.kind != "house" || object.footprint.outer.size() < 4u)
    {
      continue;
    }

    const auto &ring = object.footprint.outer;
    const double ax = toMetres(ring[1].x) - toMetres(ring[0].x), ay = toMetres(ring[1].y) - toMetres(ring[0].y);
    const double bx = toMetres(ring[2].x) - toMetres(ring[1].x), by = toMetres(ring[2].y) - toMetres(ring[1].y);
    const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);

    if (la < 1e-6 || lb < 1e-6)
    {
      continue;
    }

    squareToRoad = squareToRoad && std::abs((ax * bx + ay * by) / (la * lb)) < .05;
  }

  check(squareToRoad, "and each house stands square to the road it fronts");

  // No two houses may share ground. Measured across the whole village, not
  // within one parcel: two parcels facing the same lane are planned
  // independently, so a per-parcel check cannot see a collision between them.
  size_t collisions = 0;
  double worstOverlap = 0;

  std::vector<const hier::Object *> built;

  for (const auto &object : composer.objects())
  {
    if (object.kind == "house")
    {
      built.push_back(&object);
    }
  }

  for (size_t a = 0; a < built.size(); ++a)
  {
    for (size_t b = a + 1; b < built.size(); ++b)
    {
      std::vector<Polygon> overlap;

      if (!booleanOp({toOperand(built[a]->footprint), toOperand(built[b]->footprint)}, Operation::Intersection, {}, overlap, nullptr) || overlap.empty())
      {
        continue;
      }

      double shared = 0;

      for (const auto &piece : overlap)
      {
        shared += areaMetres(piece);
      }

      if (shared > kAreaTolerance)
      {
        ++collisions;
        worstOverlap = std::max(worstOverlap, shared);
      }
    }
  }

  std::cout << "  village: " << built.size() << " houses, " << collisions << " overlapping pairs, worst " << worstOverlap << " m2\n";

  check(collisions == 0, "and no two houses share ground");

  auto &p = panel("village-lane", "houses square to their lane, each with a driveway back to it");
  drawSettlement(p, tree, root, composer.objects());
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
  voronoiTownCases();
  villageCases();
  samplingCases();

  const std::string path = argc > 1 ? argv[1] : "generator-gallery.json";
  writeGallery(path);
  std::cout << "wrote " << gallery.size() << " panels to " << path << "\n";

  if (!townSolids.empty())
  {
    const size_t dot = path.rfind('.');
    const std::string solids = (dot == std::string::npos ? path : path.substr(0, dot)) + "-solids.json";

    std::ofstream file(solids);
    file << townSolids;

    std::cout << "wrote the town's solids to " << solids << "\n";
  }

  if (failures != 0)
  {
    std::cerr << failures << " gallery test(s) failed\n";

    return 1;
  }

  std::cout << "All gallery tests passed\n";

  return 0;
}
