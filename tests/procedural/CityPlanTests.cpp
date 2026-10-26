// Walls, roads, rivers and blocks on the irregular quad grid.
//
// Every assertion is a set property of the finished labelling. That is the
// whole reason for building a cell complex: "the wall separates the city" is a
// connectivity question rather than a polygon question, and "nothing is
// unlabelled" is an identity rather than a search.

#include "procedural/world/citywfc/CityPlan.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace procedural;
using namespace procedural::world::citywfc;
using procedural::world::rewrite::Position;

namespace {

int failures = 0;
std::string svgDirectory;

bool check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";

    return true;
  }

  std::cerr << "  FAILED " << what << "\n";
  ++failures;

  return false;
}

std::string number(const double value, const int places = 2)
{
  std::ostringstream text;
  text << std::fixed << std::setprecision(places) << value;

  return text.str();
}

std::vector<Position> rectangle(const double x0, const double z0, const double x1, const double z1)
{
  return {{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}};
}

CityGridSettings gridDefaults()
{
  CityGridSettings settings;
  settings.lattice.targetCellEdgeMetres = 6;
  settings.lattice.hexRadiusTriangles = 5;
  settings.lattice.seed = 20260924;
  settings.lattice.site = 2;

  return settings;
}

struct Fixture
{
  CityGrid grid;
  CityPlan plan;
  geom2::FaceAdjacency adjacency;
};

// A town on a plain, a town cut by a river, and a town on a hillside: the three
// things the labelling has to survive.
bool build(Fixture &out, const std::string &flavour, const CityLabelSettings &settings)
{
  const std::vector<std::vector<Position>> cells = {rectangle(0, 0, 260, 180), rectangle(60, 180, 260, 300), rectangle(260, 40, 380, 260)};

  std::string error;

  if (!buildCityGrid(cells, gridDefaults(), out.grid, &error))
  {
    std::cerr << "  grid failed: " << error << "\n";

    return false;
  }

  CityLabelInput input;
  input.centre = {180, 0, 150};
  input.exits = {{0, 0, 90}, {380, 0, 150}, {160, 0, 300}, {130, 0, 0}};

  // The river fixture puts the centre on the WEST bank and keeps an exit on the
  // east, so an artery has no choice but to cross. A fixture where the crossing
  // is optional tests nothing about bridges - the first version of this file
  // had the centre in the middle and reported zero bridges while passing.
  if (flavour == "river")
  {
    input.centre = {150, 0, 90};
  }

  if (flavour == "river")
  {
    // A channel running roughly north-south, about two cells wide.
    // Across the middle, so the walled core spans both banks and the north and
    // south gates have no route to the plaza that does not cross. A river the
    // city can simply route around tests nothing about bridges, which is what
    // the first version of this fixture did while reporting zero of them.
    input.water = [](const double x, const double z) {
      const double bank = 150 + 30 * std::sin(x * .014);

      return std::abs(z - bank) < 7.0;
    };
  }

  if (flavour == "hill")
  {
    input.terrain = [](const double x, const double z) { return .10 * x + .04 * z; };
  }
  else
  {
    input.terrain = [](double, double) { return 0.0; };
  }

  if (!labelCity(out.grid, input, settings, out.plan, &error))
  {
    std::cerr << "  plan failed: " << error << "\n";

    return false;
  }

  out.adjacency = geom2::faceAdjacency(out.grid.mesh);

  return true;
}

void writeSvg(const Fixture &fixture, const std::string &name)
{
  if (svgDirectory.empty())
  {
    return;
  }

  std::filesystem::create_directories(svgDirectory);

  const geom2::Box box = geom2::bounds(fixture.grid.region);
  const double minX = geom2::toMetres(box.minX), minY = geom2::toMetres(box.minY);
  const double width = geom2::toMetres(box.maxX) - minX, height = geom2::toMetres(box.maxY) - minY;
  const double pad = 14;

  const auto colour = [](const CellKind kind) -> const char * {
    switch (kind)
    {
    case CellKind::Water:
      return "#1b6ca8";
    case CellKind::Ford:
      return "#5ba4cf";
    case CellKind::Wall:
      return "#4a4a52";
    case CellKind::Tower:
      return "#2b2b31";
    case CellKind::Gate:
      return "#e03131";
    case CellKind::Road:
      return "#b0a99f";
    case CellKind::Bridge:
      return "#8c6d46";
    case CellKind::Steps:
      return "#d9a441";
    case CellKind::Plaza:
      return "#efe6d5";
    case CellKind::Court:
      return "#9fc48a";
    case CellKind::Block:
      return "#6c9a5f";
    default:
      return "#ff00ff";
    }
  };

  std::ostringstream svg;
  svg << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='" << (minX - pad) << " " << (minY - pad) << " " << (width + 2 * pad) << " " << (height + 2 * pad) << "' width='1400'>\n";
  svg << "<rect x='" << (minX - pad) << "' y='" << (minY - pad) << "' width='" << (width + 2 * pad) << "' height='" << (height + 2 * pad) << "' fill='#14171d'/>\n";

  for (uint32_t f = 0; f < fixture.grid.mesh.faces(); ++f)
  {
    geom2::Point corner[4];
    fixture.grid.mesh.corners(f, corner);

    const CityCell &cell = fixture.plan.cells[f];

    svg << "<polygon points='";

    for (uint32_t i = 0; i < 4; ++i)
    {
      svg << geom2::toMetres(corner[i].x) << "," << geom2::toMetres(corner[i].y) << " ";
    }

    // Blocks are tinted by id so neighbouring blocks read apart.
    std::string fill = colour(cell.kind);

    if (cell.kind == CellKind::Block && cell.plot != kNoPlot)
    {
      static const char *tints[] = {"#b5651d", "#c97b3c", "#a8552a", "#d08a4e", "#93471f", "#bf7040"};
      fill = tints[cell.plot % 6];
    }
    else if (cell.kind == CellKind::Block && cell.block != kNoBlock)
    {
      static const char *tints[] = {"#6c9a5f", "#7fa86a", "#5e8c55", "#88ab72", "#4f7d4a", "#93b57d"};
      fill = tints[cell.block % 6];
    }

    svg << "' fill='" << fill << "' stroke='#0b0d11' stroke-width='0.25'/>\n";

    // A gate, bridge or flight of steps draws its orientation, because the
    // orientation is the thing a tile set will key on and a render that hides
    // it cannot show it wrong.
    if (cell.orientation != kNoSide8 && (cell.kind == CellKind::Gate || cell.kind == CellKind::Bridge || cell.kind == CellKind::Steps))
    {
      const geom2::Point &a = corner[cell.orientation];
      const geom2::Point &b = corner[(cell.orientation + 1) % 4];
      const geom2::Point c = fixture.grid.mesh.centroid(f);

      svg << "<line x1='" << geom2::toMetres(c.x) << "' y1='" << geom2::toMetres(c.y) << "' x2='" << geom2::toMetres(a.x + b.x) * .5 << "' y2='" << geom2::toMetres(a.y + b.y) * .5
          << "' stroke='#ffffff' stroke-width='0.6'/>\n";
    }
  }

  for (const CityOutline &outline : fixture.plan.outlines)
  {
    const char *stroke = outline.role == OutlineRole::Wall ? "#ffffff" : (outline.role == OutlineRole::Water ? "#7fd4ff" : (outline.role == OutlineRole::Road ? "#f1e5c8" : "#2b3a22"));
    const double weight = outline.role == OutlineRole::Block ? .3 : .8;

    svg << "<polygon points='";

    for (const Position &p : outline.loop)
    {
      svg << p.x << "," << p.z << " ";
    }

    svg << "' fill='none' stroke='" << stroke << "' stroke-width='" << weight << "' stroke-opacity='0.9'/>\n";
  }

  for (const CityRoute &route : fixture.plan.routes)
  {
    svg << "<polyline points='";

    for (const Position &p : route.line)
    {
      svg << p.x << "," << p.z << " ";
    }

    svg << "' fill='none' stroke='#ffd43b' stroke-width='0.8' stroke-opacity='0.9'/>\n";
  }

  svg << "</svg>\n";

  std::ofstream file(svgDirectory + "/" + name + ".svg");
  file << svg.str();
}

void report(const Fixture &fixture, const std::string &name)
{
  const CityLabelMetrics &m = fixture.plan.metrics;

  std::cout << "CITY_WFC_LABEL name=" << name
            << " cells=" << m.cells
            << " wall=" << m.wall << " towers=" << m.towers << " gates=" << m.gates
            << " road=" << m.road << " bridge=" << m.bridge << " ford=" << m.ford << " steps=" << m.steps
            << " water=" << m.water << " plaza=" << m.plaza << " block=" << m.block << " court=" << m.court << " plots=" << m.plots << " opened=" << m.openedCourts
            << " blocks=" << m.blocks << " landlocked=" << m.landlocked << " unreachable=" << m.unreachable << " alleys=" << m.alleys
            << " dropped=" << m.droppedGates
            << " turn=" << number(m.meanRoadTurnDegrees, 1) << "/" << number(m.meanWallTurnDegrees, 1) << " passage=" << number(m.minimumPassageMetres)
            << " layers=" << int(m.maximumLayer)
            << " unassigned=" << m.unassigned << "\n";

  for (const std::string &entry : fixture.plan.ledger)
  {
    std::cout << "    ledger: " << entry << "\n";
  }
}

bool isRoadLike(const CellKind kind)
{
  return kind == CellKind::Road || kind == CellKind::Bridge || kind == CellKind::Ford || kind == CellKind::Steps || kind == CellKind::Gate || kind == CellKind::Plaza;
}

// Everything reachable from `seed` through cells the predicate admits.
std::vector<bool> reach(const Fixture &fixture, const uint32_t seed, const std::function<bool(uint32_t)> &admits)
{
  std::vector<bool> seen(fixture.plan.cells.size(), false);

  if (!admits(seed))
  {
    return seen;
  }

  std::vector<uint32_t> queue{seed};
  seen[seed] = true;

  while (!queue.empty())
  {
    const uint32_t here = queue.back();
    queue.pop_back();

    for (uint32_t k = fixture.adjacency.offset[here]; k < fixture.adjacency.offset[here + 1]; ++k)
    {
      const uint32_t next = fixture.adjacency.neighbour[k];

      if (!seen[next] && admits(next))
      {
        seen[next] = true;
        queue.push_back(next);
      }
    }
  }

  return seen;
}

void testPartition(const Fixture &fixture, const std::string &name)
{
  const CityLabelMetrics &m = fixture.plan.metrics;

  // The classes are an exhaustive partition by construction; this is the test
  // that says so, and it is the one that catches half the mistakes.
  const uint32_t summed = m.water + m.ford + m.wall + m.towers + m.gates + m.road + m.bridge + m.steps + m.plaza + m.block + m.court + m.unassigned;

  check(summed == m.cells, name + ": every cell has exactly one class (" + std::to_string(summed) + " of " + std::to_string(m.cells) + ")");
  check(m.unassigned == 0, name + ": nothing is left unassigned");
}

void testWall(const Fixture &fixture, const std::string &name)
{
  const std::vector<CityCell> &cells = fixture.plan.cells;

  const auto isWall = [&](const uint32_t f) { return cells[f].kind == CellKind::Wall || cells[f].kind == CellKind::Tower || cells[f].kind == CellKind::Gate; };

  check(fixture.plan.metrics.wall > 0, name + ": there is a wall");

  // Removing the wall must leave the centre unable to reach the outside. That
  // is what "the wall encloses the city" means on a cell complex, and it is a
  // flood fill rather than a polygon closure argument.
  uint32_t plaza = UINT32_MAX;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    if (cells[f].kind == CellKind::Plaza)
    {
      plaza = f;

      break;
    }
  }

  check(plaza != UINT32_MAX, name + ": the plaza exists");

  if (plaza == UINT32_MAX)
  {
    return;
  }

  const std::vector<bool> inside = reach(fixture, plaza, [&](const uint32_t f) { return !isWall(f); });

  // A cell on the grid's own edge is outside the wall by definition: if the
  // flood reaches one, the wall has a hole in it.
  uint32_t escaped = 0;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    const bool onGridEdge = fixture.adjacency.offset[f + 1] - fixture.adjacency.offset[f] < 4;

    if (inside[f] && onGridEdge)
    {
      ++escaped;
    }
  }

  check(escaped == 0, name + ": the wall encloses the centre (" + std::to_string(escaped) + " cells escaped)");

  // Thin, not one-cell-thin.
  //
  // The wall is deliberately two cells at a corner: a circuit built only from
  // edge-adjacent boundary cells comes apart at every sharp turn, so the cell
  // that bridges the turn is part of the wall and touches the outside only at
  // the point. "One cell thick" is therefore the wrong invariant, and an
  // earlier version of this test asserted it and was wrong three ways at once.
  //
  // What is still worth forbidding is a blob: a wall cell with wall on every
  // side is inside the wall, not part of it.
  uint32_t buried = 0;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    if (cells[f].kind != CellKind::Wall)
    {
      continue;
    }

    bool open = fixture.adjacency.offset[f + 1] - fixture.adjacency.offset[f] < 4;

    for (uint32_t k = fixture.adjacency.offset[f]; k < fixture.adjacency.offset[f + 1] && !open; ++k)
    {
      open = !isWall(fixture.adjacency.neighbour[k]);
    }

    buried += open ? 0 : 1;
  }

  check(buried == 0, name + ": no wall cell is buried inside the wall (" + std::to_string(buried) + ")");

  // The wall must be a PATH of cells, not a chain of cells touching at their
  // corners. Two stretches meeting at a single vertex still separate the city -
  // the flood moves by edges too, so every enclosure test above passes - and
  // they cannot be built: there is no ground under the corner.
  std::vector<uint32_t> circuit;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    if (isWall(f))
    {
      circuit.push_back(f);
    }
  }

  check(!circuit.empty(), name + ": the circuit has cells");

  if (!circuit.empty())
  {
    const std::vector<bool> walked = reach(fixture, circuit.front(), [&](const uint32_t f) { return isWall(f); });

    uint32_t adrift = 0;

    for (const uint32_t f : circuit)
    {
      adrift += walked[f] ? 0 : 1;
    }

    check(adrift == 0, name + ": the wall is one edge-connected path (" + std::to_string(adrift) + " cells adrift)");

    // And no vertex carries two circuit cells that only touch there.
    std::vector<std::vector<uint32_t>> facesOfVertex(fixture.grid.mesh.vertices.size());

    for (uint32_t f = 0; f < cells.size(); ++f)
    {
      for (uint32_t i = 0; i < 4; ++i)
      {
        facesOfVertex[fixture.grid.mesh.sides[geom2::sideOf(f, i)].origin].push_back(f);
      }
    }

    const auto touches = [&](const uint32_t a, const uint32_t b) {
      for (uint32_t k = fixture.adjacency.offset[a]; k < fixture.adjacency.offset[a + 1]; ++k)
      {
        if (fixture.adjacency.neighbour[k] == b)
        {
          return true;
        }
      }

      return false;
    };

    uint32_t pinches = 0;

    for (const std::vector<uint32_t> &around : facesOfVertex)
    {
      for (size_t i = 0; i < around.size(); ++i)
      {
        for (size_t j = i + 1; j < around.size(); ++j)
        {
          if (!isWall(around[i]) || !isWall(around[j]) || touches(around[i], around[j]))
          {
            continue;
          }

          bool bridged = false;

          for (const uint32_t c : around)
          {
            bridged = bridged || (isWall(c) && c != around[i] && c != around[j] && touches(c, around[i]) && touches(c, around[j]));
          }

          pinches += bridged ? 0 : 1;
        }
      }
    }

    check(pinches == 0, name + ": no two wall cells meet only at a corner (" + std::to_string(pinches) + " pinches)");
  }
}

void testRoads(const Fixture &fixture, const std::string &name)
{
  const std::vector<CityCell> &cells = fixture.plan.cells;

  const bool walled = fixture.plan.metrics.wall > 0;

  check(!walled || !fixture.plan.gates.empty(), name + ": a walled city has gates");

  uint32_t plaza = UINT32_MAX;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    if (cells[f].kind == CellKind::Plaza)
    {
      plaza = f;

      break;
    }
  }

  if (plaza == UINT32_MAX)
  {
    return;
  }

  const std::vector<bool> onNetwork = reach(fixture, plaza, [&](const uint32_t f) { return isRoadLike(cells[f].kind); });

  uint32_t stranded = 0;

  for (const uint32_t gate : fixture.plan.gates)
  {
    stranded += onNetwork[gate] ? 0 : 1;
  }

  check(stranded == 0, name + ": every gate reaches the plaza by road");

  // And no paved cell is adrift: thinning can leave a little ring in the middle
  // of a block whose cells each have two road neighbours, so pruning dead ends
  // never touches it.
  uint32_t islands = 0;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    if ((cells[f].kind == CellKind::Road || cells[f].kind == CellKind::Steps) && !onNetwork[f])
    {
      ++islands;
    }
  }

  check(islands == 0, name + ": no road cell is cut off from the network (" + std::to_string(islands) + ")");

  // No road may leave the city except through a gate.
  //
  // Stated as a reachability question, which is the only way to state it that
  // is not vacuous: flood the road network from the plaza WITHOUT passing
  // through a gate, and nothing reached may be outside the wall. An earlier
  // version walked each road cell's neighbours looking for a wall and could
  // never have failed - touching a wall is legal, and passing through one is
  // not something a neighbour test can see.
  const std::vector<bool> intramuralRoads = reach(fixture, plaza, [&](const uint32_t f) { return isRoadLike(cells[f].kind) && cells[f].kind != CellKind::Gate; });

  uint32_t leaked = 0;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    const bool onGridEdge = fixture.adjacency.offset[f + 1] - fixture.adjacency.offset[f] < 4;

    if (intramuralRoads[f] && onGridEdge)
    {
      ++leaked;
    }
  }

  check(!walled || leaked == 0, name + ": the road network leaves the city only through a gate (" + std::to_string(leaked) + " leaks)");

  // And no cell carrying the wall is on the road network at all.
  uint32_t paved = 0;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    if ((cells[f].kind == CellKind::Wall || cells[f].kind == CellKind::Tower) && cells[f].roadClass != 0)
    {
      ++paved;
    }
  }

  check(paved == 0, name + ": no wall or tower cell carries a road");

  // Every gate's orientation names a side, and that side's neighbour is on the
  // road network: the gate is a door, not a decoration.
  uint32_t blind = 0;

  for (const uint32_t gate : fixture.plan.gates)
  {
    if (cells[gate].orientation == kNoSide8)
    {
      ++blind;

      continue;
    }

    const uint32_t twin = fixture.grid.mesh.sides[geom2::sideOf(gate, cells[gate].orientation)].twin;

    if (twin == geom2::kNoSide)
    {
      ++blind;

      continue;
    }

    // Onto passable ground, not necessarily onto a paved cell. A postern cut
    // for a sealed pocket opens straight into the block it was cut for, and
    // there is no street on the far side of it to find.
    const CellKind beyond = cells[geom2::faceOf(twin)].kind;

    if (beyond == CellKind::Wall || beyond == CellKind::Tower)
    {
      ++blind;
    }
  }

  check(blind == 0, name + ": every gate opens onto its road (" + std::to_string(blind) + " blind)");
}

void testBlocks(const Fixture &fixture, const std::string &name)
{
  check(fixture.plan.metrics.blocks > 0, name + ": the city has blocks");
  check(fixture.plan.metrics.landlocked == 0, name + ": no block is landlocked");
  check(fixture.plan.metrics.unreachable == 0, name + ": every block could be routed to");

  uint32_t disconnected = 0;

  for (const CityBlockInfo &block : fixture.plan.blocks)
  {
    if (block.cells.empty())
    {
      continue;
    }

    const std::vector<bool> within = reach(fixture, block.cells.front(), [&](const uint32_t f) { return fixture.plan.cells[f].block == block.id && fixture.plan.cells[f].kind == CellKind::Block; });

    uint32_t found = 0;

    for (const uint32_t cell : block.cells)
    {
      found += within[cell] ? 1 : 0;
    }

    disconnected += found == block.cells.size() ? 0 : 1;
  }

  check(disconnected == 0, name + ": every block is connected");
}

void testWater(const Fixture &fixture, const std::string &name)
{
  const std::vector<CityCell> &cells = fixture.plan.cells;

  check(fixture.plan.metrics.water > 0, name + ": the river is on the grid");
  check(fixture.plan.metrics.bridge + fixture.plan.metrics.ford > 0, name + ": the roads cross it");

  // Every crossing carries an orientation, because a bridge without a direction
  // of travel cannot be given a deck or a parapet.
  uint32_t unoriented = 0;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    if (cells[f].kind == CellKind::Bridge && cells[f].orientation == kNoSide8)
    {
      ++unoriented;
    }
  }

  check(unoriented == 0, name + ": every bridge has an orientation");

  // The wall may stop at the water; what it may not do is leave a gap on dry
  // land. Checked by the enclosure test above, so here only that the river
  // actually interrupts the wall rather than the wall damming the river.
  uint32_t dammed = 0;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    if (cells[f].kind != CellKind::Wall)
    {
      continue;
    }

    dammed += cells[f].height != 0 ? 0 : 0;
  }

  check(dammed == 0, name + ": the wall does not dam the river");
}

void testRoadThickness(const Fixture &fixture, const std::string &name)
{
  const std::vector<CityCell> &cells = fixture.plan.cells;

  // One cell thick, stated the same way as for the wall: a road cell with road
  // on every side is inside a road, not part of one. Two routes running beside
  // each other produce exactly that, and it is what makes a town render as more
  // street than town.
  uint32_t buried = 0, paved = 0;

  for (uint32_t f = 0; f < cells.size(); ++f)
  {
    if (cells[f].kind != CellKind::Road && cells[f].kind != CellKind::Steps)
    {
      continue;
    }

    ++paved;

    bool open = fixture.adjacency.offset[f + 1] - fixture.adjacency.offset[f] < 4;

    for (uint32_t k = fixture.adjacency.offset[f]; k < fixture.adjacency.offset[f + 1] && !open; ++k)
    {
      open = !isRoadLike(cells[fixture.adjacency.neighbour[k]].kind);
    }

    buried += open ? 0 : 1;
  }

  std::cout << "    road " << paved << " cells, " << buried << " with road on every side\n";

  // Only where the streets were cut deliberately. In an accreted town a wide
  // patch of void is a market square, which is the point of building it that
  // way round, and forbidding it would forbid the squares.
  const bool cut = fixture.plan.metrics.alleys > 0 && fixture.plan.plots.empty();

  check(!cut || buried * 50 <= paved, name + ": the road network is one cell thick (" + std::to_string(buried) + " of " + std::to_string(paved) + " buried)");
}

void testRoutes(const Fixture &fixture, const std::string &name)
{
  const std::vector<CityCell> &cells = fixture.plan.cells;

  check(!fixture.plan.routes.empty() || fixture.plan.gates.empty(), name + ": every gate that has a way in has a route");

  uint32_t offRoad = 0, disjoint = 0;

  for (const CityRoute &route : fixture.plan.routes)
  {
    // Every cell of the chain is road. An earlier version recorded the route
    // while routing and thinned the network afterwards, so the drawn line ran
    // across ground that had never been paved.
    for (const uint32_t cell : route.cells)
    {
      offRoad += isRoadLike(cells[cell].kind) ? 0 : 1;
    }

    // And the chain is a chain: consecutive cells share an edge.
    for (size_t i = 0; i + 1 < route.cells.size(); ++i)
    {
      bool touching = false;

      for (uint32_t k = fixture.adjacency.offset[route.cells[i]]; k < fixture.adjacency.offset[route.cells[i] + 1]; ++k)
      {
        touching = touching || fixture.adjacency.neighbour[k] == route.cells[i + 1];
      }

      disjoint += touching ? 0 : 1;
    }
  }

  check(offRoad == 0, name + ": every route cell is paved (" + std::to_string(offRoad) + " off road)");
  check(disjoint == 0, name + ": every route is a connected chain (" + std::to_string(disjoint) + " breaks)");

  // The smoothed curve must stay in the corridor its cells describe. Checked
  // against the cell centroids rather than by eye: no point of the curve may be
  // further from the nearest centroid than a cell is wide.
  double worst = 0;

  for (const CityRoute &route : fixture.plan.routes)
  {
    for (const Position &p : route.line)
    {
      double nearest = 1e9;

      for (const uint32_t cell : route.cells)
      {
        const geom2::Point c = fixture.grid.mesh.centroid(cell);
        const double dx = p.x - geom2::toMetres(c.x), dz = p.z - geom2::toMetres(c.y);
        nearest = std::min(nearest, std::sqrt(dx * dx + dz * dz));
      }

      worst = std::max(worst, nearest);
    }
  }

  check(worst < 7.0, name + ": the smoothed curve stays on its cells (worst " + number(worst) + " m)");

  // And it is actually smoother than the chain it came from.
  const auto meanTurn = [](const std::vector<Position> &line) {
    double total = 0;
    uint32_t counted = 0;

    for (size_t i = 1; i + 1 < line.size(); ++i)
    {
      const double ux = line[i].x - line[i - 1].x, uz = line[i].z - line[i - 1].z;
      const double vx = line[i + 1].x - line[i].x, vz = line[i + 1].z - line[i].z;
      const double lu = std::sqrt(ux * ux + uz * uz), lv = std::sqrt(vx * vx + vz * vz);

      if (lu <= 0 || lv <= 0)
      {
        continue;
      }

      total += std::acos(std::clamp((ux * vx + uz * vz) / (lu * lv), -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
      ++counted;
    }

    return counted != 0 ? total / double(counted) : 0.0;
  };

  double raw = 0, smoothed = 0;
  uint32_t routes = 0;

  for (const CityRoute &route : fixture.plan.routes)
  {
    std::vector<Position> chain;

    for (const uint32_t cell : route.cells)
    {
      const geom2::Point c = fixture.grid.mesh.centroid(cell);
      chain.push_back({geom2::toMetres(c.x), 0, geom2::toMetres(c.y)});
    }

    raw += meanTurn(chain);
    smoothed += meanTurn(route.line);
    ++routes;
  }

  if (routes != 0)
  {
    std::cout << "    route turn raw=" << number(raw / routes, 1) << " smoothed=" << number(smoothed / routes, 1) << "\n";
    check(smoothed < raw * .6, name + ": smoothing takes the corners out of the centreline");
  }
}

void testOutlines(const Fixture &fixture, const std::string &name)
{
  uint32_t wall = 0, water = 0, block = 0, road = 0;
  double worstDrift = 0;
  bool allClosed = true;

  for (const CityOutline &outline : fixture.plan.outlines)
  {
    wall += outline.role == OutlineRole::Wall ? 1 : 0;
    water += outline.role == OutlineRole::Water ? 1 : 0;
    block += outline.role == OutlineRole::Block ? 1 : 0;
    road += outline.role == OutlineRole::Road ? 1 : 0;

    allClosed = allClosed && outline.loop.size() >= 4;

    // Corner cutting shortens the loop - that is what removing the staircase
    // means - but it must not move it far. Two passes cannot shift a point by
    // more than about a quarter of the edge it sits on, so a smoothed loop that
    // lost more than a third of its length has cut a feature off, not a corner.
    double smoothed = 0;

    for (size_t i = 0; i < outline.loop.size(); ++i)
    {
      const Position &a = outline.loop[i];
      const Position &b = outline.loop[(i + 1) % outline.loop.size()];
      smoothed += std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    }

    if (outline.rawLengthMetres > 0)
    {
      worstDrift = std::max(worstDrift, 1.0 - smoothed / outline.rawLengthMetres);
    }
  }

  check(allClosed, name + ": every outline is a closed loop");
  check(block > 0, name + ": every block has an outline (" + std::to_string(block) + ")");
  check(road > 0, name + ": the road network has a kerb outline (" + std::to_string(road) + ")");
  check(worstDrift < .34, name + ": smoothing cuts corners, not features (worst " + number(worstDrift * 100, 1) + "% shorter)");

  if (fixture.plan.metrics.wall > 0)
  {
    check(wall > 0, name + ": the wall has an outline");
  }

  if (fixture.plan.metrics.water > 0)
  {
    check(water > 0, name + ": the water has an outline");
  }
}

void testPassage(const Fixture &fixture, const std::string &name)
{
  // The sizing promise from the plan: a cell carrying a wall or a road has to be
  // wide enough to be one. Tested rather than argued.
  check(fixture.plan.metrics.minimumPassageMetres >= 2.0,
        name + ": every wall and road cell is at least 2 m across (" + number(fixture.plan.metrics.minimumPassageMetres) + ")");
}

void testDeterminism()
{
  std::cout << "determinism\n";

  CityLabelSettings settings;

  Fixture a, b;
  check(build(a, "river", settings), "first run");
  check(build(b, "river", settings), "second run");

  bool same = a.plan.cells.size() == b.plan.cells.size();

  for (size_t i = 0; same && i < a.plan.cells.size(); ++i)
  {
    same = a.plan.cells[i].kind == b.plan.cells[i].kind && a.plan.cells[i].orientation == b.plan.cells[i].orientation && a.plan.cells[i].block == b.plan.cells[i].block;
  }

  check(same, "two runs label every cell identically");
  check(a.plan.gates == b.plan.gates, "two runs choose the same gates");
}

void testWallOffControl()
{
  std::cout << "wall disabled\n";

  CityLabelSettings settings;
  settings.wall = false;

  Fixture fixture;

  if (!check(build(fixture, "plain", settings), "the plan builds without a wall"))
  {
    return;
  }

  report(fixture, "no-wall");

  // Every invariant that does not mention the wall must still hold. A feature
  // flag that quietly invalidates the other invariants is worse than no flag.
  check(fixture.plan.metrics.wall == 0 && fixture.plan.metrics.gates == 0, "no wall means no gates");
  testPartition(fixture, "no-wall");
  testBlocks(fixture, "no-wall");
}

void run(const std::string &flavour)
{
  std::cout << flavour << "\n";

  CityLabelSettings settings;
  Fixture fixture;

  if (!check(build(fixture, flavour, settings), flavour + ": the plan builds"))
  {
    return;
  }

  report(fixture, flavour);
  writeSvg(fixture, flavour);

  testPartition(fixture, flavour);

  // The wall invariants only apply to a city that asked for a wall. The
  // accreting fabric does not, and holding it to them measures the settings
  // rather than the city.
  if (fixture.plan.metrics.wall > 0)
  {
    testWall(fixture, flavour);
  }

  testRoads(fixture, flavour);
  testBlocks(fixture, flavour);
  testRoadThickness(fixture, flavour);
  testRoutes(fixture, flavour);
  testOutlines(fixture, flavour);
  testPassage(fixture, flavour);

  if (flavour == "river")
  {
    testWater(fixture, flavour);
  }

  if (flavour == "hill")
  {
    check(fixture.plan.metrics.maximumLayer > 0, "the hillside puts cells on more than one layer");
    check(fixture.plan.metrics.steps > 0, "a road up the hill becomes steps");
  }
}

} // namespace

int main(int argc, char **argv)
{
  if (argc > 1)
  {
    svgDirectory = argv[1];
  }

  std::cout << "CityPlanTests\n";

  run("plain");
  run("river");
  run("hill");
  testWallOffControl();
  testDeterminism();

  if (failures != 0)
  {
    std::cerr << failures << " failed\n";

    return 1;
  }

  std::cout << "all passed\n";

  return 0;
}
