// The cell planner's contact sheet, phases nought and one.
//
// Two rows of panels. The first shows a lot being framed: the boundary, the
// setback, the grid laid over it, and which of its cells may be built on. The
// second shows what got built there.
//
// The renders are not decoration. Every invariant in `CellDwellingTests` can
// pass on a sheet of buildings that are all the same dull rectangle in the
// middle of every lot, and the only way to see that is to look.

#include "procedural/geom2/CellGrid.hpp"
#include "procedural/geom2/CellShape.hpp"
#include "procedural/geom2/Offset.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"
#include "procedural/world/generator/CellCompound.hpp"
#include "procedural/world/generator/CellDwellingClasses.hpp"
#include "procedural/world/generator/CellDwellingDoors.hpp"
#include "procedural/world/generator/CellDwellingOpenings.hpp"
#include "procedural/world/generator/CellDwellingProgramme.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"
#include "procedural/world/generator/CellDwellingRoof.hpp"
#include "procedural/world/hierarchy/CellDwellingGenerator.hpp"
#include "procedural/world/hierarchy/CellStructure.hpp"
#include "procedural/world/hierarchy/CellElevation.hpp"
#include "procedural/world/generator/CellDwellingStairs.hpp"
#include "procedural/world/generator/CellBlock.hpp"
#include "procedural/world/generator/FortressJunction.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <string>
#include <vector>

using namespace procedural;
using namespace procedural::geom2;
using namespace procedural::world::generator;
namespace rewrite = procedural::world::rewrite;

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

struct Shape
{
  std::vector<std::pair<double, double>> points;
};

struct Layer
{
  std::string kind;
  std::string colour;
  double width = 1;
  std::vector<Shape> shapes;
};

struct Panel
{
  std::string name;
  std::string note;
  std::vector<Layer> layers;
};

std::string quote(const std::string &text)
{
  return "\"" + text + "\"";
}

void write(const std::vector<Panel> &panels, const std::string &path)
{
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(3);
  out << "{\n  \"panels\": [\n";

  for (size_t p = 0; p < panels.size(); ++p)
  {
    out << "    {\n      \"name\": " << quote(panels[p].name) << ",\n";
    out << "      \"note\": " << quote(panels[p].note) << ",\n";
    out << "      \"layers\": [\n";

    for (size_t l = 0; l < panels[p].layers.size(); ++l)
    {
      const Layer &layer = panels[p].layers[l];
      out << "        {\"kind\": " << quote(layer.kind) << ", \"colour\": " << quote(layer.colour)
          << ", \"width\": " << layer.width << ", \"shapes\": [";

      for (size_t s = 0; s < layer.shapes.size(); ++s)
      {
        out << "[";

        for (size_t k = 0; k < layer.shapes[s].points.size(); ++k)
        {
          out << "[" << layer.shapes[s].points[k].first << "," << layer.shapes[s].points[k].second << "]";

          if (k + 1 < layer.shapes[s].points.size())
          {
            out << ",";
          }
        }

        out << "]";

        if (s + 1 < layer.shapes.size())
        {
          out << ",";
        }
      }

      out << "]}";

      if (l + 1 < panels[p].layers.size())
      {
        out << ",";
      }

      out << "\n";
    }

    out << "      ]\n    }";

    if (p + 1 < panels.size())
    {
      out << ",";
    }

    out << "\n";
  }

  out << "  ]\n}\n";

  std::ofstream file(path);
  file << out.str();
}

Shape ringShape(const Ring &ring)
{
  Shape out;

  for (const Point &p : ring)
  {
    out.points.emplace_back(toMetres(p.x), toMetres(p.y));
  }

  if (!ring.empty())
  {
    out.points.emplace_back(toMetres(ring.front().x), toMetres(ring.front().y));
  }

  return out;
}

Shape cellSquare(const CellGrid &grid, const CellFrame &frame, const uint32_t i, const uint32_t j)
{
  Shape out;
  const int32_t corners[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};

  for (const auto &corner : corners)
  {
    double x = 0;
    double z = 0;
    nodePoint(grid, frame, grid.node(int32_t(i) + corner[0], int32_t(j) + corner[1]), x, z);
    out.points.emplace_back(x, z);
  }

  out.points.push_back(out.points.front());

  return out;
}

Layer cellsLayer(const CellGrid &grid,
                 const CellFrame &frame,
                 const Board &board,
                 const std::string &kind,
                 const std::string &colour,
                 const double width)
{
  Layer layer{kind, colour, width, {}};

  for (const auto &[i, j] : cells(board))
  {
    layer.shapes.push_back(cellSquare(grid, frame, i, j));
  }

  return layer;
}

Ring ringOfMetres(const std::vector<std::pair<double, double>> &points)
{
  Ring out;

  for (const auto &[x, z] : points)
  {
    out.push_back(Point{toUnit(x), toUnit(z)});
  }

  if (!counterClockwise(out))
  {
    reverse(out);
  }

  return out;
}

Polygon rectangleLot(const double width, const double depth, const double turn = 0)
{
  Polygon out;
  std::vector<std::pair<double, double>> points;

  for (const auto &[u, v] : std::vector<std::pair<double, double>>{{0, 0}, {width, 0}, {width, depth}, {0, depth}})
  {
    points.emplace_back(u * std::cos(turn) - v * std::sin(turn), u * std::sin(turn) + v * std::cos(turn));
  }

  out.outer = ringOfMetres(points);

  return out;
}

std::vector<std::pair<std::string, Polygon>> corpus()
{
  std::vector<std::pair<std::string, Polygon>> out;

  out.emplace_back("rectangle", rectangleLot(18, 14));
  out.emplace_back("turned", rectangleLot(26, 20, .6));
  out.emplace_back("deep", rectangleLot(12, 24));

  {
    Polygon wedge;
    wedge.outer = ringOfMetres({{0, 0}, {22, 0}, {17, 16}, {0, 13}});
    out.emplace_back("wedge", wedge);
  }

  {
    Polygon corner;
    corner.outer = ringOfMetres({{0, 0}, {20, 0}, {20, 18}, {12, 18}, {12, 9}, {0, 9}});
    out.emplace_back("L corner", corner);
  }

  {
    Polygon clipped;
    clipped.outer = ringOfMetres({{0, 0}, {19, 0}, {23, 5}, {23, 17}, {0, 17}});
    out.emplace_back("clipped", clipped);
  }

  return out;
}

Polygon wellAt(const double x, const double z, const double side)
{
  Polygon out;
  out.outer = ringOfMetres({{x, z}, {x + side, z}, {x + side, z + side}, {x, z + side}});

  return out;
}

} // namespace

int main(int argc, char **argv)
{
  std::cout << "CellDwellingGalleryTests\n";

  const std::string path = argc > 1 ? argv[1] : "cell-dwelling-gallery.json";

  CellDwellingSettings settings;
  std::vector<Panel> panels;
  uint32_t framed = 0;
  uint32_t built = 0;
  uint32_t declined = 0;

  // --- phase 0: the lots, framed ------------------------------------------
  for (const auto &[label, lot] : corpus())
  {
    for (uint32_t variant = 0; variant < 2; ++variant)
    {
      Ledger ledger;
      CellDwellingBrief brief;
      brief.id = 1;
      brief.seed = 1;
      brief.lot = lot;
      brief.frontage = {0};
      brief.setbackMetres = 1.0;

      if (variant == 1)
      {
        brief.obstacles.push_back(wellAt(9, 7, 4));
      }

      CellGrid grid;
      CellFrame frame;
      SiteBoard site;

      if (!frameLot(brief, settings, grid, frame, site, ledger, nullptr))
      {
        ++declined;
        continue;
      }

      ++framed;

      Panel panel;
      panel.name = "frame " + label + (variant == 1 ? " with a well" : "");

      std::ostringstream note;
      note << grid.width() << "x" << grid.height() << " cells at " << frame.cellMetres << " m, "
           << site.buildable.count() << " buildable";
      panel.note = note.str();

      panel.layers.push_back(cellsLayer(grid, frame, site.buildable, "fill", "#dfe6da", 0));
      panel.layers.push_back(cellsLayer(grid, frame, site.obstacle, "fill", "#bdb8ae", 0));
      panel.layers.push_back(cellsLayer(grid, frame, grid.mask(), "line", "#e2e0da", .02));

      // The inscribed rectangle: what anchored the lattice.
      CellBounds core;

      if (largestRectangle(grid, site.buildable, core))
      {
        Board box;

        for (uint32_t i = core.i0; i <= core.i1; ++i)
        {
          for (uint32_t j = core.j0; j <= core.j1; ++j)
          {
            box.set(i, j);
          }
        }

        Chain loop = grid.outerLoop(box);
        Ring ring;

        if (ringOf(grid, frame, loop, ring, nullptr))
        {
          panel.layers.push_back(Layer{"line", "#7fa050", .08, {ringShape(ring)}});
        }
      }

      std::vector<Polygon> inset;
      offsetPolygon(lot, -brief.setbackMetres, OffsetOptions{}, inset, nullptr);

      if (!inset.empty())
      {
        panel.layers.push_back(Layer{"line", "#a8a49a", .05, {ringShape(inset.front().outer)}});
      }

      panel.layers.push_back(Layer{"line", "#3c3e44", .12, {ringShape(lot.outer)}});

      for (const Polygon &obstacle : brief.obstacles)
      {
        panel.layers.push_back(Layer{"line", "#8a7f70", .1, {ringShape(obstacle.outer)}});
      }

      panels.push_back(std::move(panel));
    }
  }

  // --- phase 1: footprints and doors ---------------------------------------
  for (const auto &[label, lot] : corpus())
  {
    for (uint64_t seed = 1; seed <= 8; ++seed)
    {
      Ledger ledger;
      CellHouse house;
      CellDwellingBrief brief;
      brief.id = seed;
      brief.seed = seed;
      brief.lot = lot;
      brief.frontage = {0};
      brief.setbackMetres = 1.0;

      if (!planCellDwellingShell(brief, settings, house, ledger, nullptr))
      {
        ++declined;
        continue;
      }

      ++built;

      const CellFloor &ground = house.floors.front();

      Panel panel;
      panel.name = "footprint " + label + " seed " + std::to_string(seed);

      std::ostringstream note;
      note << ground.area.count() << " cells, " << mergeCollinear(house.grid, ground.loops.front()).size()
           << " corners, " << narrow(house.grid, ground.area).count() << " narrow";
      panel.note = note.str();

      panel.layers.push_back(cellsLayer(house.grid, house.frame, house.site.buildable, "fill", "#f0eee8", 0));
      panel.layers.push_back(cellsLayer(house.grid, house.frame, ground.area, "fill", "#c8d4e0", 0));
      panel.layers.push_back(cellsLayer(house.grid, house.frame, narrow(house.grid, ground.area), "fill", "#e4d3b8", 0));
      panel.layers.push_back(cellsLayer(house.grid, house.frame, house.grid.mask(), "line", "#e2e0da", .02));
      panel.layers.push_back(Layer{"line", "#3c3e44", .1, {ringShape(lot.outer)}});

      Ring ring;

      if (ringOf(house.grid, house.frame, ground.loops.front(), ring, nullptr))
      {
        panel.layers.push_back(Layer{"line", "#3c3e44", .45, {ringShape(ring)}});
      }

      // The door, drawn as the piece of wall it takes out plus a mark outside.
      {
        double ax = 0, az = 0, bx = 0, bz = 0;
        edgePoint(house.grid, house.frame, house.entrance, .2, ax, az);
        edgePoint(house.grid, house.frame, house.entrance, .8, bx, bz);
        Shape leaf;
        leaf.points.emplace_back(ax, az);
        leaf.points.emplace_back(bx, bz);
        panel.layers.push_back(Layer{"line", "#a03020", .3, {leaf}});

        double mx = 0, mz = 0;
        edgePoint(house.grid, house.frame, house.entrance, .5, mx, mz);
        Shape mark;
        mark.points.emplace_back(mx, mz);
        panel.layers.push_back(Layer{"point", "#a03020", 5, {mark}});
      }

      panels.push_back(std::move(panel));
    }
  }

  // --- phase 2: the floor stack --------------------------------------------
  {
    const std::vector<std::pair<std::string, Polygon>> plots = {
      {"rectangle", rectangleLot(20, 16)}, {"wedge", corpus()[3].second}, {"L corner", corpus()[4].second}};

    for (const auto &[label, lot] : plots)
    {
      for (uint64_t seed = 1; seed <= 30 && built < 400; ++seed)
      {
        Ledger ledger;
        CellHouse house;
        CellDwellingBrief brief;
        brief.id = seed;
        brief.seed = seed;
        brief.lot = lot;
        brief.frontage = {0};
        brief.setbackMetres = 1.0;
        brief.tags = {"tall"};

        if (!planCellDwelling(brief, settings, house, ledger, nullptr))
        {
          continue;
        }

        if (house.floors.size() < 3)
        {
          continue;
        }

        ++built;

        std::vector<const CellFloor *> ordered;

        if (house.basement.has_value())
        {
          ordered.push_back(&house.basement.value());
        }

        for (const CellFloor &floor : house.floors)
        {
          ordered.push_back(&floor);
        }

        for (const CellFloor *floor : ordered)
        {
          Panel panel;
          panel.name = "stack " + label + " seed " + std::to_string(seed) + " floor " +
                       std::to_string(floor->level);

          std::ostringstream note;
          note << floor->area.count() << " cells, compactness "
               << int(compactness(floor->area) * 100) << "%";
          panel.note = note.str();

          panel.layers.push_back(cellsLayer(house.grid, house.frame, house.site.buildable, "fill", "#f4f2ec", 0));

          // The ground floor under every panel, so a setback reads as a step
          // rather than as a different building.
          panel.layers.push_back(
            cellsLayer(house.grid, house.frame, house.floors.front().area, "fill", "#e8e5dd", 0));

          const std::string tint = floor->level < 0 ? "#cfc4b4" : "#c8d4e0";
          panel.layers.push_back(cellsLayer(house.grid, house.frame, floor->area, "fill", tint, 0));
          panel.layers.push_back(cellsLayer(house.grid, house.frame, house.grid.mask(), "line", "#e2e0da", .02));

          Ring below;

          if (ringOf(house.grid, house.frame, house.floors.front().loops.front(), below, nullptr))
          {
            panel.layers.push_back(Layer{"line", "#d8d5cd", .12, {ringShape(below)}});
          }

          Ring ring;

          if (ringOf(house.grid, house.frame, floor->loops.front(), ring, nullptr))
          {
            panel.layers.push_back(Layer{"line", "#3c3e44", .45, {ringShape(ring)}});
          }

          // The room graph, between room centroids, so an unreachable room is
          // a room with no line to it.
          const auto centroid = [&](const uint32_t room) {
            double cx = 0;
            double cz = 0;
            uint32_t count = 0;

            for (const auto &[i, j] : cells(floor->roomAreas[room]))
            {
              double x = 0;
              double z = 0;
              cellPoint(house.grid, house.frame, house.grid.cell(int32_t(i), int32_t(j)), x, z);
              cx += x;
              cz += z;
              ++count;
            }

            return std::pair<double, double>{count == 0 ? 0. : cx / double(count),
                                             count == 0 ? 0. : cz / double(count)};
          };

          for (const CellDoor &door : floor->doors)
          {
            const auto a = centroid(door.a);
            const auto b = centroid(door.b);
            Shape link;
            link.points.emplace_back(a.first, a.second);
            link.points.emplace_back(b.first, b.second);
            panel.layers.push_back(Layer{"line", "#7fa050", .05, {link}});
          }

          // The windows, in the wall line.
          for (const uint32_t e : floor->windows)
          {
            double ax = 0, az = 0, bx = 0, bz = 0;
            edgePoint(house.grid, house.frame, e, .15, ax, az);
            edgePoint(house.grid, house.frame, e, .85, bx, bz);
            Shape glass;
            glass.points.emplace_back(ax, az);
            glass.points.emplace_back(bx, bz);
            panel.layers.push_back(Layer{"line", "#3878a8", .3, {glass}});
          }

          // The doors. An arch is the full cell and a leaf is a gap in it, so
          // they are drawn at different widths: the difference is the point.
          for (const CellDoor &door : floor->doors)
          {
            const double from = door.kind == DoorKind::Open ? .05 : .28;
            const double to = door.kind == DoorKind::Open ? .95 : .72;
            double ax = 0, az = 0, bx = 0, bz = 0;
            edgePoint(house.grid, house.frame, door.edge, from, ax, az);
            edgePoint(house.grid, house.frame, door.edge, to, bx, bz);
            Shape leaf;
            leaf.points.emplace_back(ax, az);
            leaf.points.emplace_back(bx, bz);
            panel.layers.push_back(
              Layer{"line", door.kind == DoorKind::Open ? "#c89070" : "#a03020", .22, {leaf}});
          }

          for (const CellStair &stair : floor->stairs)
          {
            Board one;
            house.grid.add(one, stair.cell);
            const char *tint = stair.form == StairForm::Trapdoor ? "#8a7f70" : "#b49ac8";
            panel.layers.push_back(cellsLayer(house.grid, house.frame, one, "fill", tint, 0));

            if (stair.form != StairForm::Trapdoor)
            {
              double cx = 0, cz = 0;
              cellPoint(house.grid, house.frame, stair.cell, cx, cz);
              const double dj = double(deltaJ(stair.dir)) * house.frame.cellMetres * .4;
              const double di = double(deltaI(stair.dir)) * house.frame.cellMetres * .4;
              double ax = 0, az = 0, bx = 0, bz = 0;
              framePoint(house.frame, 0, 0, ax, az);
              framePoint(house.frame, dj / house.frame.cellMetres, di / house.frame.cellMetres, bx, bz);
              Shape arrow;
              arrow.points.emplace_back(cx - (bx - ax) * .5, cz - (bz - az) * .5);
              arrow.points.emplace_back(cx + (bx - ax) * .5, cz + (bz - az) * .5);
              panel.layers.push_back(Layer{"line", stair.up ? "#7a4fa0" : "#a08050", .2, {arrow}});
            }
          }

          if (floor->level == 0)
          {
            double mx = 0, mz = 0;
            edgePoint(house.grid, house.frame, house.entrance, .5, mx, mz);
            Shape mark;
            mark.points.emplace_back(mx, mz);
            panel.layers.push_back(Layer{"point", "#a03020", 5, {mark}});
          }

          panels.push_back(std::move(panel));
        }
      }
    }
  }

  // --- phases 3 and 4: the climb, and the rooms ------------------------------
  {
    const std::vector<std::pair<std::string, Polygon>> plots = {
      {"rectangle", rectangleLot(22, 18)}, {"wedge", corpus()[3].second}, {"L corner", corpus()[4].second}};

    const char *const tints[8] = {"#c8d4e0", "#d9cbe4", "#cfe0d0", "#e4d3b8",
                                  "#d8ccc4", "#c4d8d8", "#e0d0d0", "#d0d8c4"};

    for (const auto &[label, lot] : plots)
    {
      for (uint64_t seed = 1; seed <= 40; ++seed)
      {
        Ledger ledger;
        CellHouse house;
        CellDwellingBrief brief;
        brief.id = seed;
        brief.seed = seed;
        brief.lot = lot;
        brief.frontage = {0};
        brief.setbackMetres = 1.0;
        brief.tags = {"tall"};

        if (!planCellDwelling(brief, settings, house, ledger, nullptr) || house.floors.size() < 2)
        {
          continue;
        }

        if (house.scheme == StairScheme::PerFloor)
        {
          continue;
        }

        ++built;

        std::vector<const CellFloor *> ordered;

        for (const CellFloor &floor : house.floors)
        {
          ordered.push_back(&floor);
        }

        if (house.basement.has_value())
        {
          ordered.push_back(&house.basement.value());
        }

        for (const CellFloor *floor : ordered)
        {
          Panel panel;
          panel.name = std::string("rooms ") + name(house.scheme) + " " + label + " seed " +
                       std::to_string(seed) + " floor " + std::to_string(floor->level);

          std::ostringstream note;

          for (size_t r = 0; r < floor->roomKinds.size(); ++r)
          {
            note << (r == 0 ? "" : ", ")
                 << (floor->roomKinds[r] == rewrite::RoomKind::Count ? "room"
                                                                    : rewrite::roomKindName(floor->roomKinds[r]));
          }

          note << " | " << floor->doors.size() << " doors, " << floor->stairs.size() << " stairs, "
               << name(house.scheme);
          panel.note = note.str();

          panel.layers.push_back(cellsLayer(house.grid, house.frame, house.site.buildable, "fill", "#f4f2ec", 0));

          // Coloured by what the room is FOR, not by its index: a sheet where
          // the kitchens are all one colour is a sheet you can read.
          for (size_t r = 0; r < floor->roomAreas.size(); ++r)
          {
            const char *tint = tints[r % 8];

            if (r < floor->roomUses.size())
            {
              switch (floor->roomUses[r])
              {
                case rewrite::RoomUse::Living: tint = "#c8d4e0"; break;
                case rewrite::RoomUse::Sleeping: tint = "#d9cbe4"; break;
                case rewrite::RoomUse::Service: tint = "#e4d3b8"; break;
                case rewrite::RoomUse::Store: tint = "#d8ccc4"; break;
                case rewrite::RoomUse::Common: tint = "#cfe0d0"; break;
                case rewrite::RoomUse::Hall: tint = "#e0d6c0"; break;
                case rewrite::RoomUse::Corridor: tint = "#eceae4"; break;
                default: break;
              }
            }

            panel.layers.push_back(cellsLayer(house.grid, house.frame, floor->roomAreas[r], "fill", tint, 0));
          }

          // The narrow cells, which are what the subdivision scores on and what
          // the corridors will be made of.
          panel.layers.push_back(
            cellsLayer(house.grid, house.frame, narrow(house.grid, floor->area), "point", "#c08030", 3));

          panel.layers.push_back(cellsLayer(house.grid, house.frame, house.grid.mask(), "line", "#e2e0da", .02));

          // The inner walls.
          for (const Chain &wall : floor->innerWalls)
          {
            for (const uint32_t e : wall)
            {
              double ax = 0, az = 0, bx = 0, bz = 0;
              edgePoint(house.grid, house.frame, e, 0, ax, az);
              edgePoint(house.grid, house.frame, e, 1, bx, bz);
              Shape line;
              line.points.emplace_back(ax, az);
              line.points.emplace_back(bx, bz);
              panel.layers.push_back(Layer{"line", "#3c3e44", .15, {line}});
            }
          }

          Ring ring;

          if (ringOf(house.grid, house.frame, floor->loops.front(), ring, nullptr))
          {
            panel.layers.push_back(Layer{"line", "#3c3e44", .45, {ringShape(ring)}});
          }

          // The reservation: the stair cell, or the turret's shaft.
          if (house.stairwell.has_value() && house.grid.contains(floor->area, house.stairwell->stair))
          {
            Board one;
            house.grid.add(one, house.stairwell->stair);
            panel.layers.push_back(cellsLayer(house.grid, house.frame, one, "fill", "#b49ac8", 0));

            double ax = 0, az = 0, bx = 0, bz = 0;
            const uint32_t exit = house.grid.cellEdge(house.stairwell->stair, house.stairwell->exit);
            edgePoint(house.grid, house.frame, exit, .15, ax, az);
            edgePoint(house.grid, house.frame, exit, .85, bx, bz);
            Shape gap;
            gap.points.emplace_back(ax, az);
            gap.points.emplace_back(bx, bz);
            panel.layers.push_back(Layer{"line", "#7a4fa0", .25, {gap}});
          }

          if (house.spiral.has_value())
          {
            double cx = 0, cz = 0;
            nodePoint(house.grid, house.frame, house.spiral->node, cx, cz);
            const double radius = .7 * house.frame.cellMetres;
            Shape circle;

            for (uint32_t t = 0; t <= 32; ++t)
            {
              const double a = 2. * 3.14159265358979 * double(t) / 32.;
              circle.points.emplace_back(cx + radius * std::cos(a), cz + radius * std::sin(a));
            }

            panel.layers.push_back(Layer{"line", "#7a4fa0", .3, {circle}});
          }

          if (floor->level == 0)
          {
            double mx = 0, mz = 0;
            edgePoint(house.grid, house.frame, house.entrance, .5, mx, mz);
            Shape mark;
            mark.points.emplace_back(mx, mz);
            panel.layers.push_back(Layer{"point", "#a03020", 5, {mark}});
          }

          panels.push_back(std::move(panel));
        }
      }
    }
  }

  // --- phase 9: the roofs ---------------------------------------------------
  {
    const std::vector<std::pair<std::string, Polygon>> plots = {
      {"rectangle", rectangleLot(22, 18)}, {"wedge", corpus()[3].second}, {"L corner", corpus()[4].second}};

    for (const auto &[label, lot] : plots)
    {
      for (uint64_t seed = 1; seed <= 12; ++seed)
      {
        Ledger ledger;
        CellHouse house;
        CellDwellingBrief brief;
        brief.id = seed;
        brief.seed = seed;
        brief.lot = lot;
        brief.frontage = {0};
        brief.setbackMetres = 1.0;
        brief.tags = {"tall"};

        if (!planCellDwelling(brief, settings, house, ledger, nullptr) || house.floors.size() < 2)
        {
          continue;
        }

        Stream stream(seed * 2654435761u + 7u);
        CellRoofs roofs;

        if (!planRoofs(house, settings, stream, roofs, ledger, nullptr) || roofs.faces.empty())
        {
          continue;
        }

        ++built;

        Panel panel;
        panel.name = "roof " + label + " seed " + std::to_string(seed);

        std::ostringstream note;
        note << roofs.faces.size() << " faces, " << roofs.chimneys.size() << " chimneys, ridge "
             << int(roofs.summitHeight.back() * 100) / 100. << " m over the top storey";
        panel.note = note.str();

        panel.layers.push_back(cellsLayer(house.grid, house.frame, house.floors.front().area, "fill", "#f0eee8", 0));

        // Each face shaded by how high it climbs: a roof drawn flat in plan is
        // still a roof if you can see which way it falls.
        double highest = 1e-9;

        for (const rewrite::DwellingRoofFace &face : roofs.faces)
        {
          for (const rewrite::Position &p : face.polygon)
          {
            highest = std::max(highest, p.y);
          }
        }

        for (const rewrite::DwellingRoofFace &face : roofs.faces)
        {
          double mean = 0;

          for (const rewrite::Position &p : face.polygon)
          {
            mean += p.y;
          }

          mean /= double(face.polygon.size());
          const int shade = int(180. + 70. * mean / highest);
          char tint[8];
          std::snprintf(tint, sizeof(tint), "#%02x%02x%02x", shade, shade - 20, shade - 45);

          Shape poly;

          for (const rewrite::Position &p : face.polygon)
          {
            poly.points.emplace_back(p.x, p.z);
          }

          poly.points.push_back(poly.points.front());
          panel.layers.push_back(Layer{"fill", face.gable ? "#8a7f70" : tint, 0, {poly}});
        }

        // The eaves of every storey, so the stack reads.
        for (const CellFloor &floor : house.floors)
        {
          Ring ring;

          if (ringOf(house.grid, house.frame, floor.loops.front(), ring, nullptr))
          {
            panel.layers.push_back(Layer{"line", "#3c3e44", .12, {ringShape(ring)}});
          }
        }

        for (const rewrite::DwellingPillar &stack : roofs.chimneys)
        {
          Shape square;

          for (const rewrite::Position &p : stack.footprint)
          {
            square.points.emplace_back(p.x, p.z);
          }

          square.points.push_back(square.points.front());
          panel.layers.push_back(Layer{"fill", "#4a4038", 0, {square}});
        }

        panels.push_back(std::move(panel));
      }
    }
  }

  // --- jetty and balcony ----------------------------------------------------
  {
    const Polygon lot = rectangleLot(24, 20);
    CellDwellingSettings withBalcony = settings;
    withBalcony.balconyDepthMetres = 1.4;

    const auto ringOfPositions = [](const std::vector<rewrite::Position> &ring) {
      Shape out;

      for (const rewrite::Position &p : ring)
      {
        out.points.emplace_back(p.x, p.z);
      }

      if (!ring.empty())
      {
        out.points.emplace_back(ring.front().x, ring.front().z);
      }

      return out;
    };

    uint32_t shown = 0;

    for (uint64_t seed = 1; seed <= 60 && shown < 6; ++seed)
    {
      Ledger ledger;
      CellHouse house;
      CellDwellingBrief brief;
      brief.id = seed;
      brief.seed = seed;
      brief.lot = lot;
      brief.frontage = {0};
      brief.setbackMetres = 1.0;
      brief.tags = {"tall"};

      if (!planCellDwelling(brief, withBalcony, house, ledger, nullptr) || house.floors.size() < 2)
      {
        continue;
      }

      const double perFloor = house.floors.size() == 2 ? .6 : .45;
      const double step = std::min(perFloor, withBalcony.jettyTotalMetres / double(house.floors.size() - 1));
      house.jettyMetres.assign(house.floors.size(), 0.);

      for (size_t k = 1; k < house.jettyMetres.size(); ++k)
      {
        house.jettyMetres[k] = step;
      }

      rewrite::DwellingPlan plan;

      if (!realiseCellDwelling(house, withBalcony, plan, nullptr))
      {
        continue;
      }

      ++shown;
      ++built;

      const size_t base = house.basement.has_value() ? 1u : 0u;

      // One panel per storey, as the stack sheet does. Drawing every storey
      // into one panel put four or five rings on top of each other that differ
      // only on the street wall, and a reader sees a bundle of stray parallel
      // segments rather than a building that oversails.
      for (size_t k = base; k < plan.storeys.size(); ++k)
      {
        Panel panel;
        panel.name = "projection seed " + std::to_string(seed) + " floor " + std::to_string(k - base);

        std::ostringstream note;
        note << "jetty " << step << " m per floor, " << (k - base) * step << " m out from the ground";

        if (k == base + 1 && !plan.pillars.empty())
        {
          note << ", balcony on " << plan.pillars.size() << " posts";
        }

        panel.note = note.str();

        panel.layers.push_back(cellsLayer(house.grid, house.frame, house.site.buildable, "fill", "#f4f2ec", 0));
        panel.layers.push_back(Layer{"line", "#3c3e44", .1, {ringShape(lot.outer)}});

        // The ground floor, ghosted, so the oversail is measured against
        // something that does not move.
        panel.layers.push_back(Layer{"line", "#d8d5cd", .3, {ringOfPositions(plan.storeys[base].outer)}});

        // The storey below, so one step of the jetty is visible rather than
        // the whole accumulated stack of them.
        if (k > base)
        {
          panel.layers.push_back(Layer{"line", "#a8a49a", .2, {ringOfPositions(plan.storeys[k - 1].outer)}});
        }

        panel.layers.push_back(Layer{"line", "#3c3e44", .45, {ringOfPositions(plan.storeys[k].outer)}});

        // The balcony sits on the first floor and is drawn on its panel only.
        if (k == base + 1)
        {
          for (const rewrite::DwellingFloor &floor : plan.floors)
          {
            for (const auto &slab : floor.slabs)
            {
              panel.layers.push_back(Layer{"fill", "#d8c9a8", 0, {ringOfPositions(slab)}});
            }
          }

          for (const rewrite::DwellingPillar &post : plan.pillars)
          {
            panel.layers.push_back(Layer{"fill", "#8a7f70", 0, {ringOfPositions(post.footprint)}});
          }

          for (const rewrite::DwellingWall &rail : plan.walls)
          {
            Shape line;
            line.points.emplace_back(rail.a.x, rail.a.z);
            line.points.emplace_back(rail.b.x, rail.b.z);
            panel.layers.push_back(Layer{"line", "#a0642a", .16, {line}});
          }
        }

        if (k == base)
        {
          double mx = 0, mz = 0;
          edgePoint(house.grid, house.frame, house.entrance, .5, mx, mz);
          Shape mark;
          mark.points.emplace_back(mx, mz);
          panel.layers.push_back(Layer{"point", "#a03020", 5, {mark}});
        }

        panels.push_back(std::move(panel));
      }
    }
  }

  // --- how a household stands on its plot -----------------------------------
  {
    const Polygon deep = rectangleLot(22, 30);
    const std::vector<std::pair<std::string, PlotStanding>> standings = {
      {"slum", PlotStanding::Slum},
      {"modest", PlotStanding::Modest},
      {"comfortable", PlotStanding::Comfortable},
      {"wealthy", PlotStanding::Wealthy},
      {"grand", PlotStanding::Grand},
      {"rural", PlotStanding::Rural}};

    for (const auto &[label, wanted] : standings)
    {
      for (uint64_t seed = 1; seed <= 4; ++seed)
      {
        Ledger ledger;
        CellHouse house;
        CellDwellingBrief brief;
        brief.id = seed;
        brief.seed = seed;
        brief.lot = deep;
        brief.frontage = {0};
        brief.setbackMetres = 1.0;
        brief.standing = wanted;

        if (!planCellDwellingShell(brief, settings, house, ledger, nullptr))
        {
          ++declined;
          continue;
        }

        ++built;
        const CellFloor &ground = house.floors.front();

        Panel panel;
        panel.name = "standing " + label + " seed " + std::to_string(seed);

        std::ostringstream note;
        note << ground.area.count() << " cells, set back "
             << int(frontSetbackShare(wanted) * 100) << "% of the plot";
        panel.note = note.str();

        panel.layers.push_back(cellsLayer(house.grid, house.frame, house.site.buildable, "fill", "#f0eee8", 0));
        panel.layers.push_back(cellsLayer(house.grid, house.frame, ground.area, "fill", "#c8d4e0", 0));
        panel.layers.push_back(cellsLayer(house.grid, house.frame, house.grid.mask(), "line", "#e2e0da", .02));
        panel.layers.push_back(Layer{"line", "#3c3e44", .1, {ringShape(deep.outer)}});

        Ring ring;

        if (ringOf(house.grid, house.frame, ground.loops.front(), ring, nullptr))
        {
          panel.layers.push_back(Layer{"line", "#3c3e44", .45, {ringShape(ring)}});
        }

        double mx = 0, mz = 0;
        edgePoint(house.grid, house.frame, house.entrance, .5, mx, mz);
        Shape mark;
        mark.points.emplace_back(mx, mz);
        panel.layers.push_back(Layer{"point", "#a03020", 5, {mark}});

        panels.push_back(std::move(panel));
      }
    }
  }

  // --- phase 13: the classes ------------------------------------------------
  //
  // One row per building class, and the two things to look for are what the
  // invariants cannot see. The first is whether the classes are actually
  // different buildings rather than the same plan at three sizes. The second is
  // the corridor: drawn in its own colour, it either reads as a route from the
  // door to the far end of the plan or it reads as a stripe somebody painted
  // through the middle of a floor, and no measurement tells the two apart.
  {
    const char *const tints[8] = {"#c8d4e0", "#d9cbe4", "#cfe0d0", "#e4d3b8",
                                  "#d5d5c4", "#e0cccc", "#c6dbdb", "#ded6c0"};

    for (uint32_t c = 0; c < uint32_t(BuildingClass::Count); ++c)
    {
      const BuildingClass building = BuildingClass(c);
      const ClassProfile &profile = profileFor(building);

      // A lot with room to spare, so what is drawn is the class rather than the
      // ground refusing it.
      const double side = std::sqrt(double(profile.grid.maximumCells) * 2.6) * profile.grid.cellMetres + 6.;

      // A church gets a deep churchyard: a nave runs away from the road, and on
      // a lot wider than it is deep the cross cannot be turned to face the
      // street at all.
      const bool axial = profile.grid.footprint == FootprintKind::Axial;
      const Polygon lot = rectangleLot(axial ? side * 1.05 : side, axial ? side * 1.15 : side * .8);

      uint32_t shown = 0;

      for (uint64_t seed = 1; seed <= 40 && shown < 5; ++seed)
      {
        Ledger ledger;
        CellHouse house;
        CellDwellingBrief brief;
        brief.id = seed;
        brief.seed = seed;
        brief.lot = lot;
        brief.frontage = {0};
        brief.setbackMetres = 1.0;
        brief.building = building;
        brief.standing = profile.standing;

        if (!planCellDwelling(brief, settings, house, ledger, nullptr))
        {
          continue;
        }

        ++shown;
        ++built;

        const CellFloor &floor = house.floors.front();

        Panel panel;
        panel.name = std::string(name(building)) + " seed " + std::to_string(seed);

        // The seed leads the note. It is the only thing on a contact sheet that
        // lets a building be asked about again - "the odd one is castle seed
        // seven" is a bug report, "one of the castles looks wrong" is not - and
        // the panel name sits at the bottom in small type where nobody reads it.
        // Measured, not intended. Whether the class *may* come out symmetric is
        // in the table; whether this one DID is a property of the footprint in
        // front of you, and it is the second that a sheet is read for.
        bool mirrored = true;

        {
          const CellBounds box = bounds(floor.area);

          for (const auto &[i, j] : cells(floor.area))
          {
            mirrored = mirrored && floor.area.test(i, box.j0 + box.j1 - j);
          }
        }

        std::ostringstream note;
        note << "seed " << seed << " \u00b7 " << floor.area.count() << " cells \u00b7 " << house.floors.size()
             << " storeys \u00b7 " << floor.roomAreas.size() << " rooms \u00b7 "
             << (floor.circulation.empty() ? std::string("no spine")
                                           : std::to_string(floor.circulation.count()) + " corridor")
             << (house.courts.empty() ? "" : " \u00b7 " + std::to_string(house.courts.count()) + " court")
             << (house.stairwell.has_value() && house.stairwell->block.count() > 1
                   ? " \u00b7 " + std::to_string(house.stairwell->block.count()) + "-cell stair tower"
                   : "")
             << (mirrored ? " \u00b7 mirrored" : "");
        panel.note = note.str();

        panel.layers.push_back(cellsLayer(house.grid, house.frame, house.site.buildable, "fill", "#f4f2ec", 0));

        // The court, under the rooms, in the colour open ground is drawn in
        // everywhere else on this sheet. It is not part of any floor, so it can
        // only show up as the hole it is.
        panel.layers.push_back(cellsLayer(house.grid, house.frame, house.courts, "fill", "#dce6d4", 0));

        for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
        {
          const bool outdoors = r < floor.roomOutdoor.size() && floor.roomOutdoor[r] != 0;
          const bool spine = !floor.circulation.empty() && floor.roomAreas[r] == floor.circulation;

          // A court is a room now, so it takes a tint like any other unless it
          // is drawn as what it is. Open ground is green on every other panel
          // of this sheet and it has to stay green here.
          panel.layers.push_back(cellsLayer(house.grid, house.frame, floor.roomAreas[r], "fill",
                                            outdoors ? "#dce6d4" : (spine ? "#e8c890" : tints[r % 8]), 0));
        }

        // The stair, over the rooms, because at castle scale it is a tower and
        // a tower you cannot pick out of a contact sheet may as well not be
        // one.
        if (house.stairwell.has_value())
        {
          panel.layers.push_back(
            cellsLayer(house.grid, house.frame, house.stairwell->block, "fill", "#b49ac8", 0));
        }

        // Every loop of the floor, so a court reads as a court and not as a
        // room somebody forgot to colour.
        for (const Chain &loop : floor.loops)
        {
          Ring ring;

          if (ringOf(house.grid, house.frame, loop, ring, nullptr))
          {
            panel.layers.push_back(Layer{"line", "#3c3e44", .45, {ringShape(ring)}});
          }
        }

        for (const CellDoor &door : floor.doors)
        {
          const double from = door.kind == DoorKind::Open ? .05 : .28;
          const double to = door.kind == DoorKind::Open ? .95 : .72;
          double ax = 0, az = 0, bx = 0, bz = 0;
          edgePoint(house.grid, house.frame, door.edge, from, ax, az);
          edgePoint(house.grid, house.frame, door.edge, to, bx, bz);
          Shape leaf;
          leaf.points.emplace_back(ax, az);
          leaf.points.emplace_back(bx, bz);
          panel.layers.push_back(
            Layer{"line", door.kind == DoorKind::Open ? "#c89070" : "#a03020", .22, {leaf}});
        }

        double mx = 0, mz = 0;
        edgePoint(house.grid, house.frame, house.entrance, .5, mx, mz);
        Shape mark;
        mark.points.emplace_back(mx, mz);
        panel.layers.push_back(Layer{"point", "#a03020", 5, {mark}});

        panels.push_back(std::move(panel));
      }
    }
  }

  // --- phase 14: the compounds ----------------------------------------------
  //
  // The precinct, the wall with its gate, the towers, the courts and every
  // wing's ground floor in place. What to look for is the ground between the
  // buildings: a castle whose ward you cannot cross, or a manor whose court is
  // a corridor between two ranges, passes every invariant in the suite and is
  // still not the building it claims to be.
  {
    const char *const tints[8] = {"#c8d4e0", "#d9cbe4", "#cfe0d0", "#e4d3b8",
                                  "#d5d5c4", "#e0cccc", "#c6dbdb", "#ded6c0"};

    const std::vector<std::pair<CellCompoundKind, double>> kinds = {{CellCompoundKind::Castle, 72.},
                                                                   {CellCompoundKind::Manor, 46.},
                                                                   {CellCompoundKind::Church, 40.},
                                                                   {CellCompoundKind::Hall, 34.}};

    for (const auto &[kind, width] : kinds)
    {
      for (uint64_t seed = 1; seed <= 4; ++seed)
      {
        CellCompoundBrief brief;
        brief.id = seed;
        brief.seed = seed;
        brief.kind = kind;
        brief.precinct = rectangleLot(width, width * .8);
        brief.approach = Point{toUnit(width * .5), toUnit(-4.)};

        Ledger ledger;
        CellCompound compound;

        if (!planCellCompound(brief, settings, compound, ledger, nullptr))
        {
          continue;
        }

        ++built;

        Panel panel;
        panel.name = std::string(name(kind)) + " compound seed " + std::to_string(seed);

        std::ostringstream note;
        note << "seed " << seed << " \u00b7 " << compound.plannedWings() << " of " << compound.wings.size()
             << " wings, " << compound.walls.size() << " wall runs, " << compound.towers.size() << " towers, "
             << compound.courts.size() << " courts";
        panel.note = note.str();

        panel.layers.push_back(Layer{"fill", "#f4f2ec", 0, {ringShape(compound.precinct.outer)}});

        // The open ground, under everything, so a wing standing on a court
        // would be visible as a building on a green.
        for (const Polygon &court : compound.courts)
        {
          panel.layers.push_back(Layer{"fill", "#e6ece0", 0, {ringShape(court.outer)}});

          for (const Ring &hole : court.holes)
          {
            panel.layers.push_back(Layer{"fill", "#f4f2ec", 0, {ringShape(hole)}});
          }
        }

        for (const Polygon &tower : compound.towers)
        {
          panel.layers.push_back(Layer{"fill", "#b0a69a", 0, {ringShape(tower.outer)}});
        }

        // The wall, with the gate drawn as a gap in it rather than as a line of
        // another colour: a gate you can see through is a gate.
        for (const CellCurtain &wall : compound.walls)
        {
          if (wall.gate)
          {
            continue;
          }

          Shape run;
          run.points.emplace_back(toMetres(wall.a.x), toMetres(wall.a.y));
          run.points.emplace_back(toMetres(wall.b.x), toMetres(wall.b.y));
          panel.layers.push_back(Layer{"line", "#6a625a", wall.thicknessMetres, {run}});
        }

        uint32_t tint = 0;

        for (const CellWing &wing : compound.wings)
        {
          if (!wing.planned || wing.house.floors.empty())
          {
            continue;
          }

          const CellFloor &floor = wing.house.floors.front();

          for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
          {
            const bool spine = !floor.circulation.empty() && floor.roomAreas[r] == floor.circulation;
            panel.layers.push_back(cellsLayer(wing.house.grid, wing.house.frame, floor.roomAreas[r], "fill",
                                              spine ? "#e8c890" : tints[(tint + r) % 8], 0));
          }

          for (const Chain &loop : floor.loops)
          {
            Ring ring;

            if (ringOf(wing.house.grid, wing.house.frame, loop, ring, nullptr))
            {
              panel.layers.push_back(Layer{"line", "#3c3e44", .35, {ringShape(ring)}});
            }
          }

          double mx = 0;
          double mz = 0;
          edgePoint(wing.house.grid, wing.house.frame, wing.house.entrance, .5, mx, mz);
          Shape mark;
          mark.points.emplace_back(mx, mz);
          panel.layers.push_back(Layer{"point", "#a03020", 4, {mark}});

          tint += 3;
        }

        Shape gate;
        gate.points.emplace_back(toMetres(compound.gate.x), toMetres(compound.gate.y));
        panel.layers.push_back(Layer{"point", "#3878a8", 6, {gate}});

        panels.push_back(std::move(panel));
      }
    }
  }

  // --- phase 10: the solids -------------------------------------------------
  //
  // The 3D track starts here. Written beside the blueprint document so a fault
  // in the massing can be traced back to the plan that caused it.
  {
    namespace hier = procedural::world::hierarchy;

    hier::CellDwellings generator;
    hier::RegionTree tree;
    hier::Context context;
    context.worldSeed = 20260918;

    std::ostringstream solids;
    solids.setf(std::ios::fixed);
    solids.precision(3);
    solids << "{\n  \"buildings\": [\n";

    bool first = true;
    uint32_t emitted = 0;
    uint32_t skeletonTiles = 0;
    bool skeletonIsTheStructure = true;

    const auto faces = [&](const char *kind, const std::vector<hier::Solid::Face> &group, bool &any) {
      if (group.empty())
      {
        return;
      }

      solids << (any ? ",\n" : "") << "      \"" << kind << "\": [";
      any = true;

      for (size_t f = 0; f < group.size(); ++f)
      {
        solids << (f == 0 ? "" : ", ") << "[";

        for (size_t v = 0; v < group[f].size(); ++v)
        {
          solids << (v == 0 ? "" : ",") << "[" << group[f][v].x << "," << group[f][v].y << ","
                 << group[f][v].z << "]";
        }

        solids << "]";
      }

      solids << "]";
    };

    for (const auto &[label, lot] : corpus())
    {
      for (uint32_t k = 0; k < 4 && emitted < 24; ++k)
      {
        hier::Region region = hier::makeRegion(hier::RegionId(k + 1), hier::kNoRegion, lot);
        std::vector<hier::Object> objects;
        Ledger ledger;

        if (!generator.populate(hier::Request{region, tree, context}, objects, ledger, nullptr) ||
            objects.empty() || objects.front().solid == nullptr)
        {
          continue;
        }

        const hier::Solid &solid = *objects.front().solid;
        ++emitted;
        solids << (first ? "" : ",\n") << "    {\n      \"name\": \"" << label << " " << (k + 1) << "\",\n";
        first = false;

        bool any = false;
        faces("walls", solid.walls, any);
        faces("innerwalls", solid.innerWalls, any);
        faces("slopes", solid.slopes, any);
        faces("gables", solid.gables, any);
        faces("soffits", solid.soffits, any);
        faces("pillars", solid.pillars, any);
        faces("joinery", solid.joinery, any);
        faces("reveals", solid.reveals, any);
        faces("innerreveals", solid.innerReveals, any);
        faces("decking", solid.decking, any);
        faces("stairs", solid.stairs, any);
        faces("timbers", solid.timbers, any);
        faces("furniture", solid.furniture, any);
        faces("dormerwalls", solid.dormerWalls, any);
        faces("dormerwindows", solid.dormerWindows, any);
        faces("dormerroofs", solid.dormerRoofs, any);
        faces("dormerceilings", solid.dormerCeilings, any);
        faces("yard", solid.yard, any);
        faces("foundations", solid.foundations, any);
        solids << "\n    }";

        // P26: the skeleton beside the mesh. Every structure face above
        // ground as its own polygon, keyed `s_<kind>` with a segment tint for
        // the walls, no cuts and no returns - what the mesh was built from.
        if (objects.front().structure != nullptr)
        {
          const hier::Structure &structure = *objects.front().structure;
          std::map<std::string, std::vector<hier::Solid::Face>> skeleton;
          std::map<std::string, uint32_t> perKind;

          for (const hier::StructureFace &face : structure.faces)
          {
            double top = -1e300;

            for (const rewrite::Position &q : face.outline)
            {
              top = std::max(top, q.y);
            }

            if (top <= 1e-6 && face.kind != hier::FaceKind::Floor && face.kind != hier::FaceKind::Paving)
            {
              continue;
            }

            std::string key = "s_";

            for (const char *ch = hier::name(face.kind); *ch != '\0'; ++ch)
            {
              key += *ch == ' ' ? '_' : *ch;
            }

            ++perKind[key];

            const bool wallish = face.kind == hier::FaceKind::Wall || face.kind == hier::FaceKind::WindowedWall ||
                                 face.kind == hier::FaceKind::DoorWall || face.kind == hier::FaceKind::DoubleDoorWall ||
                                 face.kind == hier::FaceKind::ArchWall || face.kind == hier::FaceKind::Parapet ||
                                 face.kind == hier::FaceKind::Fence || face.kind == hier::FaceKind::Rail;

            if (wallish && face.segment != UINT32_MAX)
            {
              key += "_" + std::to_string(face.segment % 4);
            }

            hier::Solid::Face polygon;

            for (const rewrite::Position &q : face.outline)
            {
              polygon.push_back(hier::Solid::Vertex{q.x, q.y, q.z});
            }

            skeleton[key].push_back(std::move(polygon));
          }

          solids << ",\n    {\n      \"name\": \"" << label << " " << (k + 1) << " skeleton\",\n";
          bool anySkeleton = false;
          uint32_t drawn = 0;

          for (const auto &[key, group] : skeleton)
          {
            faces(key.c_str(), group, anySkeleton);
            drawn += uint32_t(group.size());
          }

          solids << "\n    }";

          // The gate: what the tile draws IS the structure - one polygon per
          // face above ground, none invented, none dropped.
          uint32_t counted = 0;

          for (const auto &[key, n] : perKind)
          {
            counted += n;
          }

          skeletonTiles += 1;
          skeletonIsTheStructure = skeletonIsTheStructure && drawn == counted && drawn > 0;
        }
      }
    }

    // P27's small settlement vocabulary on the same 3D sheet. Forced here,
    // as in the invariant corpus, so the visual gate always contains the hut,
    // its fenced pen and its one-cell shed rather than depending on a chance.
    {
      const Polygon lot = rectangleLot(18, 14);
      uint32_t shown = 0;

      for (uint64_t seed = 1; seed <= 40 && shown < 6; ++seed)
      {
        const ClassProfile &profile = profileFor(BuildingClass::Hut);
        Stream classStream(seed * 7919u + 13u);
        CellDwellingSettings smallSettings = classed(settings, BuildingClass::Hut, classStream);
        smallSettings.pens = 1;
        smallSettings.sheds = 1;

        CellDwellingBrief brief;
        brief.id = 1000 + seed;
        brief.seed = seed;
        brief.lot = lot;
        brief.frontage = {0};
        brief.setbackMetres = .5;
        brief.building = BuildingClass::Hut;
        brief.standing = profile.standing;

        Ledger ledger;
        CellHouse house;
        rewrite::DwellingPlan plan;
        if (!planCellDwelling(brief, smallSettings, house, ledger, nullptr) ||
            !realiseCellDwelling(house, smallSettings, plan, nullptr) || house.pens.empty() || house.sheds.empty())
          continue;

        hier::Solid solid;
        hier::buildCellDwellingSolid(plan, solid);
        solids << (first ? "" : ",\n") << "    {\n      \"name\": \"hut + pen + shed " << seed << "\",\n";
        first = false;
        bool any = false;
        faces("walls", solid.walls, any);
        faces("innerwalls", solid.innerWalls, any);
        faces("slopes", solid.slopes, any);
        faces("gables", solid.gables, any);
        faces("soffits", solid.soffits, any);
        faces("pillars", solid.pillars, any);
        faces("joinery", solid.joinery, any);
        faces("reveals", solid.reveals, any);
        faces("decking", solid.decking, any);
        faces("stairs", solid.stairs, any);
        faces("timbers", solid.timbers, any);
        faces("furniture", solid.furniture, any);
        faces("dormerwalls", solid.dormerWalls, any);
        faces("dormerwindows", solid.dormerWindows, any);
        faces("dormerroofs", solid.dormerRoofs, any);
        faces("dormerceilings", solid.dormerCeilings, any);
        faces("yard", solid.yard, any);
        faces("foundations", solid.foundations, any);
        solids << "\n    }";
        ++shown;
      }

      check(shown == 6, "small_dwellings_render: the solids sheet carries huts, pens and sheds");
    }

    // A STAIR TURRET, on the same sheet, forced.
    //
    // There was no turret house in the solids gallery at all - the hex tower
    // entries below are structural cutaways of a belfry, not a dwelling - so
    // everything the turret does wrong was invisible here: the host wall
    // standing inside the drum, the flights crossing it, the headroom at a
    // landing. A turret is a quarter of tall houses in the city; it belongs in
    // the picture that is used to judge them.
    {
      const Polygon lot = rectangleLot(20, 16);
      uint32_t shown = 0;

      for (uint64_t seed = 1; seed <= 60 && shown < 2; ++seed)
      {
        CellDwellingBrief brief;
        brief.id = 2000 + seed;
        brief.seed = seed;
        brief.lot = lot;
        brief.frontage = {0};
        brief.setbackMetres = 1.0;
        brief.tags = {"tall", "spiral"};

        Ledger ledger;
        CellHouse house;
        rewrite::DwellingPlan plan;

        if (!planCellDwelling(brief, settings, house, ledger, nullptr) || !house.spiral.has_value() ||
            house.floors.size() < 3 || !realiseCellDwelling(house, settings, plan, nullptr))
        {
          continue;
        }

        hier::Solid solid;
        hier::buildCellDwellingSolid(plan, solid);
        solids << (first ? "" : ",\n") << "    {\n      \"name\": \"stair turret " << seed << "\",\n";
        first = false;
        bool any = false;
        faces("walls", solid.walls, any);
        faces("innerwalls", solid.innerWalls, any);
        faces("slopes", solid.slopes, any);
        faces("gables", solid.gables, any);
        faces("soffits", solid.soffits, any);
        faces("pillars", solid.pillars, any);
        faces("joinery", solid.joinery, any);
        faces("reveals", solid.reveals, any);
        faces("innerreveals", solid.innerReveals, any);
        faces("decking", solid.decking, any);
        faces("stairs", solid.stairs, any);
        faces("timbers", solid.timbers, any);
        faces("furniture", solid.furniture, any);
        faces("yard", solid.yard, any);
        faces("foundations", solid.foundations, any);
        solids << "\n    }";
        ++shown;
      }

      check(shown == 2, "turret_render: the solids sheet carries a stair turret");
    }

    // P32: three context structures, produced by the planners themselves.
    // They share the ordinary solid renderer so the picture cannot silently
    // use a kinder interpretation of the geometry than the mesh does.
    {
      CellBlockBrief brief;
      brief.id = 320; brief.seed = 19; brief.cellMetres = 2.; brief.block = rectangleLot(30, 20);
      CellBlockLevel high;
      high.region.outer = ringOfMetres({{14,0},{30,0},{30,20},{14,20}}); high.elevation = 1.;
      brief.levels.push_back(high);
      CellBlockLotBrief z;
      z.id = 1; z.polygon.outer = ringOfMetres({{2,2},{12,2},{12,10},{2,10}}); z.storeys = 3; z.apartmentStair = ApartmentStairStyle::ZigZag;
      CellBlockLotBrief zWide;
      zWide.id = 2; zWide.polygon.outer = ringOfMetres({{18,8},{28,8},{28,16},{18,16}}); zWide.storeys = 3; zWide.apartmentStair = ApartmentStairStyle::ZigZag;
      brief.lots = {z, zWide};
      Ledger ledger;
      CellBlockPlan block;
      planCellBlock(brief, block, ledger, nullptr);
      hier::Solid terrain;
      for (const CellBlockCell &cell : block.cells)
      {
        hier::Solid::Face q;
        for (const rewrite::Position &p : cell.polygon) q.push_back({p.x,p.y,p.z});
        (cell.kind == BlockCellKind::Paving ? terrain.yard : terrain.decking).push_back(std::move(q));
      }
      for (const CellBlockStair &stair : block.stairs)
      {
        hier::Solid::Face q;
        for (const rewrite::Position &p : stair.flight.opening) q.push_back({p.x,p.y,p.z});
        terrain.stairs.push_back(std::move(q));
      }
      solids << ",\n    {\n      \"name\": \"whole block + terraces\",\n";
      bool any = false; faces("yard", terrain.yard, any); faces("decking", terrain.decking, any); faces("stairs", terrain.stairs, any);
      solids << "\n    }";

      for (const CellBlockBuilding &building : block.buildings)
      {
        std::vector<hier::Solid::Face> apartmentWalls, balconies, stairs, rails, doors;
        std::vector<access::Face> tris;
        access::triangulate(building.exteriorAccess, tris, nullptr);
        for (const access::Face &t : tris)
        {
          hier::Solid::Face f{{t.a.x,t.a.y,t.a.z},{t.b.x,t.b.y,t.b.z},{t.c.x,t.c.y,t.c.z}};
          if (t.kind == access::FaceKind::RampFace) stairs.push_back(std::move(f));
          else if (t.kind == access::FaceKind::SolidFace) apartmentWalls.push_back(std::move(f));
          else if (t.kind == access::FaceKind::BarrierFace) rails.push_back(std::move(f));
          else if (t.kind == access::FaceKind::PortalFace) doors.push_back(std::move(f));
          else balconies.push_back(std::move(f));
        }
        solids << ",\n    {\n      \"name\": \"apartment exterior Z stair on one wall + guarded balconies + openings\",\n";
        any = false; faces("apartmentwalls", apartmentWalls, any); faces("balconies", balconies, any);
        faces("exteriorstairs", stairs, any); faces("balconyrails", rails, any); faces("apartmentdoors", doors, any);
        solids << "\n    }";
      }

      FortressJunctionBrief fb;
      fb.centre = {0,0,0}; fb.towerSides = 6; fb.towerRadius = 4.; fb.height = 9.; fb.floors = 3;
      fb.walls = {{{12.69,0,5.92},2.0},{{-1.22,0,13.95},2.0},{{-11.47,0,-8.03},2.0}};
      FortressJunctionBlueprint blueprint;
      planFortressJunction(fb, blueprint, nullptr);
      const FortressJunctionStructure fs = buildFortressJunctionStructure(blueprint);
      std::vector<hier::Solid::Face> tower, wallArms, wallJoiners, deck, belfry, roof, spiral, turns, turnClosures, openingRails;
      for (const FortressPiece &piece : fs.pieces)
      for (const uint32_t index : piece.faces)
      {
        hier::Solid::Face face;
        for (const rewrite::Position &p : fs.faces[index]) face.push_back({p.x,p.y,p.z});
        auto *bucket = &wallArms;
        if (piece.kind == FortressPieceKind::Tower) bucket = &tower;
        else if (piece.kind == FortressPieceKind::StairTurn || piece.kind == FortressPieceKind::StairFlight || piece.kind == FortressPieceKind::Guard) continue;
        else if (piece.kind == FortressPieceKind::Joiner) bucket = &wallJoiners;
        else if (piece.kind == FortressPieceKind::TopDeck) bucket = &deck;
        else if (piece.kind == FortressPieceKind::BelfryPier) bucket = &belfry;
        else if (piece.kind == FortressPieceKind::Roof) bucket = &roof;
        bucket->push_back(std::move(face));
      }
      for (const access::Ramp &r : blueprint.towerAccess.ramps)
        spiral.push_back({{r.footLeft.x,r.footLeft.y,r.footLeft.z},{r.footRight.x,r.footRight.y,r.footRight.z},
                          {r.headRight.x,r.headRight.y,r.headRight.z},{r.headLeft.x,r.headLeft.y,r.headLeft.z}});
      for (const FortressStairTurn &turn : blueprint.stairTurns)
        if (turn.wallClosure.size() == 3)
          turnClosures.push_back({{turn.wallClosure[0].x,turn.wallClosure[0].y,turn.wallClosure[0].z},
                                  {turn.wallClosure[1].x,turn.wallClosure[1].y,turn.wallClosure[1].z},
                                  {turn.wallClosure[2].x,turn.wallClosure[2].y,turn.wallClosure[2].z}});
      std::vector<access::Face> accessFaces; access::triangulate(blueprint.towerAccess, accessFaces, nullptr);
      for (const access::Face &t : accessFaces)
      {
        if (t.kind == access::FaceKind::BarrierFace)
        {
          const auto barrier = std::find_if(blueprint.towerAccess.barriers.begin(), blueprint.towerAccess.barriers.end(),
                                            [&](const access::Barrier &b){ return b.id == t.of; });
          if (barrier != blueprint.towerAccess.barriers.end() && barrier->note == "top stairwell guard")
            openingRails.push_back({{t.a.x,t.a.y,t.a.z},{t.b.x,t.b.y,t.b.z},{t.c.x,t.c.y,t.c.z}});
          continue;
        }
        if (t.kind != access::FaceKind::SurfaceFace) continue;
        const auto found = std::find_if(blueprint.towerAccess.surfaces.begin(), blueprint.towerAccess.surfaces.end(),
                                        [&](const access::Surface &s){ return s.id == t.of; });
        if (found == blueprint.towerAccess.surfaces.end() || found->kind != access::SurfaceKind::Landing ||
            found->height >= fb.base + fb.height - 1e-6) continue;
        turns.push_back({{t.a.x,t.a.y,t.a.z},{t.b.x,t.b.y,t.b.z},{t.c.x,t.c.y,t.c.z}});
      }
      solids << ",\n    {\n      \"name\": \"roofed open hex belfry + oblique wall joiners + polygon stair\",\n";
      any = false; faces("fortresstower", tower, any); faces("fortresswalls", wallArms, any);
      faces("fortressjoiners", wallJoiners, any); faces("fortressdeck", deck, any);
      faces("fortressbelfry", belfry, any); faces("fortressroof", roof, any);
      faces("fortressstairs", spiral, any); faces("fortressturns", turns, any); faces("fortressturnclosures", turnClosures, any); faces("fortressopeningrail", openingRails, any);
      solids << "\n    }";
      solids << ",\n    {\n      \"name\": \"hex tower polygon stair cutaway + angular landings + roof removed\",\n";
      any = false; faces("fortressdeck", deck, any); faces("fortressbelfry", belfry, any);
      faces("fortressstairs", spiral, any); faces("fortressturns", turns, any); faces("fortressturnclosures", turnClosures, any); faces("fortressopeningrail", openingRails, any);
      solids << "\n    }";
      solids << ",\n    {\n      \"name\": \"hex tower ordinary walls + two-triangle stair turn plan\",\n";
      any = false; faces("fortresstower", tower, any);
      if (!spiral.empty()) faces("fortressstairs", {spiral.front()}, any);
      if (!turns.empty()) faces("fortressturns", {turns.front()}, any);
      if (!turnClosures.empty()) faces("fortressturnclosures", {turnClosures.front()}, any);
      solids << "\n    }";
    }

    solids << "\n  ]\n}\n";

    std::string solidPath = path;
    const size_t dot = solidPath.rfind(".json");

    if (dot != std::string::npos)
    {
      solidPath = solidPath.substr(0, dot) + "-solids.json";
    }

    std::ofstream file(solidPath);
    file << solids.str();
    std::cout << "  wrote " << emitted << " solids (with " << skeletonTiles << " skeleton tiles) to " << solidPath << "\n";
    check(skeletonTiles == emitted && skeletonIsTheStructure, "skeleton_is_the_structure: every building has a skeleton tile drawn from its structure's faces, one polygon each");
  }

  // --- phase 15: elevations --------------------------------------------------
  //
  // Four per house, N E S W, from the built solid and nothing else: the faces
  // projected onto the page and painted back to front. A drawing, not a model
  // - a storey that does not line up is visible by looking along the row.
  {
    namespace hier = procedural::world::hierarchy;

    hier::CellDwellings generator;
    hier::RegionTree tree;
    hier::Context context;
    context.worldSeed = 20260919;

    const std::map<std::string, std::string> colours = {
      {"walls", "#b9b2a4"}, {"reveals", "#8f8879"}, {"joinery", "#6f7d8a"}, {"pillars", "#a08e70"},
      {"slopes", "#9c4a36"}, {"gables", "#8b3f2e"}, {"soffits", "#7a3a2c"}, {"yard", "#d8d0b6"}};

    uint32_t drawn = 0;

    for (const auto &[label, lot] : corpus())
    {
      for (uint32_t k = 0; k < 2 && drawn < 12; ++k)
      {
        hier::Region region = hier::makeRegion(hier::RegionId(k + 1), hier::kNoRegion, lot);
        std::vector<hier::Object> objects;
        Ledger ledger;

        if (!generator.populate(hier::Request{region, tree, context}, objects, ledger, nullptr) ||
            objects.empty() || objects.front().solid == nullptr)
        {
          continue;
        }

        ++drawn;

        for (const hier::ElevationSide side : {hier::ElevationSide::North, hier::ElevationSide::East,
                                               hier::ElevationSide::South, hier::ElevationSide::West})
        {
          const hier::Elevation view = hier::elevationOf(*objects.front().solid, side);

          Panel panel;
          panel.name = label + " " + std::to_string(k + 1) + " - " + hier::name(side);
          panel.note = std::to_string(view.faces.size()) + " faces, " +
                       std::to_string(int(std::lround(view.right - view.left))) + " m across, " +
                       std::to_string(int(std::lround(view.top))) + " m high";

          for (const hier::ElevationFace &face : view.faces)
          {
            const auto colour = colours.find(face.kind);
            Shape shape;
            shape.points = face.outline;
            panel.layers.push_back(Layer{"fill", colour == colours.end() ? "#cccccc" : colour->second, 0, {shape}});
          }

          panels.push_back(std::move(panel));
        }
      }
    }

    std::cout << "  drew " << drawn * 4 << " elevations\n";
  }

  write(panels, path);

  std::cout << "  wrote " << panels.size() << " panels to " << path << "\n";

  check(framed >= 10, "gallery: the framing panels were produced");
  check(built >= 40, "gallery: the footprint panels were produced");
  check(!panels.empty(), "gallery: the document is not empty");

  if (failures != 0)
  {
    std::cerr << failures << " check(s) failed\n";

    return 1;
  }

  std::cout << "all checks passed\n";

  return 0;
}
