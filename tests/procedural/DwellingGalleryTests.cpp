// Every dwelling the planner can be asked for, on lots plain enough to judge
// by eye, plus the one number that says whether the planner is working.
//
// That number is the decline rate. `planDwelling` fails explicitly rather than
// forcing a room it cannot justify, which is a defensible choice and makes the
// rate the thing to watch: a planner that declines a third of its lots has not
// planned a third of the town, and no assertion about the plans it *did*
// produce will say so. Watabou's Dwellings assigns what it can and leaves the
// rest generic, and the author says outright that the purpose stage "sometimes
// fails" - so a decline there costs a label, and a decline here costs a
// building.
//
// The corpus is deliberately ordinary: the wing proportions a settlement
// actually hands the planner, across every use and floor count, not the shapes
// that happen to work.

#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/geom2/Quantise.hpp"
#include "procedural/world/WorldDwellingPlan.hpp"
#include "procedural/world/generator/ComplexDwellingPlanner.hpp"
#include "procedural/world/generator/PlotPlanner.hpp"
#include "procedural/world/generator/CompoundPlanner.hpp"
#include "procedural/world/generator/DwellingPlanner.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <vector>

using namespace procedural;
using namespace procedural::world;
using procedural::world::rewrite::Position;
namespace generator = procedural::world::generator;
using procedural::world::generator::planDwelling;

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
// The render document, in the same shape the other galleries use so the same
// renderer draws it.
// ---------------------------------------------------------------------------

struct Layer
{
  std::string kind; // fill | line | point | legend
  std::string colour;
  double width = 1;
  std::string note;
  std::vector<std::vector<Position>> shapes;
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

void writeGallery(const std::string &path)
{
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(4);

  out << "{\n  \"panels\": [\n";

  for (size_t i = 0; i < gallery.size(); ++i)
  {
    const Panel &p = gallery[i];
    out << "    {\"name\": \"" << p.name << "\", \"note\": \"" << p.note << "\", \"layers\": [\n";

    for (size_t l = 0; l < p.layers.size(); ++l)
    {
      const Layer &layer = p.layers[l];
      out << "      {\"kind\": \"" << layer.kind << "\", \"colour\": \"" << layer.colour << "\", \"width\": " << layer.width << ", \"note\": \"" << layer.note << "\", \"shapes\": [";

      for (size_t s = 0; s < layer.shapes.size(); ++s)
      {
        out << "[";

        for (size_t v = 0; v < layer.shapes[s].size(); ++v)
        {
          out << "[" << layer.shapes[s][v].x << "," << layer.shapes[s][v].z << "]";

          if (v + 1 < layer.shapes[s].size())
          {
            out << ",";
          }
        }

        out << "]" << (s + 1 < layer.shapes.size() ? "," : "");
      }

      out << "]}" << (l + 1 < p.layers.size() ? ",\n" : "\n");
    }

    out << "    ]}" << (i + 1 < gallery.size() ? ",\n" : "\n");
  }

  out << "  ]\n}\n";

  std::ofstream file(path);
  file << out.str();

  std::cout << "wrote " << gallery.size() << " panels to " << path << "\n";
}

// ---------------------------------------------------------------------------
// The exterior, as solids.
//
// A floor plan cannot show whether a building stands up. The roof was the part
// with no surfaces at all, so it could only be judged as a ridge line drawn
// over a plan - which is exactly how a roof that does not reach its walls, or
// two wings that interpenetrate, go unnoticed. These write the walls and roof
// faces out as three-dimensional polygons for a separate renderer.
// ---------------------------------------------------------------------------

struct Solid
{
  std::string name;
  std::vector<std::vector<Position>> walls, innerWalls, slopes, gables, soffits, pillars, garden, yard;
};

std::vector<Solid> solids;

void collectSolid(const std::string &name, const rewrite::DwellingPlan &plan, const double foundation)
{
  Solid solid;
  solid.name = name;

  // Walls come from the storey plates, not from the wings.
  //
  // A wing's own outline includes the wall it shares with the wing next to
  // it, so extruding every wing put a duplicate face inside every building
  // where two wings met. The plate is the union at that level: a shared wall
  // is interior to it and simply is not a boundary edge.
  const auto band = [&](const std::vector<Position> &ring, const double base, const double top, std::vector<std::vector<Position>> &out) {
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const Position &a = ring[i];
      const Position &b = ring[(i + 1) % ring.size()];

      if (std::hypot(b.x - a.x, b.z - a.z) < 1e-6)
      {
        continue;
      }

      out.push_back({{a.x, base, a.z}, {b.x, base, b.z}, {b.x, top, b.z}, {a.x, top, a.z}});
    }
  };

  for (const rewrite::DwellingStorey &storey : plan.storeys)
  {
    band(storey.outer, storey.base, storey.top, solid.walls);

    for (const auto &hole : storey.holes)
    {
      band(hole, storey.base, storey.top, solid.innerWalls);
    }
  }

  if (plan.storeys.empty())
  {
    for (const rewrite::DwellingWing &w : plan.wings)
    {
      band(w.footprint, foundation, w.eavesHeight, solid.walls);
    }
  }

  for (const rewrite::DwellingRoofFace &face : plan.roofs)
  {
    (face.soffit ? solid.soffits : face.gable ? solid.gables : solid.slopes).push_back(face.polygon);
  }

  for (const rewrite::DwellingPillar &post : plan.pillars)
  {
    band(post.footprint, post.base, post.top, solid.pillars);

    std::vector<Position> cap;

    for (const Position &q : post.footprint)
    {
      cap.push_back(Position{q.x, post.top, q.z});
    }

    solid.pillars.push_back(std::move(cap));
  }

  solids.push_back(std::move(solid));
}

void writeSolids(const std::string &path)
{
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(4);

  const auto faces = [&](const char *key, const std::vector<std::vector<Position>> &list, const bool last) {
    out << "      \"" << key << "\": [";

    for (size_t f = 0; f < list.size(); ++f)
    {
      out << "[";

      for (size_t v = 0; v < list[f].size(); ++v)
      {
        out << "[" << list[f][v].x << "," << list[f][v].y << "," << list[f][v].z << "]" << (v + 1 < list[f].size() ? "," : "");
      }

      out << "]" << (f + 1 < list.size() ? "," : "");
    }

    out << "]" << (last ? "\n" : ",\n");
  };

  out << "{\n  \"buildings\": [\n";

  for (size_t i = 0; i < solids.size(); ++i)
  {
    out << "    {\n      \"name\": \"" << solids[i].name << "\",\n";
    faces("garden", solids[i].garden, false);
    faces("yard", solids[i].yard, false);
    faces("walls", solids[i].walls, false);
    faces("innerwalls", solids[i].innerWalls, false);
    faces("pillars", solids[i].pillars, false);
    faces("slopes", solids[i].slopes, false);
    faces("soffits", solids[i].soffits, false);
    faces("gables", solids[i].gables, true);
    out << "    }" << (i + 1 < solids.size() ? ",\n" : "\n");
  }

  out << "  ]\n}\n";

  std::ofstream file(path);
  file << out.str();

  std::cout << "wrote " << solids.size() << " solids to " << path << "\n";
}

// ---------------------------------------------------------------------------
// The corpus.
// ---------------------------------------------------------------------------

// A rectangular wing, counter clockwise, with the entrance centred on the
// south edge - the frame the planner documents itself as taking.
rewrite::DwellingBrief wing(const uint64_t id, const double width, const double depth, const rewrite::DwellingUse use, const uint32_t floors)
{
  rewrite::DwellingBrief brief;
  brief.id = id;
  brief.seed = 0x5eed0000ull + id;
  brief.use = use;
  brief.floors = floors;
  brief.shell = {{0, 0, 0}, {width, 0, 0}, {width, 0, depth}, {0, 0, depth}};
  brief.entrance = {width * .5, 0, 0};
  brief.availableHeight = 2.9 * double(floors) + 1.2;

  return brief;
}

const char *useName(const rewrite::DwellingUse use)
{
  switch (use)
  {
  case rewrite::DwellingUse::Cottage:
    return "cottage";
  case rewrite::DwellingUse::ShopHouse:
    return "shop-house";
  case rewrite::DwellingUse::Lodging:
    return "lodging";
  case rewrite::DwellingUse::Hall:
    return "hall";
  case rewrite::DwellingUse::Warehouse:
    return "warehouse";
  }

  return "?";
}

const char *roomName(const rewrite::RoomUse use)
{
  switch (use)
  {
  case rewrite::RoomUse::Living:
    return "living";
  case rewrite::RoomUse::Sleeping:
    return "sleeping";
  case rewrite::RoomUse::Shop:
    return "shop";
  case rewrite::RoomUse::Store:
    return "store";
  case rewrite::RoomUse::Common:
    return "common";
  case rewrite::RoomUse::Service:
    return "service";
  case rewrite::RoomUse::Hall:
    return "hall";
  case rewrite::RoomUse::Corridor:
    return "corridor";
  }

  return "?";
}

const char *roomColour(const rewrite::RoomUse use)
{
  switch (use)
  {
  case rewrite::RoomUse::Living:
    return "#c9a227";
  case rewrite::RoomUse::Sleeping:
    return "#6f8fbf";
  case rewrite::RoomUse::Shop:
    return "#c05a42";
  case rewrite::RoomUse::Store:
    return "#8a7a63";
  case rewrite::RoomUse::Common:
    return "#7fa86b";
  case rewrite::RoomUse::Service:
    return "#9a6fa8";
  case rewrite::RoomUse::Hall:
    return "#d9cbb0";
  case rewrite::RoomUse::Corridor:
    return "#efe7d8";
  }

  return "#999999";
}

const char *furnitureColour(const rewrite::FurnitureKind kind)
{
  switch (kind)
  {
  case rewrite::FurnitureKind::Bed:
    return "#f2ead6";
  case rewrite::FurnitureKind::Table:
    return "#6b4f32";
  case rewrite::FurnitureKind::Counter:
    return "#4a3a28";
  case rewrite::FurnitureKind::Hearth:
    return "#2f2f31";
  case rewrite::FurnitureKind::Chest:
    return "#8a6b45";
  case rewrite::FurnitureKind::Bench:
    return "#9b8158";
  case rewrite::FurnitureKind::Workbench:
    return "#5c4a33";
  }

  return "#777777";
}

// One floor of a plan, drawn: rooms filled by purpose, walls stroked, doors
// dotted, furniture over the top. A floor plan is the only honest way to judge
// a floor plan.
void drawPlan(Panel &p, const rewrite::DwellingBrief &brief, const rewrite::DwellingPlan &plan, const uint32_t floor)
{
  Layer shell{"line", "#20303c", 2.0, "", {}};
  std::vector<Position> ring = brief.shell;
  ring.push_back(brief.shell.front());
  shell.shapes.push_back(ring);
  p.layers.push_back(shell);

  // Rooms, one layer per purpose so the legend can name them.
  std::map<int, std::vector<std::vector<Position>>> byUse;

  for (const rewrite::DwellingRoom &room : plan.rooms)
  {
    if (room.floor == floor && room.boundary.size() >= 3)
    {
      byUse[int(room.use)].push_back(room.boundary);
    }
  }

  for (const auto &[use, rooms] : byUse)
  {
    p.layers.push_back({"fill", roomColour(rewrite::RoomUse(use)), 1, "", rooms});
  }

  Layer walls{"line", "#3a4550", 1.2, "", {}};

  for (const rewrite::DwellingWall &wall : plan.walls)
  {
    if (wall.floor == floor)
    {
      walls.shapes.push_back({wall.a, wall.b});
    }
  }

  p.layers.push_back(walls);

  Layer doors{"point", "#ffffff", 2.4, "", {}};
  Layer exterior{"point", "#d02020", 3.2, "", {}};

  for (const rewrite::DwellingDoor &door : plan.doors)
  {
    if (door.floor != floor)
    {
      continue;
    }

    (door.exterior ? exterior : doors).shapes.push_back({door.center});
  }

  p.layers.push_back(doors);
  p.layers.push_back(exterior);

  // Furniture last, over the rooms: it is what makes a plan readable without
  // reading the legend, which is the whole point of drawing it.
  {
    std::map<int, std::vector<std::vector<Position>>> byKind;

    for (const rewrite::DwellingFurniture &piece : plan.furniture)
    {
      if (piece.floor == floor && piece.footprint.size() >= 3)
      {
        byKind[int(piece.kind)].push_back(piece.footprint);
      }
    }

    for (const auto &[kind, shapes] : byKind)
    {
      p.layers.push_back({"fill", furnitureColour(rewrite::FurnitureKind(kind)), 1, "", shapes});
    }
  }

  for (const auto &[use, rooms] : byUse)
  {
    p.layers.push_back({"legend", roomColour(rewrite::RoomUse(use)), 0, roomName(rewrite::RoomUse(use)), {}});
  }
}

// ---------------------------------------------------------------------------
// Invariants that must hold of any plan, whatever its shape.
// ---------------------------------------------------------------------------

bool insideShell(const Position &p, const rewrite::DwellingBrief &brief)
{
  double minX = brief.shell.front().x, maxX = minX, minZ = brief.shell.front().z, maxZ = minZ;

  for (const Position &v : brief.shell)
  {
    minX = std::min(minX, v.x);
    maxX = std::max(maxX, v.x);
    minZ = std::min(minZ, v.z);
    maxZ = std::max(maxZ, v.z);
  }

  return p.x >= minX - 1e-6 && p.x <= maxX + 1e-6 && p.z >= minZ - 1e-6 && p.z <= maxZ + 1e-6;
}

double ringArea(const std::vector<Position> &ring)
{
  double twice = 0;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const Position &a = ring[i], &b = ring[(i + 1) % ring.size()];
    twice += a.x * b.z - b.x * a.z;
  }

  return std::abs(twice) * .5;
}

} // namespace

// Exteriors composed rather than carved.
//
// Everything above hands the massing an outline and asks it to divide it, so
// the only shapes on offer are the shapes the plots have. This asks it to
// build one, and the question is whether a single rule - lay a wing, push more
// wings off the sides of what stands - actually produces a range of buildings
// or the same block every time.
void composedExteriorCases()
{
  std::cout << "Exteriors composed on a plot\n";

  const auto plot = [](const double width, const double depth) {
    return std::vector<Position>{{0, 0, 0}, {width, 0, 0}, {width, 0, depth}, {0, 0, depth}};
  };

  generator::ComplexDwellingSettings settings;

  std::map<size_t, int> byWings;
  std::map<uint32_t, int> byStoreys;
  int reflex = 0, built = 0, stepped = 0, multiLevel = 0;
  double coverSum = 0;

  for (uint64_t seed = 1; seed <= 240; ++seed)
  {
    const double width = 14 + double(seed % 7) * 4, depth = 12 + double((seed / 7) % 6) * 4;
    const std::vector<Position> ground = plot(width, depth);

    std::vector<rewrite::DwellingWing> wings;
    std::vector<rewrite::DwellingRoofFace> roofs;
    rewrite::DwellingPlan plan;

    generator::massComposedBuilding(ground, seed, .55, .3, 2.9, 3, settings, wings, roofs, &plan.storeys);

    if (wings.empty())
    {
      continue;
    }

    ++built;
    byWings[wings.size()]++;

    double area = 0;

    for (const rewrite::DwellingWing &w : wings)
    {
      double twice = 0;

      for (size_t i = 0; i < w.footprint.size(); ++i)
      {
        const Position &a = w.footprint[i];
        const Position &c = w.footprint[(i + 1) % w.footprint.size()];
        twice += a.x * c.z - c.x * a.z;
      }

      area += std::abs(twice) * .5;
    }

    coverSum += area / (width * depth);
    reflex += wings.size() > 1 ? 1 : 0;

    // How the building changes as it rises: how many distinct storey counts
    // its wings carry, and how many plates the exterior stacks into.
    std::set<uint32_t> heights;

    for (const rewrite::DwellingWing &w : wings)
    {
      heights.insert(w.storeys);
      byStoreys[w.storeys]++;
    }

    stepped += heights.size() > 1 ? 1 : 0;

    std::set<uint32_t> levelsSeen;

    for (const rewrite::DwellingStorey &level : plan.storeys)
    {
      levelsSeen.insert(level.level);
    }

    multiLevel += levelsSeen.size() > 1 ? 1 : 0;

    if (seed <= 12)
    {
      plan.wings = wings;
      plan.roofs = roofs;
      collectSolid("composed-" + std::to_string(seed), plan, .3);
    }
  }

  std::printf("    composed %d plots, mean coverage %.0f%%, wings:", built, 100.0 * coverSum / double(std::max(1, built)));

  for (const auto &[count, howMany] : byWings)
  {
    std::printf(" %zux%d", count, howMany);
  }

  std::printf("\n");
  std::printf("    wings by storeys:");

  for (const auto &[height, count] : byStoreys)
  {
    std::printf(" %ux%d", height, count);
  }

  std::printf("; %d buildings step in height, %d stack more than one plate\n", stepped, multiLevel);

  check(built == 240, "every plot got a building");

  // The building has to change as it rises, or it is a prism with a roof.
  // Width alone gave every wing of a compact house the same height, so the
  // plates stacked to a constant outline however many wings there were.
  check(byStoreys.size() >= 3, "and wings carry a range of heights, not one");
  check(stepped * 4 > built, "and a good share of buildings step in height across their wings");
  check(multiLevel * 2 > built, "and most of them stack more than one storey plate");

  // The point of composing is that the shape is not the plot's. On a
  // rectangle, a building with one wing *is* the plot inset - so if that is
  // most of them, the rule has produced nothing a simple inset would not.
  check(reflex * 2 > built, "and most of them turn a corner rather than filling the plot as one block");

  // ...and a rule that always makes the same thing is a shape by another
  // name. Three distinct wing counts is the least that says otherwise.
  check(byWings.size() >= 3, "and the buildings are not all the same composition");
  check(coverSum / double(std::max(1, built)) < .8, "and none of them swallows its whole plot");
}

// A street of houses, each planned as a plot and built from that plan.
//
// Everything else here exercises one stage at a time. This is the whole chain
// on one lot: a standing decides how much of the ground the house takes and
// how tall it is, the massing composes wings on that ground, the storeys stack
// with each floor jettied out over the one below, an arcade cuts the ground
// floor back behind posts, and the roof is the straight skeleton of whatever
// outline came out. The point is to see a range of buildings rather than one
// building from a range of angles.
void houseShowcaseCases()
{
  std::cout << "A street of houses, plan to solid\n";

  struct Lot
  {
    const char *name;
    generator::PlotStanding standing;
    std::vector<Position> ground;
  };

  const auto rect = [](const double w, const double d) {
    return std::vector<Position>{{0, 0, 0}, {w, 0, 0}, {w, 0, d}, {0, 0, d}};
  };

  const auto ell = [](const double w, const double d, const double bite) {
    return std::vector<Position>{{0, 0, 0}, {w, 0, 0}, {w, 0, d - bite}, {w - bite, 0, d - bite}, {w - bite, 0, d}, {0, 0, d}};
  };

  const auto taper = [](const double w, const double d, const double lean) {
    return std::vector<Position>{{0, 0, 0}, {w, 0, 0}, {w - lean, 0, d}, {lean, 0, d}};
  };

  const std::vector<Lot> street = {
    {"terrace-narrow", generator::PlotStanding::Slum, rect(7.5, 13)},
    {"terrace-deep", generator::PlotStanding::Slum, rect(9, 19)},
    {"craft-corner", generator::PlotStanding::Modest, ell(15, 15, 6)},
    {"craft-wide", generator::PlotStanding::Modest, rect(16, 12)},
    {"merchant-house", generator::PlotStanding::Comfortable, rect(19, 17)},
    {"merchant-wedge", generator::PlotStanding::Comfortable, taper(20, 16, 4)},
    {"patrician-court", generator::PlotStanding::Wealthy, rect(26, 24)},
    {"patrician-ell", generator::PlotStanding::Wealthy, ell(28, 26, 10)},
    {"grand-hall", generator::PlotStanding::Grand, rect(34, 30)},
    {"grand-wings", generator::PlotStanding::Grand, ell(36, 32, 13)},
    {"farmstead", generator::PlotStanding::Rural, rect(30, 26)},
    {"cottage", generator::PlotStanding::Rural, rect(18, 16)},
  };

  generator::ComplexDwellingSettings settings;

  int built = 0, jettied = 0, posted = 0, stepped = 0;
  std::map<uint32_t, int> byStoreys;

  for (size_t i = 0; i < street.size(); ++i)
  {
    const Lot &lot = street[i];

    generator::PlotBrief brief;
    brief.id = 9000 + uint64_t(i);
    brief.seed = 0xB0A7ull + uint64_t(i) * 7919ull;
    brief.standing = lot.standing;
    generator::describeStanding(brief);
    brief.lot = lot.ground;

    // The street runs along the lot's south edge.
    brief.frontage.push_back({lot.ground.front(), lot.ground[1]});

    generator::PlotPlan plan;
    std::string error;

    if (!generator::planPlot(brief, settings, plan, &error))
    {
      check(false, std::string(lot.name) + ": " + error);

      continue;
    }

    if (plan.house.empty())
    {
      check(false, std::string(lot.name) + ": " + plan.note);

      continue;
    }

    ++built;
    byStoreys[plan.storeys]++;

    // Jettied: a floor whose plate reaches further out than the one below it.
    double lowestSpan = 0, highestSpan = 0;
    uint32_t lowest = 0xffffffffu, highest = 0;

    for (const rewrite::DwellingStorey &level : plan.built.levels)
    {
      double span = 0;

      for (const Position &a : level.outer)
      {
        for (const Position &b : level.outer)
        {
          span = std::max(span, std::hypot(a.x - b.x, a.z - b.z));
        }
      }

      if (level.level <= lowest)
      {
        lowest = level.level;
        lowestSpan = std::max(lowestSpan, span);
      }

      if (level.level >= highest)
      {
        highest = level.level;
        highestSpan = std::max(highestSpan, span);
      }
    }

    const bool oversails = highest > lowest && highestSpan > lowestSpan + .05;
    jettied += oversails ? 1 : 0;
    posted += plan.built.pillars.empty() ? 0 : 1;

    std::set<uint32_t> heights;

    for (const rewrite::DwellingWing &w : plan.built.wings)
    {
      heights.insert(w.storeys);
    }

    stepped += heights.size() > 1 ? 1 : 0;

    // Distinct roof planes: a box has four, a plan that turns a corner has
    // more, and that is the skeleton showing.
    std::set<std::tuple<int, int, int>> planes;

    for (const rewrite::DwellingRoofFace &face : plan.built.roofs)
    {
      if (face.polygon.size() < 3 || face.soffit)
      {
        continue;
      }

      const Position &a = face.polygon[0], &b = face.polygon[1], &c = face.polygon[2];
      double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
      double ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
      double nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      const double length = std::sqrt(nx * nx + ny * ny + nz * nz);

      if (length < 1e-9)
      {
        continue;
      }

      nx /= length;
      ny /= length;
      nz /= length;

      if (ny < 0)
      {
        nx = -nx;
        ny = -ny;
        nz = -nz;
      }

      planes.insert({int(std::lround(nx * 20)), int(std::lround(ny * 20)), int(std::lround(nz * 20))});
    }

    // How many separate roof masses the building carries, and how many of its
    // triangles are slivers. A building with three roofs reads as three
    // buildings; a roof that is a tenth slivers reads as torn.
    std::set<uint32_t> masses;

    for (const rewrite::DwellingWing &w : plan.built.wings)
    {
      masses.insert(w.cluster);
    }

    int slivers = 0;

    for (const rewrite::DwellingRoofFace &face : plan.built.roofs)
    {
      if (face.polygon.size() < 3 || face.soffit)
      {
        continue;
      }

      double longest = 0, shortest = 1e30;

      for (size_t i = 0; i < face.polygon.size(); ++i)
      {
        const Position &a = face.polygon[i];
        const Position &b = face.polygon[(i + 1) % face.polygon.size()];
        const double length = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        longest = std::max(longest, length);
        shortest = std::min(shortest, length);
      }

      slivers += longest > 1e-9 && shortest / longest < .05 ? 1 : 0;
    }

    std::printf("    %-16s %u storeys, %zu wings, %zu masses, %zu plates, %zu planes, %zu posts, %zu sheds, %d/%zu slivers%s\n", lot.name, plan.storeys, plan.built.wings.size(), masses.size(), plan.built.levels.size(), planes.size(), plan.built.pillars.size(), plan.sheds.size(), slivers, plan.built.roofs.size(), oversails ? ", jettied" : "");

    // Out as a solid, with the plot's ground under it.
    rewrite::DwellingPlan drawn;
    drawn.wings = plan.built.wings;
    drawn.roofs = plan.built.roofs;
    drawn.storeys = plan.built.levels;
    drawn.pillars = plan.built.pillars;

    for (const generator::PlotBuilding &shed : plan.sheds)
    {
      drawn.wings.insert(drawn.wings.end(), shed.wings.begin(), shed.wings.end());
      drawn.roofs.insert(drawn.roofs.end(), shed.roofs.begin(), shed.roofs.end());
      drawn.storeys.insert(drawn.storeys.end(), shed.levels.begin(), shed.levels.end());
    }

    collectSolid(lot.name, drawn, brief.foundationMetres);

    // The lot itself, so the house is seen on its plot rather than in the air.
    Solid &solid = solids.back();

    for (const auto &piece : plan.garden)
    {
      solid.garden.push_back(piece.outer);
    }

    for (const auto &piece : plan.yard)
    {
      solid.yard.push_back(piece.outer);
    }

    for (const auto &piece : plan.fence)
    {
      std::vector<Position> ring = piece.outer;

      for (size_t k = 0; k < ring.size(); ++k)
      {
        const Position &a = ring[k], &b = ring[(k + 1) % ring.size()];
        solid.walls.push_back({{a.x, 0, a.z}, {b.x, 0, b.z}, {b.x, 1.2, b.z}, {a.x, 1.2, a.z}});
      }
    }
  }

  std::printf("    %d houses; storeys", built);

  for (const auto &[height, count] : byStoreys)
  {
    std::printf(" %ux%d", height, count);
  }

  std::printf("; %d jettied, %d with posts, %d stepping\n", jettied, posted, stepped);

  check(built == int(street.size()), "every lot on the street got a house");
  check(byStoreys.size() >= 2, "and the street is not all one height");
  check(jettied * 2 >= built, "and the upper floors oversail the ground floor");
  check(stepped > 0, "and some of them step in height across their wings");
}

int main(int argc, char **argv)
{
  // -------------------------------------------------------------------------
  // The decline rate, over the corpus.
  // -------------------------------------------------------------------------
  std::cout << "Decline rate over the lot corpus\n";

  struct Tally
  {
    int asked = 0, declined = 0, fellBack = 0;
  };

  std::map<std::string, Tally> byUse;
  Tally total;
  std::vector<std::pair<std::string, std::string>> reasons;

  const rewrite::DwellingUse uses[] = {
    rewrite::DwellingUse::Cottage,
    rewrite::DwellingUse::ShopHouse,
    rewrite::DwellingUse::Lodging,
    rewrite::DwellingUse::Hall,
    rewrite::DwellingUse::Warehouse,
  };

  uint64_t id = 1;

  for (const rewrite::DwellingUse use : uses)
  {
    // The proportions a settlement actually hands the planner: narrow street
    // frontages on deep plots through to broad shallow ones.
    for (double width = 5; width <= 14.0001; width += 1.5)
    {
      for (double depth = 5; depth <= 14.0001; depth += 1.5)
      {
        for (uint32_t floors = 1; floors <= 3; ++floors)
        {
          const rewrite::DwellingBrief brief = wing(id++, width, depth, use, floors);

          rewrite::DwellingPlan plan;
          std::string error;

          ++total.asked;
          ++byUse[useName(use)].asked;

          if (!planDwelling(brief, plan, &error))
          {
            ++total.declined;
            ++byUse[useName(use)].declined;

            if (reasons.size() < 400u)
            {
              reasons.emplace_back(useName(use), error);
            }

            continue;
          }

          if (plan.fallback)
          {
            ++total.fellBack;
            ++byUse[useName(use)].fellBack;
          }
        }
      }
    }
  }

  for (const auto &[name, tally] : byUse)
  {
    std::printf("  %-11s asked %3d  declined %3d (%.0f%%)  fellBack %3d\n", name.c_str(), tally.asked, tally.declined, 100.0 * double(tally.declined) / double(std::max(1, tally.asked)), tally.fellBack);
  }

  std::printf("  %-11s asked %3d  declined %3d (%.0f%%)  fellBack %3d\n", "TOTAL", total.asked, total.declined, 100.0 * double(total.declined) / double(std::max(1, total.asked)), total.fellBack);

  // The reasons, most common first: a decline rate is only actionable with
  // them, and they are what says whether the cause is one rule or many.
  std::map<std::string, int> byReason;

  for (const auto &[name, reason] : reasons)
  {
    ++byReason[reason];
  }

  std::vector<std::pair<int, std::string>> ranked;

  for (const auto &[reason, count] : byReason)
  {
    ranked.emplace_back(count, reason);
  }

  std::sort(ranked.rbegin(), ranked.rend());

  for (size_t i = 0; i < ranked.size() && i < 8u; ++i)
  {
    std::printf("    %4d  %s\n", ranked[i].first, ranked[i].second.c_str());
  }

  // The gate. Watabou's planner assigns what it can and leaves the rest
  // generic; a quarter of a town going unbuilt is not a plan, it is a hole.
  check(total.declined * 5 < total.asked, "the planner declines under a fifth of the lots it is offered");

  // -------------------------------------------------------------------------
  // Invariants, on the plans that were produced.
  // -------------------------------------------------------------------------
  std::cout << "Plan invariants\n";

  int planned = 0, roomsOutside = 0, emptyFloors = 0, roomsWithoutArea = 0, doorsOutside = 0;

  id = 1;

  for (const rewrite::DwellingUse use : uses)
  {
    for (double width = 5; width <= 14.0001; width += 1.5)
    {
      for (double depth = 5; depth <= 14.0001; depth += 1.5)
      {
        for (uint32_t floors = 1; floors <= 3; ++floors)
        {
          const rewrite::DwellingBrief brief = wing(id++, width, depth, use, floors);

          rewrite::DwellingPlan plan;

          if (!planDwelling(brief, plan, nullptr))
          {
            continue;
          }

          ++planned;

          std::map<uint32_t, int> perFloor;

          for (const rewrite::DwellingRoom &room : plan.rooms)
          {
            ++perFloor[room.floor];

            if (ringArea(room.boundary) <= 1e-6)
            {
              ++roomsWithoutArea;
            }

            for (const Position &v : room.boundary)
            {
              if (!insideShell(v, brief))
              {
                ++roomsOutside;
                break;
              }
            }
          }

          for (uint32_t f = 0; f < plan.floors.size(); ++f)
          {
            if (perFloor[f] == 0)
            {
              ++emptyFloors;
            }
          }

          for (const rewrite::DwellingDoor &door : plan.doors)
          {
            if (!insideShell(door.center, brief))
            {
              ++doorsOutside;
            }
          }
        }
      }
    }
  }

  std::printf("  planned %d, rooms outside the shell %d, rooms without area %d, empty floors %d, doors outside %d\n", planned, roomsOutside, roomsWithoutArea, emptyFloors, doorsOutside);

  check(planned > 0, "the corpus produced plans at all");
  check(roomsOutside == 0, "every room lies inside its own shell");
  check(roomsWithoutArea == 0, "no room is degenerate");
  check(emptyFloors == 0, "every floor the plan declares has at least one room");
  check(doorsOutside == 0, "every door lies inside the shell");

  // Furniture: the gate is that a room says what it is for without a legend,
  // which means the rooms that have a purpose carry something.
  {
    int furnished = 0, habitable = 0;
    std::map<std::string, int> byKind;

    uint64_t fid = 1;

    for (const rewrite::DwellingUse use : uses)
    {
      for (double width = 5; width <= 14.0001; width += 1.5)
      {
        for (double depth = 5; depth <= 14.0001; depth += 1.5)
        {
          for (uint32_t floors = 1; floors <= 3; ++floors)
          {
            const rewrite::DwellingBrief brief = wing(fid++, width, depth, use, floors);

            rewrite::DwellingPlan plan;

            if (!planDwelling(brief, plan, nullptr))
            {
              continue;
            }

            std::set<uint64_t> carries;

            for (const rewrite::DwellingFurniture &piece : plan.furniture)
            {
              carries.insert(piece.room);
              ++byKind[furnitureKindName(piece.kind)];
            }

            for (const rewrite::DwellingRoom &room : plan.rooms)
            {
              ++habitable;
              furnished += carries.count(room.id) != 0 ? 1 : 0;
            }
          }
        }
      }
    }

    std::printf("  furniture: %d of %d rooms carry something\n", furnished, habitable);

    for (const auto &[kind, count] : byKind)
    {
      std::printf("    %-10s %d\n", kind.c_str(), count);
    }

    check(furnished * 10 > habitable * 9, "and nine rooms in ten say what they are for without a legend");
  }

  // -------------------------------------------------------------------------
  // The panels: one plan per use, drawn.
  // -------------------------------------------------------------------------
  uint64_t panelId = 9000;

  for (const rewrite::DwellingUse use : uses)
  {
    const rewrite::DwellingBrief brief = wing(panelId++, 11, 9.5, use, use == rewrite::DwellingUse::Warehouse ? 1u : 2u);

    rewrite::DwellingPlan plan;
    std::string error;

    if (!planDwelling(brief, plan, &error))
    {
      Panel &p = panel(std::string("dwelling-") + useName(use), std::string("DECLINED: ") + error);
      Layer shell{"line", "#d02020", 2.0, "", {}};
      std::vector<Position> ring = brief.shell;
      ring.push_back(brief.shell.front());
      shell.shapes.push_back(ring);
      p.layers.push_back(shell);

      continue;
    }

    Panel &p = panel(std::string("dwelling-") + useName(use), std::string(useName(use)) + ": ground floor, rooms by purpose, red dot is the entrance");
    drawPlan(p, brief, plan, 0);
  }

  // The same lot at three sizes, so the effect of the plate on the plan is
  // visible rather than asserted.
  for (const double side : {6.0, 10.0, 14.0})
  {
    const rewrite::DwellingBrief brief = wing(panelId++, side, side, rewrite::DwellingUse::Cottage, 2);

    rewrite::DwellingPlan plan;
    std::string error;

    std::ostringstream name;
    name << "cottage-" << int(side) << "m";

    if (!planDwelling(brief, plan, &error))
    {
      Panel &p = panel(name.str(), std::string("DECLINED: ") + error);
      Layer shell{"line", "#d02020", 2.0, "", {}};
      std::vector<Position> ring = brief.shell;
      ring.push_back(brief.shell.front());
      shell.shapes.push_back(ring);
      p.layers.push_back(shell);

      continue;
    }

    Panel &p = panel(name.str(), "a square cottage at three plate sizes");
    drawPlan(p, brief, plan, 0);
  }

  // A deep, wide plate: every room opens onto the corridor rather than being
  // reached through its neighbour, and each floor is cut on its own.
  {
    const rewrite::DwellingBrief brief = wing(panelId++, 13, 15, rewrite::DwellingUse::Lodging, 3);

    rewrite::DwellingPlan plan;
    std::string error;

    if (planDwelling(brief, plan, &error))
    {
      for (uint32_t floor = 0; floor < plan.floors.size(); ++floor)
      {
        std::ostringstream name;
        name << "corridor-floor-" << floor;

        Panel &p = panel(name.str(), floor == 0 ? "a deep plate: rooms hang off a corridor, not off each other" : "an upper floor, cut on its own");
        drawPlan(p, brief, plan, floor);
      }

      // D2's gate: every room on every floor has a door, and on a plate this
      // deep the corridor is what gives it one.
      int roomsWithoutADoor = 0;

      for (const rewrite::DwellingRoom &room : plan.rooms)
      {
        bool hasDoor = false;

        for (const rewrite::DwellingDoor &door : plan.doors)
        {
          hasDoor = hasDoor || (door.room == room.id && !door.shopOpening);
        }

        roomsWithoutADoor += hasDoor ? 0 : 1;
      }

      std::printf("  corridor: %zu rooms over %zu floors, %d without a door\n", plan.rooms.size(), plan.floors.size(), roomsWithoutADoor);

      check(plan.floors.size() == 3u, "a three storey wing plans three floors");
      check(roomsWithoutADoor == 0, "and every room on every floor has its own door");
    }
    else
    {
      check(false, "the deep corridor wing plans: " + error);
    }
  }

  // Footprints that are not rectangles, which is most of them.
  //
  // A house's outline is whatever the lot left: a trapezoid from a Voronoi
  // block, an L where two streets meet, a U around a yard. The rectangular
  // planner takes a four vertex rectangle and nothing else, so an interior
  // planned for one of these does not line up with the building it is inside.
  {
    std::cout << "Complex footprints\n";

    struct Shape
    {
      const char *name;
      std::vector<Position> outline;
      Position entrance;
      rewrite::DwellingUse use;
      uint32_t floors;
    };

    const std::vector<Shape> shapes = {
      {"house-trapezoid", {{0, 0, 0}, {13, 0, 0}, {10.5, 0, 9}, {1.5, 0, 9}}, {6.5, 0, 0}, rewrite::DwellingUse::Cottage, 2},
      {"house-L", {{0, 0, 0}, {14, 0, 0}, {14, 0, 7}, {6, 0, 7}, {6, 0, 15}, {0, 0, 15}}, {7, 0, 0}, rewrite::DwellingUse::ShopHouse, 2},
      {"manor-U", {{0, 0, 0}, {20, 0, 0}, {20, 0, 16}, {14, 0, 16}, {14, 0, 6}, {6, 0, 6}, {6, 0, 16}, {0, 0, 16}}, {10, 0, 0}, rewrite::DwellingUse::Hall, 2},
      {"house-wedge", {{0, 0, 0}, {15, 0, 0}, {9, 0, 11}, {0, 0, 8}}, {7, 0, 0}, rewrite::DwellingUse::Lodging, 3},
      // A wide body with narrow wings: the case where the ground plan has to
      // decide the height, because the answer differs across the building.
      {"mansion-wings",
       {{0, 0, 0}, {30, 0, 0}, {30, 0, 7}, {22, 0, 7}, {22, 0, 20}, {8, 0, 20}, {8, 0, 7}, {0, 0, 7}},
       {15, 0, 0},
       rewrite::DwellingUse::Hall,
       3},
    };

    for (const Shape &shape : shapes)
    {
      rewrite::DwellingBrief brief;
      brief.id = panelId++;
      brief.seed = 0x51a7ull + brief.id;
      brief.use = shape.use;
      brief.floors = shape.floors;
      brief.shell = shape.outline;
      brief.entrance = shape.entrance;
      brief.availableHeight = 2.9 * double(shape.floors) + 1.2;

      rewrite::DwellingPlan plan;
      std::string error;

      const bool ok = generator::planComplexDwelling(brief, {}, plan, &error);

      check(ok, std::string(shape.name) + " plans on its own footprint: " + error);

      if (!ok)
      {
        continue;
      }

      // The claim: every room is inside the outline, and together they cover
      // nearly all of it. That is what "the interior matches the outer shape"
      // means, and it is measurable.
      double roomArea = 0;
      int outside = 0;

      for (const rewrite::DwellingRoom &room : plan.rooms)
      {
        if (room.floor != 0)
        {
          continue;
        }

        roomArea += room.usableArea;

        for (const Position &v : room.boundary)
        {
          double crossings = 0;

          for (size_t i = 0; i < shape.outline.size(); ++i)
          {
            const Position &a = shape.outline[i];
            const Position &b = shape.outline[(i + 1) % shape.outline.size()];

            if ((a.z > v.z + 1e-6) != (b.z > v.z + 1e-6))
            {
              const double x = a.x + (v.z - a.z) / (b.z - a.z) * (b.x - a.x);
              crossings += x > v.x - 1e-6 ? 1 : 0;
            }
          }

          if (std::fmod(crossings, 2.0) < .5)
          {
            ++outside;
            break;
          }
        }
      }

      double shellArea = 0;

      for (size_t i = 0; i < shape.outline.size(); ++i)
      {
        const Position &a = shape.outline[i];
        const Position &b = shape.outline[(i + 1) % shape.outline.size()];
        shellArea += a.x * b.z - b.x * a.z;
      }

      shellArea = std::abs(shellArea) * .5;

      std::printf("  %-16s %2zu rooms on the ground floor, covering %.0f%% of the footprint, %d outside it\n", shape.name, size_t(std::count_if(plan.rooms.begin(), plan.rooms.end(), [](const rewrite::DwellingRoom &r) { return r.floor == 0; })), 100.0 * roomArea / shellArea, outside);

      // Doors on corridors, which is the point of having corridors.
      //
      // A door is counted as being on the circulation when the room it serves
      // shares a wall with a corridor. Anything else is a room you reach by
      // walking through another room.
      {
        std::map<uint64_t, const rewrite::DwellingRoom *> byId;

        for (const rewrite::DwellingRoom &room : plan.rooms)
        {
          byId[room.id] = &room;
        }

        int onCorridor = 0, roomsServed = 0, corridorRooms = 0;

        for (const rewrite::DwellingRoom &room : plan.rooms)
        {
          if (room.floor != 0)
          {
            continue;
          }

          if (room.use == rewrite::RoomUse::Corridor)
          {
            ++corridorRooms;
            continue;
          }

          ++roomsServed;

          // Does this room share a run of wall with any corridor?
          bool touches = false;

          for (const rewrite::DwellingRoom &other : plan.rooms)
          {
            if (other.floor != 0 || other.use != rewrite::RoomUse::Corridor || touches)
            {
              continue;
            }

            for (size_t i = 0; i < room.boundary.size() && !touches; ++i)
            {
              const Position &p = room.boundary[i];
              const Position &q = room.boundary[(i + 1) % room.boundary.size()];
              const double ex = q.x - p.x, ez = q.z - p.z;
              const double length = std::hypot(ex, ez);

              if (length < .9)
              {
                continue;
              }

              for (size_t j = 0; j < other.boundary.size() && !touches; ++j)
              {
                const Position &a = other.boundary[j];
                const Position &c = other.boundary[(j + 1) % other.boundary.size()];
                const double sx = c.x - a.x, sz = c.z - a.z;
                const double slength = std::hypot(sx, sz);

                if (slength < .9)
                {
                  continue;
                }

                if (std::abs(sx * (p.z - a.z) - sz * (p.x - a.x)) / slength > .25 || std::abs(sx * (q.z - a.z) - sz * (q.x - a.x)) / slength > .25)
                {
                  continue;
                }

                const double t0 = ((p.x - a.x) * sx + (p.z - a.z) * sz) / slength;
                const double t1 = ((q.x - a.x) * sx + (q.z - a.z) * sz) / slength;

                touches = std::min(slength, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1)) >= .9;
              }
            }
          }

          onCorridor += touches ? 1 : 0;
        }

        double corridorArea = 0, floorArea = 0;

        for (const rewrite::DwellingRoom &room : plan.rooms)
        {
          if (room.floor != 0)
          {
            continue;
          }

          floorArea += room.usableArea;
          corridorArea += room.use == rewrite::RoomUse::Corridor ? room.usableArea : 0.0;
        }

        // The narrowest corridor, as twice its area over its perimeter - the
        // width of a strip. Measuring the centre's distance to the nearest
        // wall instead was wrong for any corridor that is not convex: on an L
        // or a cross the centre sits near a wall and the answer came out zero
        // for a corridor that is perfectly walkable.
        double narrowest = 1e300;

        for (const rewrite::DwellingRoom &room : plan.rooms)
        {
          if (room.floor != 0 || room.use != rewrite::RoomUse::Corridor || room.boundary.size() < 3)
          {
            continue;
          }

          double twiceArea = 0, perimeter = 0;

          for (size_t i = 0; i < room.boundary.size(); ++i)
          {
            const Position &a = room.boundary[i];
            const Position &b = room.boundary[(i + 1) % room.boundary.size()];

            twiceArea += a.x * b.z - b.x * a.z;
            perimeter += std::hypot(b.x - a.x, b.z - a.z);
          }

          if (perimeter > 1e-6)
          {
            narrowest = std::min(narrowest, std::abs(twiceArea) / perimeter);
          }
        }

        std::printf("    %-16s %d corridor pieces (%.0f%% of the floor), %d of %d rooms open onto one, narrowest %.2f m\n", shape.name, corridorRooms, 100.0 * corridorArea / std::max(1.0, floorArea), onCorridor, roomsServed, narrowest > 1e299 ? 0.0 : narrowest);

        if (corridorRooms > 0)
        {
          check(narrowest >= .9, std::string(shape.name) + ": every corridor is wide enough to walk down");
        }

        // Both numbers together are the claim. Circulation that reaches every
        // room is easy if you are allowed to spend half the house on it, and
        // cheap circulation is easy if it need not reach anything.
        check(corridorArea < floorArea * .30, std::string(shape.name) + ": the corridor is circulation, not most of the house");

        check(corridorRooms > 0, std::string(shape.name) + ": the plan has a corridor");
        check(onCorridor * 4 >= roomsServed * 3, std::string(shape.name) + ": three rooms in four open onto the corridor");

        // An internal room is one with no wall on the outside of the building.
        // It can only exist where two corridors bracket a bay, which is why a
        // subdivision that only ever cuts a piece in two cannot produce one.
        int internal = 0;

        for (const rewrite::DwellingRoom &room : plan.rooms)
        {
          if (room.floor != 0 || room.use == rewrite::RoomUse::Corridor)
          {
            continue;
          }

          bool onOutside = false;

          for (size_t i = 0; i < room.boundary.size() && !onOutside; ++i)
          {
            const Position &p = room.boundary[i];
            const Position &q = room.boundary[(i + 1) % room.boundary.size()];

            for (size_t j = 0; j < shape.outline.size() && !onOutside; ++j)
            {
              const Position &a = shape.outline[j];
              const Position &c = shape.outline[(j + 1) % shape.outline.size()];
              const double sx = c.x - a.x, sz = c.z - a.z;
              const double slength = std::hypot(sx, sz);

              if (slength < 1e-9)
              {
                continue;
              }

              onOutside = std::abs(sx * (p.z - a.z) - sz * (p.x - a.x)) / slength < .4 && std::abs(sx * (q.z - a.z) - sz * (q.x - a.x)) / slength < .4;
            }
          }

          internal += onOutside ? 0 : 1;
        }

        std::printf("    %-16s %d of %d rooms are internal\n", shape.name, internal, roomsServed);

        // Massing: the storeys a place carries come from the ground plan, so
        // an upper floor stands on less ground than the one below it.
        std::vector<double> perFloor(plan.floors.size(), 0.0);

        for (const rewrite::DwellingRoom &room : plan.rooms)
        {
          if (room.floor < perFloor.size())
          {
            perFloor[room.floor] += room.usableArea;
          }
        }

        // What the programme actually produced. The point of a list of room
        // types is that a house gets the rooms it needs, so the mix is the
        // claim.
        {
          std::map<std::string, int> mix;

          for (const rewrite::DwellingRoom &room : plan.rooms)
          {
            if (room.floor == 0)
            {
              ++mix[roomName(room.use)];
            }
          }

          std::printf("    %-16s ground floor:", shape.name);

          for (const auto &[name, count] : mix)
          {
            std::printf(" %s=%d", name.c_str(), count);
          }

          std::printf("%s\n", plan.note.empty() ? "" : ("  [" + plan.note + "]").c_str());

          check(mix.size() >= 3u, std::string(shape.name) + ": the plan has a mix of room types, not one repeated");
        }

        std::printf("    %-16s storeys", shape.name);

        for (size_t f = 0; f < perFloor.size(); ++f)
        {
          std::printf(" %.0f m2", perFloor[f]);
        }

        std::printf("\n");

        // An upper storey never stands on more ground than the one below it.
        // Whether it stands on *less* depends on the plan: a plate that is
        // wide everywhere carries its full height everywhere, and a wing that
        // is one room wide carries one storey. The mansion below is the case
        // that mixes the two.
        if (perFloor.size() > 1)
        {
          check(perFloor[1] <= perFloor[0] * 1.001, std::string(shape.name) + ": an upper storey stands on no more ground than the one below it");

          // ...and on a plan that mixes widths, strictly less: the wings are
          // one storey and the body is three.
          if (std::string(shape.name) == "mansion-wings")
          {
            check(perFloor[1] < perFloor[0] * .85, "mansion-wings: the wings carry fewer storeys than the body");
          }
        }
      }

      check(outside == 0, std::string(shape.name) + ": every room lies inside the outline");
      check(roomArea > shellArea * .75, std::string(shape.name) + ": the rooms cover the footprint");

      // Every floor of the mansion, so a multi-storey plan can be read as one.
      if (std::string(shape.name) == "mansion-wings")
      {
        for (uint32_t f = 0; f < plan.floors.size(); ++f)
        {
          std::map<std::string, int> mix;

          for (const rewrite::DwellingRoom &room : plan.rooms)
          {
            if (room.floor == f)
            {
              ++mix[roomName(room.use)];
            }
          }

          std::printf("    mansion floor %u:", f);

          for (const auto &[name, count] : mix)
          {
            std::printf(" %s=%d", name.c_str(), count);
          }

          std::printf("\n");

          std::ostringstream name;
          name << "mansion-floor-" << f;

          Panel &floorPanel = panel(name.str(), f == 0 ? "ground: hall, service and stores" : "above: the body only, and the household sleeps here");

          Layer outlineLayer{"line", "#8a8478", 1.2, "", {}};
          std::vector<Position> outlineRing = shape.outline;
          outlineRing.push_back(shape.outline.front());
          outlineLayer.shapes.push_back(outlineRing);
          floorPanel.layers.push_back(outlineLayer);

          drawPlan(floorPanel, brief, plan, f);
        }

        // The household sleeps above the ground floor, not on it.
        int groundBeds = 0, upperBeds = 0;

        for (const rewrite::DwellingRoom &room : plan.rooms)
        {
          if (room.use == rewrite::RoomUse::Sleeping)
          {
            (room.floor == 0 ? groundBeds : upperBeds)++;
          }
        }

        check(upperBeds > groundBeds * 2, "mansion-wings: the bedrooms are upstairs, not on the ground floor");
      }

      // The outside: wings and their ridges, over the ground floor plan, so
      // the one-one match between exterior and floor plan can be seen.
      {
        std::map<uint32_t, int> byStoreys;
        double covered = 0;

        for (const rewrite::DwellingWing &w : plan.wings)
        {
          ++byStoreys[w.storeys];

          double twice = 0;

          for (size_t i = 0; i < w.footprint.size(); ++i)
          {
            const Position &a = w.footprint[i];
            const Position &c = w.footprint[(i + 1) % w.footprint.size()];
            twice += a.x * c.z - c.x * a.z;
          }

          covered += std::abs(twice) * .5;
        }

        std::printf("    %-16s %zu wings covering %.0f%% of the footprint, storeys:", shape.name, plan.wings.size(), 100.0 * covered / shellArea);

        for (const auto &[storeys, count] : byStoreys)
        {
          std::printf(" %ux%u", count, storeys);
        }

        std::printf("\n");

        check(!plan.wings.empty(), std::string(shape.name) + ": the building has wings");
        check(covered > shellArea * .95, std::string(shape.name) + ": the wings are the footprint, seen from above");

        // A cluster is the set of wings roofed as one mass, and the roofer
        // will span it. So every cluster has to be one connected piece of
        // building at one height - two wings at opposite ends of a manor
        // cannot share a roof however alike they are.
        std::map<uint32_t, std::vector<const rewrite::DwellingWing *>> byCluster;

        for (const rewrite::DwellingWing &w : plan.wings)
        {
          byCluster[w.cluster].push_back(&w);
        }

        bool clustersSound = true;

        for (const auto &[id, group] : byCluster)
        {
          for (const rewrite::DwellingWing *w : group)
          {
            clustersSound = clustersSound && w->storeys == group.front()->storeys;
          }

          // Connected: walk the group from its first wing through wings that
          // touch, and every one has to be reachable.
          std::set<const rewrite::DwellingWing *> seen{group.front()};
          std::vector<const rewrite::DwellingWing *> frontier{group.front()};

          while (!frontier.empty())
          {
            const rewrite::DwellingWing *w = frontier.back();
            frontier.pop_back();

            for (const rewrite::DwellingWing *other : group)
            {
              if (seen.count(other))
              {
                continue;
              }

              bool touches = false;

              for (const Position &a : w->footprint)
              {
                for (const Position &b : other->footprint)
                {
                  touches = touches || std::hypot(a.x - b.x, a.z - b.z) <= .6;
                }
              }

              if (touches)
              {
                seen.insert(other);
                frontier.push_back(other);
              }
            }
          }

          clustersSound = clustersSound && seen.size() == group.size();
        }

        // The roof, as surfaces rather than as a ridge line.
        //
        // Three things have to hold or the exterior is not a building. Seen
        // from above the roof has to cover the walls and then some, because it
        // oversails them. Nothing may stand above its own mass's ridge. And a
        // face called a gable has to be far steeper than a slope, which is
        // what separates the two in the data.
        double roofPlan = 0;
        int gables = 0, slopes = 0;
        bool roofSound = !plan.roofs.empty();

        for (const rewrite::DwellingRoofFace &face : plan.roofs)
        {
          double twice = 0, lowest = 1e9, highest = -1e9;

          for (size_t i = 0; i < face.polygon.size(); ++i)
          {
            const Position &a = face.polygon[i];
            const Position &c = face.polygon[(i + 1) % face.polygon.size()];
            twice += a.x * c.z - c.x * a.z;
            lowest = std::min(lowest, a.y);
            highest = std::max(highest, a.y);
          }

          roofPlan += std::abs(twice) * .5;

          double nx = 0, ny = 0, nz = 0;

          for (size_t i = 0; i < face.polygon.size(); ++i)
          {
            const Position &u = face.polygon[i];
            const Position &v = face.polygon[(i + 1) % face.polygon.size()];
            nx += (u.y - v.y) * (u.z + v.z);
            ny += (u.z - v.z) * (u.x + v.x);
            nz += (u.x - v.x) * (u.y + v.y);
          }

          const double normalLength = std::sqrt(nx * nx + ny * ny + nz * nz);

          const rewrite::DwellingWing &w = plan.wings[face.wing];
          roofSound = roofSound && highest <= w.ridgeHeight + 1e-6 && lowest >= w.eavesHeight - 1e-6;

          // Flat. A face from a skewed eaves edge up to the ridge is a warped
          // quad - four corners not in a plane - which draws as something
          // plausible and is not a surface. A fifth of the town's roof faces
          // were warped before they were split into triangles.
          if (normalLength > 1e-9)
          {
            const double px = nx / normalLength, py = ny / normalLength, pz = nz / normalLength;
            const double plane = px * face.polygon[0].x + py * face.polygon[0].y + pz * face.polygon[0].z;

            for (const Position &v : face.polygon)
            {
              roofSound = roofSound && std::abs(px * v.x + py * v.y + pz * v.z - plane) < .02;
            }
          }

          // A gable is steep, not necessarily vertical.
          //
          // "No extent in plan" was the test, and it was right only while a
          // gable was an edge of weight exactly zero. Such an edge never
          // collapses, so the wavefront could stall on it; gables are a small
          // positive weight now - a clipped gable, which is a real roof - and
          // they lean in slightly. What still separates one from a slope is
          // that it is far steeper, so that is what is measured.
          // A gable stands over its own eaves edge: its face is vertical, so
          // its normal is horizontal and it has no extent in plan.
          if (face.gable && normalLength > 1e-9)
          {
            roofSound = roofSound && std::abs(ny) / normalLength < .05;
          }

          gables += face.gable ? 1 : 0;
          slopes += face.gable ? 0 : 1;
        }

        // ...and the roof has to reach the walls. Its plan - every face
        // projected to the ground and unioned - must contain each wing's own
        // ground; what it leaves uncovered is wall standing under open sky.
        // The 3D view was built to catch exactly this, and a slot between a
        // wing's roof and its neighbour's wall is what it showed.
        double uncovered = 0;

        {
          std::vector<geom2::Operand> faces;

          for (const rewrite::DwellingRoofFace &face : plan.roofs)
          {
            geom2::Polygon flat;

            for (const Position &v : face.polygon)
            {
              flat.outer.push_back({geom2::toUnit(v.x), geom2::toUnit(v.z)});
            }

            if (flat.outer.size() >= 3 && geom2::polygonFault(flat).empty())
            {
              faces.push_back(geom2::toOperand(flat));
            }
          }

          std::vector<geom2::Polygon> roofUnion;

          if (!faces.empty() && geom2::unionOf(faces, roofUnion, nullptr))
          {
            std::vector<geom2::Operand> clips;

            for (const geom2::Polygon &piece : roofUnion)
            {
              clips.push_back(geom2::toOperand(piece));
            }

            for (const rewrite::DwellingWing &w : plan.wings)
            {
              geom2::Polygon ground;

              for (const Position &q : w.footprint)
              {
                ground.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
              }

              std::vector<geom2::Polygon> bare;

              if (geom2::differenceOf(geom2::toOperand(ground), clips, bare, nullptr))
              {
                for (const geom2::Polygon &piece : bare)
                {
                  uncovered += geom2::areaMetres(piece);
                }
              }
            }
          }
        }

        std::printf("    %-16s %zu roof faces (%d sloping, %d gable) over %.0f%% of the walls, %.2f m2 of wall under open sky\n", shape.name, plan.roofs.size(), slopes, gables, 100.0 * roofPlan / shellArea, uncovered);
        check(uncovered < .5, std::string(shape.name) + ": the roof reaches every wall");

        // How many distinct planes the roof is made of.
        //
        // The roof this replaced leaned every eaves edge onto one ridge
        // segment, so a plan of any shape got the planes of a single hipped
        // box - four at most, and on an L that meant a ridge running through a
        // wall. A straight skeleton gives one plane per eaves edge and finds
        // the valleys between them on its own, so a plan that turns a corner
        // has more planes than a box can have, and counting them is the test
        // that the skeleton is what built it.
        std::set<std::tuple<int, int, int>> planes;

        for (const rewrite::DwellingRoofFace &face : plan.roofs)
        {
          if (face.polygon.size() < 3)
          {
            continue;
          }

          const Position &a = face.polygon[0], &b = face.polygon[1], &c = face.polygon[2];
          const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
          const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
          double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
          const double length = std::sqrt(nx * nx + ny * ny + nz * nz);

          if (length < 1e-9)
          {
            continue;
          }

          nx /= length;
          ny /= length;
          nz /= length;

          if (ny < 0)
          {
            nx = -nx;
            ny = -ny;
            nz = -nz;
          }

          planes.insert({int(std::lround(nx * 24)), int(std::lround(ny * 24)), int(std::lround(nz * 24))});
        }

        std::printf("    %-16s roof is %zu distinct planes\n", shape.name, planes.size());

        if (std::string(shape.name) == "house-L" || std::string(shape.name) == "manor-U" || std::string(shape.name) == "mansion-wings")
        {
          check(planes.size() > 4, std::string(shape.name) + ": a plan that turns a corner gets more roof planes than a box has");
        }

        check(roofSound, std::string(shape.name) + ": every roof face is flat and runs from its eaves to its own ridge");
        check(roofPlan > shellArea, std::string(shape.name) + ": the roof oversails the walls it covers");

        collectSolid(shape.name, plan, 0.0);

        std::printf("    %-16s %zu roof clusters over %zu wings\n", shape.name, byCluster.size(), plan.wings.size());
        check(clustersSound, std::string(shape.name) + ": each roof cluster is one connected mass at one height");

        Panel &massing = panel(std::string(shape.name) + "-massing", "wings and roof ridges: the exterior is the plan seen from above");

        std::map<uint32_t, std::vector<std::vector<Position>>> wingsByStoreys;

        for (const rewrite::DwellingWing &w : plan.wings)
        {
          wingsByStoreys[w.storeys].push_back(w.footprint);
        }

        static const char *shades[] = {"#c8bda8", "#a89a80", "#87795f", "#6a5d47"};

        for (const auto &[storeys, rings] : wingsByStoreys)
        {
          massing.layers.push_back({"fill", shades[std::min<size_t>(3, storeys - 1)], 1, "", rings});
          massing.layers.push_back({"legend", shades[std::min<size_t>(3, storeys - 1)], 0, std::to_string(storeys) + (storeys == 1 ? " storey" : " storeys"), {}});
        }

        Layer seams{"line", "#4a4136", 1.0, "", {}};
        Layer ridges{"line", "#2b2520", 2.2, "", {}};

        for (const rewrite::DwellingWing &w : plan.wings)
        {
          std::vector<Position> ring = w.footprint;
          ring.push_back(w.footprint.front());
          seams.shapes.push_back(ring);
          ridges.shapes.push_back({w.ridgeFrom, w.ridgeTo});
        }

        massing.layers.push_back(seams);
        massing.layers.push_back(ridges);
      }

      Panel &p = panel(shape.name, "the interior is the shape of the building, not a rectangle inside it");

      Layer outline{"line", "#20303c", 2.0, "", {}};
      std::vector<Position> ring = shape.outline;
      ring.push_back(shape.outline.front());
      outline.shapes.push_back(ring);
      p.layers.push_back(outline);

      drawPlan(p, brief, plan, 0);
    }
  }

  // Compounds: a castle and a manor, each a set of wings around a court, every
  // inhabited one planned by the same rules as a house. Towers included - a
  // tower is a small plate with a stair in the corner of each room, which is
  // exactly what the smallest cottages are.
  {
    std::cout << "Compounds\n";

    for (const auto &[name, kind, side] : {std::tuple<const char *, generator::CompoundKind, double>{"castle", generator::CompoundKind::Castle, 46.0}, {"manor", generator::CompoundKind::Manor, 34.0}})
    {
      generator::CompoundBrief brief;
      brief.id = 7000 + uint64_t(side);
      brief.seed = 991;
      brief.kind = kind;
      brief.precinct = {{0, 0, 0}, {side, 0, 0}, {side, 0, side}, {0, 0, side}};
      brief.approach = {side * .5, 0, 0};

      generator::CompoundPlan compound;
      generator::Ledger ledger;
      std::string error;

      const bool ok = generator::planCompound(brief, [](double, double) { return 0.0; }, compound, ledger, &error);

      int planned = 0, floors = 0, rooms = 0;

      for (const generator::CompoundWing &w : compound.wings)
      {
        planned += w.planned ? 1 : 0;
        floors += w.planned ? int(w.plan.floors.size()) : 0;
        rooms += w.planned ? int(w.plan.rooms.size()) : 0;
      }

      std::printf("  %s: %zu wings, %d planned, %d floors, %d rooms, %zu towers\n", name, compound.wings.size(), planned, floors, rooms, compound.towers.size());

      check(ok && planned == int(compound.wings.size()), std::string(name) + ": every wing is a planned interior: " + error);
      check(floors > planned, std::string(name) + ": some wings rise more than one storey");

      Panel &p = panel(std::string("compound-") + name, "wings, towers and courts, each interior cut by the dwelling planner");

      Layer precinct{"line", "#8a8478", 1.4, "", {}};
      std::vector<Position> ring = brief.precinct;
      ring.push_back(brief.precinct.front());
      precinct.shapes.push_back(ring);
      p.layers.push_back(precinct);

      Layer courts{"fill", "#d7dcc9", 1, "", {}};

      for (const auto &court : compound.courts)
      {
        courts.shapes.push_back(court);
      }

      p.layers.push_back(courts);

      Layer walls{"line", "#5d5a52", 3.0, "", {}};

      for (const generator::CompoundWall &wall : compound.walls)
      {
        walls.shapes.push_back({wall.a, wall.b});
      }

      p.layers.push_back(walls);

      Layer towers{"fill", "#a9a294", 1, "", {}};

      for (const generator::CompoundTower &tower : compound.towers)
      {
        towers.shapes.push_back(tower.footprint);
      }

      p.layers.push_back(towers);

      // Every wing's ground floor, drawn with the same rules as a house.
      for (const generator::CompoundWing &w : compound.wings)
      {
        if (w.planned)
        {
          drawPlan(p, w.brief, w.plan, 0);
        }
      }
    }
  }

  if (argc > 1)
  {
  composedExteriorCases();
  houseShowcaseCases();

    writeGallery(argv[1]);

    std::string solidPath = argv[1];
    const size_t dot = solidPath.rfind('.');
    solidPath = (dot == std::string::npos ? solidPath : solidPath.substr(0, dot)) + "-solids.json";
    writeSolids(solidPath);
  }

  if (failures > 0)
  {
    std::cerr << failures << " dwelling gallery test(s) failed\n";

    return 1;
  }

  return 0;
}
