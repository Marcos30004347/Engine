#include "procedural/world/citywfc/CityPlan.hpp"

#include "procedural/world/citywfc/CityRouter.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

namespace procedural::world::citywfc {
namespace {

using geom2::FaceAdjacency;
using geom2::kNoFace;
using geom2::kNoSide;
using geom2::QuadMesh;
using geom2::sideOf;
using geom2::toMetres;

struct Flat
{
  double x = 0, y = 0;
};

Flat centreOf(const QuadMesh &mesh, const uint32_t face)
{
  const geom2::Point p = mesh.centroid(face);

  return {toMetres(p.x), toMetres(p.y)};
}

double distance(const Flat &a, const Flat &b)
{
  const double dx = b.x - a.x, dy = b.y - a.y;

  return std::sqrt(dx * dx + dy * dy);
}

uint32_t nearestFace(const QuadMesh &mesh, const Flat &to, const std::vector<bool> *within = nullptr)
{
  uint32_t best = kNoFace;
  double bestDistance = 0;

  for (uint32_t f = 0; f < mesh.faces(); ++f)
  {
    if (within && !(*within)[f])
    {
      continue;
    }

    const double d = distance(centreOf(mesh, f), to);

    if (best == kNoFace || d < bestDistance)
    {
      best = f;
      bestDistance = d;
    }
  }

  return best;
}

// Which side of `face` leads to `other`, or kNoSide8 when they do not touch.
uint8_t sideTowards(const QuadMesh &mesh, const uint32_t face, const uint32_t other)
{
  for (uint32_t i = 0; i < 4; ++i)
  {
    const uint32_t twin = mesh.sides[sideOf(face, i)].twin;

    if (twin != kNoSide && geom2::faceOf(twin) == other)
    {
      return uint8_t(i);
    }
  }

  return kNoSide8;
}

// The largest circle that fits across a cell, approximated by its shortest
// distance between opposite edge midpoints. What "is this wide enough to be a
// gate" means, and cheaper than an inscribed-circle solve.
double passageWidth(const QuadMesh &mesh, const uint32_t face)
{
  geom2::Point corner[4];
  mesh.corners(face, corner);

  Flat mid[4];

  for (uint32_t i = 0; i < 4; ++i)
  {
    mid[i] = {toMetres(corner[i].x + corner[(i + 1) % 4].x) * .5, toMetres(corner[i].y + corner[(i + 1) % 4].y) * .5};
  }

  return std::min(distance(mid[0], mid[2]), distance(mid[1], mid[3]));
}

// The faces around a vertex, in cyclic order, starting from `side`.
//
// Rotating by twin(prev(s)) walks the fan. `closed` says whether it came back
// to where it started; an open fan means the vertex is on the mesh boundary,
// and the gap counts as outside for every question asked of it here.
std::vector<uint32_t> facesAroundVertex(const QuadMesh &mesh, const uint32_t side, bool &closed)
{
  std::vector<uint32_t> fan;
  closed = false;

  uint32_t walk = side;

  for (uint32_t guard = 0; guard < 16; ++guard)
  {
    fan.push_back(geom2::faceOf(walk));

    const uint32_t back = mesh.sides[geom2::prevSide(walk)].twin;

    if (back == kNoSide)
    {
      return fan;
    }

    if (back == side)
    {
      closed = true;

      return fan;
    }

    walk = back;
  }

  return fan;
}

// A majority filter over the cell adjacency graph.
//
// The boundary of a rasterised set is a staircase, and everything built along it
// inherits every step: a wall that zigzags, a river bank that saws in and out.
// Smoothing the SET is what fixes that, and it has to happen before the boundary
// is extracted - smoothing the boundary afterwards moves the geometry away from
// the cells it is supposed to describe, and the two then disagree about which
// side of the wall a cell is on.
//
// A cell joins the set when most of its neighbours are in it and leaves when
// most are not. Cells the caller pins are never touched.
void smoothCellSet(const FaceAdjacency &adjacency, std::vector<bool> &set, const uint32_t passes, const std::vector<bool> *pinned = nullptr)
{
  const uint32_t count = uint32_t(set.size());

  for (uint32_t pass = 0; pass < passes; ++pass)
  {
    std::vector<bool> next = set;

    for (uint32_t f = 0; f < count; ++f)
    {
      if (pinned && (*pinned)[f])
      {
        continue;
      }

      const uint32_t neighbours = adjacency.offset[f + 1] - adjacency.offset[f];

      if (neighbours < 3)
      {
        continue; // On the rim, where there is nothing to take a majority of.
      }

      uint32_t inside = 0;

      for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
      {
        inside += set[adjacency.neighbour[k]] ? 1 : 0;
      }

      // Strictly more than half joins, strictly less than half leaves, and an
      // even split is left alone - so a straight boundary does not creep.
      if (inside * 2 > neighbours)
      {
        next[f] = true;
      }
      else if (inside * 2 < neighbours)
      {
        next[f] = false;
      }
    }

    if (next == set)
    {
      return;
    }

    set.swap(next);
  }
}

uint64_t splitmix(uint64_t value)
{
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;

  return value ^ (value >> 31);
}

// Purpose-addressed: the seed, what is being decided, and the stable identity of
// the thing it is decided for. Never the traversal order.
uint64_t purposeHash(const uint64_t seed, const uint64_t purpose, const uint64_t subject)
{
  return splitmix(splitmix(splitmix(seed ^ 0x2545f4914f6cdd1dull) ^ purpose) ^ subject);
}

// Cells reachable from a seed through `passable`, by breadth first search.
std::vector<uint32_t> componentOf(const FaceAdjacency &adjacency, const std::vector<bool> &passable, const uint32_t seed, std::vector<bool> &seen)
{
  std::vector<uint32_t> found;

  if (!passable[seed] || seen[seed])
  {
    return found;
  }

  std::vector<uint32_t> queue{seed};
  seen[seed] = true;

  while (!queue.empty())
  {
    const uint32_t here = queue.back();
    queue.pop_back();
    found.push_back(here);

    for (uint32_t k = adjacency.offset[here]; k < adjacency.offset[here + 1]; ++k)
    {
      const uint32_t next = adjacency.neighbour[k];

      if (passable[next] && !seen[next])
      {
        seen[next] = true;
        queue.push_back(next);
      }
    }
  }

  std::sort(found.begin(), found.end());

  return found;
}

} // namespace

const char *cellKindName(const CellKind kind)
{
  switch (kind)
  {
  case CellKind::Unassigned:
    return "unassigned";
  case CellKind::Water:
    return "water";
  case CellKind::Ford:
    return "ford";
  case CellKind::Wall:
    return "wall";
  case CellKind::Tower:
    return "tower";
  case CellKind::Gate:
    return "gate";
  case CellKind::Road:
    return "road";
  case CellKind::Bridge:
    return "bridge";
  case CellKind::Steps:
    return "steps";
  case CellKind::Plaza:
    return "plaza";
  case CellKind::Block:
    return "block";
  default:
    return "?";
  }
}

bool labelCity(const CityGrid &grid, const CityLabelInput &input, const CityLabelSettings &settings, CityPlan &out, std::string *error)
{
  out = {};

  const auto fail = [&](const std::string &why) {
    if (error)
    {
      *error = why;
    }

    out = {};

    return false;
  };

  const QuadMesh &mesh = grid.mesh;
  const uint32_t count = mesh.faces();

  if (count == 0)
  {
    return fail("the grid has no cells");
  }

  const FaceAdjacency adjacency = geom2::faceAdjacency(mesh);

  out.cells.assign(count, {});
  out.metrics.cells = count;

  std::vector<Flat> centre(count);

  for (uint32_t f = 0; f < count; ++f)
  {
    centre[f] = centreOf(mesh, f);
  }

  // ---- 1. Water ----------------------------------------------------------
  //
  // Before everything, because a river decides where a wall may end and where a
  // road has to pay for a bridge.
  if (input.water)
  {
    std::vector<bool> wet(count, false);

    for (uint32_t f = 0; f < count; ++f)
    {
      wet[f] = input.water(centre[f].x, centre[f].y);
    }

    // A channel rasterised cell by cell has a sawtooth bank. Smoothing the set
    // straightens it before anything is built along it.
    smoothCellSet(adjacency, wet, settings.waterSmoothingPasses);

    for (uint32_t f = 0; f < count; ++f)
    {
      if (wet[f])
      {
        out.cells[f].kind = CellKind::Water;
        ++out.metrics.water;
      }
    }
  }

  // ---- 2. Ground -------------------------------------------------------
  //
  // One height per cell and the vertical slot it puts the cell in. The column
  // model is only the ground layer for now; nothing stacks yet.
  if (input.terrain)
  {
    double lowest = 0;
    bool first = true;

    for (uint32_t f = 0; f < count; ++f)
    {
      out.cells[f].height = float(input.terrain(centre[f].x, centre[f].y));
      lowest = first ? out.cells[f].height : std::min(lowest, double(out.cells[f].height));
      first = false;
    }

    const double step = std::max(.5, settings.layerHeightMetres);

    for (uint32_t f = 0; f < count; ++f)
    {
      const double above = double(out.cells[f].height) - lowest;
      out.cells[f].layerGround = uint8_t(std::clamp(above / step, 0.0, 254.0));
      out.metrics.maximumLayer = std::max(out.metrics.maximumLayer, out.cells[f].layerGround);
    }
  }

  const uint32_t plazaCell = nearestFace(mesh, {input.centre.x, input.centre.z});

  if (plazaCell == kNoFace)
  {
    return fail("the city centre does not fall on the grid");
  }

  // ---- 3. The walled core, and the wall as its boundary layer -------------
  std::vector<bool> intramural(count, false);

  if (settings.wall && settings.walledAreaFraction > 0)
  {
    // Grown outward from the centre by travel cost rather than by radius, so
    // the core follows the ground and stops at the water.
    struct Reach
    {
      double cost = 0;
      uint32_t cell = 0;

      bool operator>(const Reach &other) const
      {
        return cost != other.cost ? cost > other.cost : cell > other.cell;
      }
    };

    double total = 0;

    for (uint32_t f = 0; f < count; ++f)
    {
      total += mesh.areaMetres(f);
    }

    const double wanted = total * std::clamp(settings.walledAreaFraction, 0.0, 1.0);

    std::priority_queue<Reach, std::vector<Reach>, std::greater<Reach>> frontier;
    std::vector<bool> seen(count, false);

    frontier.push({0, plazaCell});
    seen[plazaCell] = true;

    double taken = 0;

    while (!frontier.empty() && taken < wanted)
    {
      const Reach here = frontier.top();
      frontier.pop();

      // Water does not stop the core. A river through a walled city is normal -
      // it is most of the reference material - and a core that stops at the
      // bank produces a wall that stops at the bank, which leaves the city open
      // along its whole length. The wall crosses the water instead, which is a
      // river wall, and the crossing of it is a water gate.
      intramural[here.cell] = true;
      taken += mesh.areaMetres(here.cell);

      for (uint32_t k = adjacency.offset[here.cell]; k < adjacency.offset[here.cell + 1]; ++k)
      {
        const uint32_t next = adjacency.neighbour[k];

        if (seen[next])
        {
          continue;
        }

        seen[next] = true;
        frontier.push({here.cost + distance(centre[here.cell], centre[next]), next});
      }
    }

    // Regularise: fill a cell surrounded by the core, drop one barely attached,
    // and remove one-cell necks. A neck is fatal to "the wall is one cell
    // thick", because the boundary layer of a neck is the neck itself seen from
    // both sides.
    for (uint32_t round = 0; round < 3; ++round)
    {
      std::vector<bool> next = intramural;

      for (uint32_t f = 0; f < count; ++f)
      {
        uint32_t inside = 0, neighbours = adjacency.offset[f + 1] - adjacency.offset[f];

        for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
        {
          inside += intramural[adjacency.neighbour[k]] ? 1 : 0;
        }

        if (!intramural[f] && neighbours > 0 && inside >= neighbours - 0u && inside >= 3)
        {
          next[f] = true; // A hole in the core.
        }

        if (intramural[f] && inside <= 1)
        {
          next[f] = false; // A spur, or a neck one cell wide.
        }
      }

      if (next == intramural)
      {
        break;
      }

      intramural = next;
    }

    // Smooth the core before its boundary becomes a wall. A core grown by
    // travel cost has a ragged edge, and the wall is that edge.
    smoothCellSet(adjacency, intramural, settings.coreSmoothingPasses);

    // Remove vertex pinches from the core: a point where the region touches
    // itself across a corner without sharing an edge.
    //
    // This is where the diagonal wall comes from, and fixing it here rather
    // than in the wall is the difference between one rule and a repair pass
    // that fights itself. A region with a corner pinch has a boundary layer
    // with a corner pinch; a region without one does not.
    for (uint32_t round = 0; round < 4; ++round)
    {
      std::vector<uint32_t> sideOfVertex(mesh.vertices.size(), kNoSide);

      for (uint32_t f = 0; f < count; ++f)
      {
        for (uint32_t i = 0; i < 4; ++i)
        {
          const uint32_t side = sideOf(f, i);
          sideOfVertex[mesh.sides[side].origin] = side;
        }
      }

      uint32_t cut = 0;

      for (uint32_t v = 0; v < uint32_t(mesh.vertices.size()); ++v)
      {
        if (sideOfVertex[v] == kNoSide)
        {
          continue;
        }

        bool closed = false;
        const std::vector<uint32_t> fan = facesAroundVertex(mesh, sideOfVertex[v], closed);

        if (fan.size() < 3)
        {
          continue;
        }

        // Count the maximal runs of core faces around the vertex. One run is a
        // corner; two are a pinch, and the region touches itself at a point.
        const uint32_t steps = uint32_t(fan.size());

        const auto core = [&](const uint32_t at) { return intramural[fan[at % steps]]; };

        uint32_t runs = 0;

        for (uint32_t k = 0; k < steps; ++k)
        {
          const bool previous = closed ? core(k + steps - 1) : (k > 0 && core(k - 1));

          if (core(k) && !previous)
          {
            ++runs;
          }
        }

        if (runs < 2)
        {
          continue;
        }

        // Only a single cell is ever removed. A wider pinch is a genuine
        // isthmus in the core, which the neck rule above is for; clearing whole
        // runs here shreds the region, which is what the first attempt did.
        uint32_t lone = kNoFace;

        for (uint32_t k = 0; k < steps; ++k)
        {
          const bool previous = closed ? core(k + steps - 1) : (k > 0 && core(k - 1));
          const bool following = closed ? core(k + 1) : (k + 1 < steps && core(k + 1));

          if (core(k) && !previous && !following)
          {
            lone = fan[k];

            break;
          }
        }

        if (lone == kNoFace)
        {
          continue;
        }

        intramural[lone] = false;
        ++cut;
      }

      if (cut == 0)
      {
        break;
      }
    }

    // Keep only the component the plaza is in; the regularisation can shed an
    // island, and a wall around two pieces is two walls.
    {
      std::vector<bool> seenCore(count, false);
      const std::vector<uint32_t> core = componentOf(adjacency, intramural, plazaCell, seenCore);
      std::vector<bool> kept(count, false);

      for (const uint32_t f : core)
      {
        kept[f] = true;
      }

      const uint32_t shed = uint32_t(std::count(intramural.begin(), intramural.end(), true)) - uint32_t(core.size());

      if (shed != 0)
      {
        out.ledger.push_back("dropped " + std::to_string(shed) + " core cell(s) not connected to the centre");
      }

      intramural = kept;
    }

    // The wall is every core cell touching a boundary VERTEX, plus a repair for
    // whatever that still leaves disjoint.
    //
    // Two consecutive boundary edges of the core meet at a vertex, and the
    // cells inside them are joined only through the fan of core cells at that
    // vertex. A fan cell touching the boundary only AT the vertex is invisible
    // to an edge-adjacency test, so a wall built from edge adjacency breaks
    // there: measured, 59 of 124 wall cells had no wall neighbour they shared
    // an edge with, and the render showed a dotted line. Asking for the vertex
    // takes the whole fan and the ring closes.
    //
    // Three things that do not work, all measured. Promoting only the one-cell
    // diagonal gap leaves the wall in pieces (8 adrift on a plain, 148 across a
    // river). Walking the fan for any gap goes the long way round through the
    // cells one row inside and thickens the wall everywhere. Repairing pinches
    // iteratively cascades - each promotion makes new corners, and four passes
    // walled the city solid.
    //
    // Water does not enter this. The river is *in* the city and the wall
    // crosses it only where the core ends; treating a bank as a boundary walls
    // both sides of every channel and doubles the circuit.
    std::vector<bool> vertexFacesOutside(mesh.vertices.size(), false);

    for (uint32_t f = 0; f < count; ++f)
    {
      for (uint32_t i = 0; i < 4; ++i)
      {
        const uint32_t side = sideOf(f, i);

        if (!intramural[f])
        {
          vertexFacesOutside[mesh.sides[side].origin] = true;
        }

        if (mesh.sides[side].twin == kNoSide)
        {
          vertexFacesOutside[mesh.sides[side].origin] = true;
          vertexFacesOutside[mesh.destination(side)] = true;
        }
      }
    }

    for (uint32_t f = 0; f < count; ++f)
    {
      if (!intramural[f])
      {
        continue;
      }

      bool onEdge = false;

      for (uint32_t i = 0; i < 4 && !onEdge; ++i)
      {
        onEdge = vertexFacesOutside[mesh.sides[sideOf(f, i)].origin];
      }

      if (onEdge)
      {
        out.cells[f].kind = CellKind::Wall;
      }
    }

    // One repair pass for corners the fan cannot bridge: two wall cells across
    // a vertex with no core cell between them. Nothing inside can join them, so
    // the wall bulges outward by one cell. Once, not iteratively.
    {
      std::vector<std::vector<uint32_t>> facesOfVertex(mesh.vertices.size());

      for (uint32_t f = 0; f < count; ++f)
      {
        for (uint32_t i = 0; i < 4; ++i)
        {
          facesOfVertex[mesh.sides[sideOf(f, i)].origin].push_back(f);
        }
      }

      std::vector<uint32_t> add;

      for (const std::vector<uint32_t> &around : facesOfVertex)
      {
        for (size_t i = 0; i < around.size(); ++i)
        {
          for (size_t j = i + 1; j < around.size(); ++j)
          {
            const uint32_t a = around[i], b = around[j];

            if (out.cells[a].kind != CellKind::Wall || out.cells[b].kind != CellKind::Wall || sideTowards(mesh, a, b) != kNoSide8)
            {
              continue;
            }

            uint32_t bridge = kNoFace;
            bool joined = false;

            for (const uint32_t c : around)
            {
              if (c == a || c == b || sideTowards(mesh, c, a) == kNoSide8 || sideTowards(mesh, c, b) == kNoSide8)
              {
                continue;
              }

              joined = joined || out.cells[c].kind == CellKind::Wall;
              bridge = bridge == kNoFace ? c : bridge;
            }

            if (!joined && bridge != kNoFace)
            {
              add.push_back(bridge);
            }
          }
        }
      }

      for (const uint32_t f : add)
      {
        out.cells[f].kind = CellKind::Wall;
      }
    }

    for (uint32_t f = 0; f < count; ++f)
    {
      out.metrics.wall += out.cells[f].kind == CellKind::Wall ? 1 : 0;
    }
  }

  // ---- 4. Gates -----------------------------------------------------------
  //
  // Chosen with the roads rather than before them: a gate is a wall cell an
  // artery passes through, so the candidates are the wall cells nearest the
  // world's own road exits.
  std::vector<uint32_t> wallCells;

  for (uint32_t f = 0; f < count; ++f)
  {
    if (out.cells[f].kind == CellKind::Wall)
    {
      wallCells.push_back(f);
    }
  }

  std::vector<uint32_t> gateCandidates;

  if (!wallCells.empty())
  {
    std::vector<Flat> wanted;

    for (const Position &exit : input.exits)
    {
      wanted.push_back({exit.x, exit.z});
    }

    if (wanted.empty())
    {
      // No exits supplied: spread them by angle about the centre, which is a
      // fallback rather than a plan and says so on the ledger.
      const Flat middle = centre[plazaCell];

      for (uint32_t g = 0; g < settings.maximumGates; ++g)
      {
        const double turn = 6.283185307179586 * double(g) / double(std::max(1u, settings.maximumGates));
        wanted.push_back({middle.x + std::cos(turn) * 1e5, middle.y + std::sin(turn) * 1e5});
      }

      out.ledger.push_back("no road exits supplied: gates spread by angle");
    }

    for (const Flat &target : wanted)
    {
      uint32_t best = kNoFace;
      double bestDistance = 0;

      for (const uint32_t f : wallCells)
      {
        if (passageWidth(mesh, f) < 2.0)
        {
          continue;
        }

        // A gate has to be a way through, not just a piece of wall near a
        // road: open ground on the inside and open ground on the outside. A
        // wall cell at a corner has wall on both sides and seals the gate the
        // moment it is cut, and the route out of it then fails.
        bool openInside = false, openOutside = false;

        for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
        {
          const uint32_t next = adjacency.neighbour[k];
          const bool walled = out.cells[next].kind == CellKind::Wall || out.cells[next].kind == CellKind::Tower;

          openInside = openInside || (intramural[next] && !walled);
          openOutside = openOutside || !intramural[next];
        }

        if (!openInside || !openOutside)
        {
          continue;
        }

        const double d = distance(centre[f], target);

        if (best == kNoFace || d < bestDistance)
        {
          best = f;
          bestDistance = d;
        }
      }

      if (best == kNoFace)
      {
        continue;
      }

      bool tooClose = false;

      for (const uint32_t already : gateCandidates)
      {
        tooClose = tooClose || distance(centre[already], centre[best]) < settings.gateSpacingMetres;
      }

      if (!tooClose && gateCandidates.size() < settings.maximumGates)
      {
        gateCandidates.push_back(best);
      }
    }
  }

  for (const uint32_t g : gateCandidates)
  {
    out.cells[g].kind = CellKind::Gate;
    --out.metrics.wall;
    ++out.metrics.gates;
  }

  const auto isRoad = [&](const uint32_t f) {
    const CellKind kind = out.cells[f].kind;

    return kind == CellKind::Road || kind == CellKind::Bridge || kind == CellKind::Ford || kind == CellKind::Steps || kind == CellKind::Gate || kind == CellKind::Plaza;
  };

  RouteSettings routing;
  routing.maximumExpansions = settings.maximumRouteExpansions;
  routing.lowestCostFactor = std::min(settings.reuseFactor, 1.0);

  // What a change of direction costs, shared by every router.
  //
  // It was on the arteries only to begin with, and the arteries are a handful
  // of routes out of two hundred: the alleys and the block splits carried no
  // turn penalty at all, so raising the artery weight moved the measured mean
  // turn from 23.8 to 21.7 degrees and no further. A cost term that most of the
  // network does not pay is not a cost term.
  const auto turnPenalty = [&](const uint32_t previous, const uint32_t from, const uint32_t to, const double base) {
    if (previous == kNoFace)
    {
      return 0.0;
    }

    const Flat &a = centre[previous], &b = centre[from], &c = centre[to];
    const double ux = b.x - a.x, uy = b.y - a.y;
    const double vx = c.x - b.x, vy = c.y - b.y;
    const double lu = std::sqrt(ux * ux + uy * uy), lv = std::sqrt(vx * vx + vy * vy);

    if (lu <= 0 || lv <= 0)
    {
      return 0.0;
    }

    const double cosine = std::clamp((ux * vx + uy * vy) / (lu * lv), -1.0, 1.0);

    return settings.turnWeight * (1.0 - cosine) * base;
  };

  // ---- 5a. Accretion: buildings first, streets as what is left -----------
  //
  // The other order. Seeds are scattered at a spacing, each claims the ground
  // around it up to a plot's worth, and the cells nobody claimed are the void.
  // The void is then read rather than designed: the part of it you can walk out
  // of is the street, and a pocket with building all round it is a courtyard.
  //
  // Nothing here decides where a street goes. That is the point - a lane
  // between two houses is not a decision anyone made, it is what the two houses
  // did not take - and it is why this produces courtyards without a rule for
  // them and the subdividing order does not.
  if (settings.fabric == FabricMode::Accrete)
  {
    // The through routes first, so the houses have something to line.
    if (settings.primaryStreetsFirst)
    {
      const StepCost trunk = [&](const uint32_t previous, const uint32_t from, const uint32_t to) {
        if (out.cells[to].kind == CellKind::Wall || out.cells[to].kind == CellKind::Tower)
        {
          return kBlocked;
        }

        const double base = distance(centre[from], centre[to]);
        const double rise = std::abs(double(out.cells[to].height) - double(out.cells[from].height));
        const double grade = base > 0 ? rise / base : 0;

        double step = base + settings.gradeWeight * grade * grade * base + turnPenalty(previous, from, to, base);

        if (isRoad(to))
        {
          step *= settings.reuseFactor;
        }

        if (out.cells[to].kind == CellKind::Water)
        {
          step += settings.bridgeCost;
        }

        return step;
      };

      // Where each of the world's roads meets the edge of the grid.
      std::vector<uint32_t> heads;

      for (const Position &exit : input.exits)
      {
        uint32_t from = kNoFace;
        double best = 0;

        for (uint32_t f = 0; f < count; ++f)
        {
          if (adjacency.offset[f + 1] - adjacency.offset[f] >= 4)
          {
            continue; // Not on the rim.
          }

          const double d = distance(centre[f], {exit.x, exit.z});

          if (from == kNoFace || d < best)
          {
            from = f;
            best = d;
          }
        }

        if (from != kNoFace)
        {
          heads.push_back(from);
        }
      }

      // Every head to the centre, and then head to head. The second set is what
      // makes the trunk network a network: routes that all converge on one
      // point enclose nothing, and a fabric accreted around them has no blocks
      // in it - which is exactly how the first version came out.
      std::vector<std::pair<uint32_t, uint32_t>> runs;

      for (const uint32_t head : heads)
      {
        runs.push_back({head, plazaCell});
      }

      for (size_t i = 0; i < heads.size(); ++i)
      {
        for (size_t j = i + 1; j < heads.size(); ++j)
        {
          runs.push_back({heads[i], heads[j]});
        }
      }

      for (const auto &run : runs)
      {
        std::vector<uint32_t> path;
        RouteReport report;

        if (!routeCells(mesh, adjacency, run.first, run.second, trunk, routing, path, report))
        {
          continue;
        }

        for (size_t step = 0; step < path.size(); ++step)
        {
          const uint32_t cell = path[step];

          if (out.cells[cell].kind == CellKind::Water)
          {
            out.cells[cell].kind = CellKind::Bridge;
            out.cells[cell].orientation = step > 0 ? sideTowards(mesh, cell, path[step - 1]) : kNoSide8;
          }
          else if (out.cells[cell].kind == CellKind::Unassigned)
          {
            out.cells[cell].kind = CellKind::Road;
          }

          out.cells[cell].roadClass = 1;
        }
      }
    }

    std::vector<bool> buildable(count, false);
    double reach = 1;

    for (uint32_t f = 0; f < count; ++f)
    {
      buildable[f] = out.cells[f].kind == CellKind::Unassigned;
      reach = std::max(reach, distance(centre[f], centre[plazaCell]));
    }

    // Seeds, at a spacing measured in cells rather than metres so the pattern
    // follows the grid it is laid on. Order is a hash of the cell's own
    // identity, never the sweep, so permuting the faces cannot change which
    // cells are chosen.
    std::vector<std::pair<uint64_t, uint32_t>> shuffled;

    for (uint32_t f = 0; f < count; ++f)
    {
      if (!buildable[f])
      {
        continue;
      }

      // Density falls off from the centre. A cell that loses the draw is not a
      // street - it is simply not seeded - and the accretion around it decides
      // what it becomes.
      const double away = distance(centre[f], centre[plazaCell]) / reach;
      const double keep = 1.0 - settings.densityFalloff * away * away;

      const uint64_t draw = purposeHash(input.seed, 11, f);

      if (double(draw % 1000u) / 1000.0 > keep)
      {
        continue;
      }

      shuffled.push_back({purposeHash(input.seed, 12, f), f});
    }

    std::sort(shuffled.begin(), shuffled.end());

    std::vector<uint32_t> hops(count, UINT32_MAX);
    std::vector<uint32_t> seeds;

    for (const auto &entry : shuffled)
    {
      if (hops[entry.second] != UINT32_MAX)
      {
        continue;
      }

      seeds.push_back(entry.second);

      // Mark everything within the spacing so no second seed lands there.
      std::vector<uint32_t> queue{entry.second};
      hops[entry.second] = 0;

      for (size_t at = 0; at < queue.size(); ++at)
      {
        const uint32_t here = queue[at];

        if (hops[here] >= settings.plotSpacingCells)
        {
          continue;
        }

        for (uint32_t k = adjacency.offset[here]; k < adjacency.offset[here + 1]; ++k)
        {
          const uint32_t next = adjacency.neighbour[k];

          if (!buildable[next] || hops[next] != UINT32_MAX)
          {
            continue;
          }

          hops[next] = hops[here] + 1;
          queue.push_back(next);
        }
      }
    }

    // Grow every plot at once, one ring at a time, so neighbouring plots meet
    // in the middle instead of the first one taking everything.
    std::vector<uint32_t> owner(count, kNoPlot);
    std::vector<uint32_t> taken(seeds.size(), 1);
    std::vector<uint32_t> want(seeds.size(), 0);

    for (uint32_t p = 0; p < uint32_t(seeds.size()); ++p)
    {
      owner[seeds[p]] = p;

      const uint32_t span = settings.plotCellsMaximum >= settings.plotCellsMinimum ? settings.plotCellsMaximum - settings.plotCellsMinimum + 1 : 1;
      want[p] = settings.plotCellsMinimum + uint32_t(purposeHash(input.seed, 13, seeds[p]) % span);
    }

    std::vector<uint32_t> front = seeds;

    for (uint32_t ring = 0; ring < settings.plotCellsMaximum + 2 && !front.empty(); ++ring)
    {
      std::vector<uint32_t> next;

      for (const uint32_t here : front)
      {
        const uint32_t plot = owner[here];

        if (plot == kNoPlot || taken[plot] >= want[plot])
        {
          continue;
        }

        for (uint32_t k = adjacency.offset[here]; k < adjacency.offset[here + 1]; ++k)
        {
          const uint32_t candidate = adjacency.neighbour[k];

          if (!buildable[candidate] || owner[candidate] != kNoPlot || taken[plot] >= want[plot])
          {
            continue;
          }

          owner[candidate] = plot;
          ++taken[plot];
          next.push_back(candidate);
        }
      }

      front.swap(next);
    }

    // Read the void - but only the void that is IN the town.
    //
    // Everything unbuilt becomes street otherwise, and the countryside is
    // unbuilt, so the first version paved 59 % of the map and called the fields
    // a road network. A street is void with building beside it; open ground
    // with nothing near it is just open ground.
    std::vector<bool> nearBuilt(count, false);

    {
      std::vector<uint32_t> queue;
      std::vector<uint32_t> hopsOut(count, UINT32_MAX);

      for (uint32_t f = 0; f < count; ++f)
      {
        if (owner[f] != kNoPlot)
        {
          hopsOut[f] = 0;
          nearBuilt[f] = true;
          queue.push_back(f);
        }
      }

      for (size_t at = 0; at < queue.size(); ++at)
      {
        const uint32_t here = queue[at];

        if (hopsOut[here] >= settings.streetReachCells)
        {
          continue;
        }

        for (uint32_t k = adjacency.offset[here]; k < adjacency.offset[here + 1]; ++k)
        {
          const uint32_t next = adjacency.neighbour[k];

          if (hopsOut[next] != UINT32_MAX)
          {
            continue;
          }

          hopsOut[next] = hopsOut[here] + 1;
          nearBuilt[next] = true;
          queue.push_back(next);
        }
      }
    }

    std::vector<bool> open(count, false);

    for (uint32_t f = 0; f < count; ++f)
    {
      open[f] = buildable[f] && owner[f] == kNoPlot && nearBuilt[f];
    }

    // The outside is the rim of the grid; the plaza is inside. Both are street
    // by definition, and everything the void connects to them is too.
    std::vector<bool> reached(count, false);
    std::vector<uint32_t> queue;

    const auto enter = [&](const uint32_t f) {
      if (open[f] && !reached[f])
      {
        reached[f] = true;
        queue.push_back(f);
      }
    };

    enter(plazaCell);

    // And from wherever the town meets open country: a lane running out of the
    // last houses is a way out, so anything it connects to is street.
    for (uint32_t f = 0; f < count; ++f)
    {
      if (!open[f])
      {
        continue;
      }

      bool meetsCountry = adjacency.offset[f + 1] - adjacency.offset[f] < 4;

      for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1] && !meetsCountry; ++k)
      {
        meetsCountry = !nearBuilt[adjacency.neighbour[k]];
      }

      if (meetsCountry)
      {
        enter(f);
      }
    }

    for (size_t at = 0; at < queue.size(); ++at)
    {
      for (uint32_t k = adjacency.offset[queue[at]]; k < adjacency.offset[queue[at] + 1]; ++k)
      {
        enter(adjacency.neighbour[k]);
      }
    }

    for (uint32_t f = 0; f < count; ++f)
    {
      if (!open[f])
      {
        continue;
      }

      out.cells[f].kind = reached[f] ? CellKind::Road : CellKind::Court;
      out.cells[f].roadClass = reached[f] ? 2 : 0;
    }

    // A court too large to be a yard is opened: the nearest street cell to it
    // is joined through the building, which is how a wynd gets cut.
    {
      std::vector<bool> isCourt(count, false);

      for (uint32_t f = 0; f < count; ++f)
      {
        isCourt[f] = out.cells[f].kind == CellKind::Court;
      }

      std::vector<bool> seen(count, false);

      for (uint32_t f = 0; f < count; ++f)
      {
        const std::vector<uint32_t> pocket = componentOf(adjacency, isCourt, f, seen);

        if (pocket.empty())
        {
          continue;
        }

        double area = 0;

        for (const uint32_t cell : pocket)
        {
          area += mesh.areaMetres(cell);
        }

        if (area <= settings.maximumCourtAreaMetres)
        {
          continue;
        }

        const StepCost outward = [&](const uint32_t previous, const uint32_t from, const uint32_t to) {
          if (out.cells[to].kind == CellKind::Water)
          {
            return kBlocked;
          }

          (void)previous;

          return distance(centre[from], centre[to]) * (isRoad(to) ? .1 : 1.0);
        };

        std::vector<uint32_t> path;
        RouteReport report;

        if (!routeCells(mesh, adjacency, pocket.front(), plazaCell, outward, routing, path, report))
        {
          continue;
        }

        for (const uint32_t cell : path)
        {
          if (out.cells[cell].kind == CellKind::Court || out.cells[cell].kind == CellKind::Block || out.cells[cell].kind == CellKind::Unassigned)
          {
            out.cells[cell].kind = CellKind::Road;
            out.cells[cell].roadClass = 3;
            owner[cell] = kNoPlot;
          }
        }

        ++out.metrics.openedCourts;
      }
    }

    // A pocket smaller than a yard is a gap, not a courtyard.
    {
      std::vector<bool> isCourt(count, false);

      for (uint32_t f = 0; f < count; ++f)
      {
        isCourt[f] = out.cells[f].kind == CellKind::Court;
      }

      std::vector<bool> seen(count, false);

      for (uint32_t f = 0; f < count; ++f)
      {
        const std::vector<uint32_t> pocket = componentOf(adjacency, isCourt, f, seen);

        if (pocket.empty() || pocket.size() >= settings.minimumCourtCells)
        {
          continue;
        }

        for (const uint32_t cell : pocket)
        {
          // Into the street if it touches one, and into a neighbouring plot if
          // it does not: either way it stops being a hole in the map.
          bool joined = false;

          for (uint32_t k = adjacency.offset[cell]; k < adjacency.offset[cell + 1] && !joined; ++k)
          {
            if (isRoad(adjacency.neighbour[k]))
            {
              out.cells[cell].kind = CellKind::Road;
              out.cells[cell].roadClass = 3;
              joined = true;
            }
          }

          for (uint32_t k = adjacency.offset[cell]; k < adjacency.offset[cell + 1] && !joined; ++k)
          {
            const uint32_t next = adjacency.neighbour[k];

            if (owner[next] != kNoPlot)
            {
              owner[cell] = owner[next];
              out.cells[cell].kind = CellKind::Unassigned;
              joined = true;
            }
          }

          if (!joined)
          {
            out.cells[cell].kind = CellKind::Unassigned;
          }
        }
      }
    }

    // One street network, not several.
    //
    // A pocket of void can open onto the fields without ever reaching the
    // middle of the town - a legitimate farm track, and a disconnected road
    // network all the same. Each stray component is joined to the centre, and
    // the join is allowed to cut through building because that is what a wynd
    // is.
    for (uint32_t attempt = 0; attempt < 64; ++attempt)
    {
      // Everything you can walk along, which includes the bridge this loop just
      // built. Counting only Road and Plaza leaves the new bridge out of the
      // network it was built to join, so the far bank reads as stranded on the
      // next pass and the loop tries to reach it again for ever.
      std::vector<bool> street(count, false);

      for (uint32_t f = 0; f < count; ++f)
      {
        street[f] = isRoad(f);
      }

      std::vector<bool> seen(count, false);
      const std::vector<uint32_t> main = componentOf(adjacency, street, plazaCell, seen);

      for (const uint32_t f : main)
      {
        street[f] = false;
      }

      uint32_t stray = kNoFace;

      for (uint32_t f = 0; f < count && stray == kNoFace; ++f)
      {
        stray = street[f] ? f : kNoFace;
      }

      if (stray == kNoFace)
      {
        break;
      }

      // Water is crossable at the bridge price, not forbidden.
      //
      // Forbidding it strands the far bank: its streets are a component the
      // join cannot reach, so they are quietly turned into courtyards and half
      // the town becomes a walled garden. The failure looked like "no bridges"
      // and was actually "no far bank".
      const StepCost join = [&](const uint32_t previous, const uint32_t from, const uint32_t to) {
        (void)previous;

        const double base = distance(centre[from], centre[to]);

        if (out.cells[to].kind == CellKind::Water)
        {
          return base + settings.bridgeCost;
        }

        return isRoad(to) ? base * .1 : base * 6.0;
      };

      std::vector<uint32_t> path;
      RouteReport report;

      if (!routeCells(mesh, adjacency, stray, plazaCell, join, routing, path, report))
      {
        // Nothing can reach it; it is a yard rather than a street.
        std::vector<bool> reachable(count, false);
        std::vector<bool> ignored(count, false);

        for (uint32_t f = 0; f < count; ++f)
        {
          reachable[f] = isRoad(f) && out.cells[f].kind != CellKind::Plaza;
        }

        for (const uint32_t cell : componentOf(adjacency, reachable, stray, ignored))
        {
          out.cells[cell].kind = CellKind::Court;
          out.cells[cell].roadClass = 0;
        }

        continue;
      }

      for (size_t step = 0; step < path.size(); ++step)
      {
        const uint32_t cell = path[step];

        if (out.cells[cell].kind == CellKind::Water)
        {
          out.cells[cell].kind = CellKind::Bridge;
          out.cells[cell].roadClass = 3;
          out.cells[cell].orientation = step > 0 ? sideTowards(mesh, cell, path[step - 1]) : kNoSide8;

          continue;
        }

        if (out.cells[cell].kind == CellKind::Block || out.cells[cell].kind == CellKind::Court || out.cells[cell].kind == CellKind::Unassigned)
        {
          out.cells[cell].kind = CellKind::Road;
          out.cells[cell].roadClass = 3;
          owner[cell] = kNoPlot;
        }
      }
    }


    // Publish the plots, and mark their cells as built ground.
    out.plots.clear();
    out.plots.resize(seeds.size());

    for (uint32_t p = 0; p < uint32_t(seeds.size()); ++p)
    {
      out.plots[p].id = p;
    }

    for (uint32_t f = 0; f < count; ++f)
    {
      if (owner[f] == kNoPlot || out.cells[f].kind == CellKind::Road || out.cells[f].kind == CellKind::Court)
      {
        continue;
      }

      out.cells[f].kind = CellKind::Block;
      out.cells[f].plot = owner[f];
      out.plots[owner[f]].cells.push_back(f);
      out.plots[owner[f]].areaMetres += mesh.areaMetres(f);
    }

    for (CityPlot &plot : out.plots)
    {
      for (const uint32_t cell : plot.cells)
      {
        for (uint32_t k = adjacency.offset[cell]; k < adjacency.offset[cell + 1]; ++k)
        {
          plot.onStreet = plot.onStreet || isRoad(adjacency.neighbour[k]);
        }
      }
    }
  }

  // ---- 5. Roads -----------------------------------------------------------
  const StepCost artery = [&](const uint32_t previous, const uint32_t from, const uint32_t to) {
    const CellKind kind = out.cells[to].kind;

    // A wall may only be crossed at a gate. That is the whole rule, and on a
    // cell complex it is one comparison rather than a segment intersection.
    if (kind == CellKind::Wall || kind == CellKind::Tower)
    {
      return kBlocked;
    }

    const double base = distance(centre[from], centre[to]);
    const double rise = std::abs(double(out.cells[to].height) - double(out.cells[from].height));
    const double grade = base > 0 ? rise / base : 0;

    if (grade > settings.maximumGrade * 3)
    {
      return kBlocked;
    }

    double step = base + settings.gradeWeight * grade * grade * base + turnPenalty(previous, from, to, base);

    // Reuse: a cell that is already road is most of the way to being one, so
    // routes bundle instead of running side by side.
    if (isRoad(to))
    {
      step *= settings.reuseFactor;
    }

    if (kind == CellKind::Water)
    {
      step += settings.bridgeCost;
    }

    return step;
  };

  out.cells[plazaCell].kind = CellKind::Plaza;

  for (uint32_t k = adjacency.offset[plazaCell]; k < adjacency.offset[plazaCell + 1]; ++k)
  {
    const uint32_t next = adjacency.neighbour[k];

    if (out.cells[next].kind != CellKind::Wall && out.cells[next].kind != CellKind::Gate && out.cells[next].kind != CellKind::Water && distance(centre[next], centre[plazaCell]) < settings.plazaRadiusMetres)
    {
      out.cells[next].kind = CellKind::Plaza;
    }
  }

  // Gates in a fixed order, so a later artery can reuse an earlier one and the
  // result does not depend on the order they were discovered in.
  std::vector<uint32_t> gateOrder = gateCandidates;
  std::sort(gateOrder.begin(), gateOrder.end());

  for (const uint32_t gate : settings.fabric == FabricMode::Subdivide ? gateOrder : std::vector<uint32_t>{})
  {
    std::vector<uint32_t> path;
    RouteReport report;

    if (!routeCells(mesh, adjacency, gate, plazaCell, artery, routing, path, report))
    {
      out.cells[gate].kind = CellKind::Wall;
      ++out.metrics.wall;
      --out.metrics.gates;
      ++out.metrics.droppedGates;
      out.ledger.push_back("gate at cell " + std::to_string(gate) + " dropped: no route to the centre");

      continue;
    }

    out.gates.push_back(gate);

    for (size_t i = 0; i < path.size(); ++i)
    {
      const uint32_t cell = path[i];

      if (out.cells[cell].kind == CellKind::Gate || out.cells[cell].kind == CellKind::Plaza)
      {
        continue;
      }

      if (out.cells[cell].kind == CellKind::Water)
      {
        out.cells[cell].kind = CellKind::Bridge;
      }
      else
      {
        out.cells[cell].kind = CellKind::Road;
      }

      out.cells[cell].roadClass = 1;

      // The side the route enters by: the socket the gatehouse, the bridge
      // parapet and the flight of steps will all be oriented against.
      if (i > 0)
      {
        out.cells[cell].orientation = sideTowards(mesh, cell, path[i - 1]);
      }
    }

    if (path.size() > 1)
    {
      out.cells[gate].orientation = sideTowards(mesh, gate, path[1]);
    }

    // No centreline is recorded here. Thinning has not run yet, so anything
    // written now describes cells that may not survive it; the routes are
    // rebuilt from the finished network below.
  }

  // ---- 6. Bridges, fords and steps ---------------------------------------
  //
  // A crossing one cell long over shallow water is a ford, not a bridge: it
  // costs nothing to build and it is why a route prefers a shallow reach. A run
  // longer than the budget is no crossing at all - the cells go back to water
  // and the route that made them is recorded as unbuilt.
  {
    std::vector<bool> counted(count, false);

    for (uint32_t f = 0; f < count; ++f)
    {
      if (out.cells[f].kind != CellKind::Bridge || counted[f])
      {
        continue;
      }

      std::vector<bool> seen(count, false);
      std::vector<bool> isBridge(count, false);

      for (uint32_t g = 0; g < count; ++g)
      {
        isBridge[g] = out.cells[g].kind == CellKind::Bridge;
      }

      const std::vector<uint32_t> run = componentOf(adjacency, isBridge, f, seen);

      for (const uint32_t cell : run)
      {
        counted[cell] = true;
      }

      if (run.size() > settings.maximumBridgeCells)
      {
        for (const uint32_t cell : run)
        {
          out.cells[cell].kind = CellKind::Water;
          out.cells[cell].roadClass = 0;
          out.cells[cell].orientation = kNoSide8;
        }

        out.ledger.push_back("a crossing of " + std::to_string(run.size()) + " cells exceeded the bridge budget and was abandoned");

        continue;
      }

      if (run.size() == 1 && settings.fordDepthMetres > 0)
      {
        out.cells[run.front()].kind = CellKind::Ford;
      }
    }
  }

  // A road step between two columns whose ground differs by a layer is a
  // flight, oriented uphill. This is where the vertical axis first shows up in
  // the plan rather than only in the data.
  for (uint32_t f = 0; f < count; ++f)
  {
    if (out.cells[f].kind != CellKind::Road)
    {
      continue;
    }

    uint8_t uphill = kNoSide8;
    int32_t rise = 0;

    for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
    {
      const uint32_t next = adjacency.neighbour[k];

      if (!isRoad(next))
      {
        continue;
      }

      const int32_t difference = int32_t(out.cells[next].layerGround) - int32_t(out.cells[f].layerGround);

      if (difference > rise)
      {
        rise = difference;
        uphill = uint8_t(adjacency.side[k]);
      }
    }

    if (rise >= 1 && uphill != kNoSide8)
    {
      out.cells[f].kind = CellKind::Steps;
      out.cells[f].orientation = uphill;
    }
  }

  // ---- 7. Towers ----------------------------------------------------------
  //
  // Where the wall turns sharply, thinned so two do not sit on top of each
  // other. Measured from the wall's own neighbours rather than from a circuit
  // walk, because on a cell complex a corner is a local fact.
  {
    std::vector<uint32_t> placed;

    for (uint32_t f = 0; f < count; ++f)
    {
      if (out.cells[f].kind != CellKind::Wall)
      {
        continue;
      }

      std::vector<uint32_t> along;

      for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
      {
        const uint32_t next = adjacency.neighbour[k];

        if (out.cells[next].kind == CellKind::Wall || out.cells[next].kind == CellKind::Gate || out.cells[next].kind == CellKind::Tower)
        {
          along.push_back(next);
        }
      }

      if (along.size() != 2)
      {
        continue;
      }

      const Flat &a = centre[along[0]], &b = centre[f], &c = centre[along[1]];
      const double ux = b.x - a.x, uy = b.y - a.y;
      const double vx = c.x - b.x, vy = c.y - b.y;
      const double lu = std::sqrt(ux * ux + uy * uy), lv = std::sqrt(vx * vx + vy * vy);

      if (lu <= 0 || lv <= 0)
      {
        continue;
      }

      const double cosine = std::clamp((ux * vx + uy * vy) / (lu * lv), -1.0, 1.0);
      const double turn = std::acos(cosine) * 180.0 / 3.14159265358979323846;

      if (turn < settings.towerTurnDegrees)
      {
        continue;
      }

      bool tooClose = false;

      for (const uint32_t already : placed)
      {
        tooClose = tooClose || distance(centre[already], centre[f]) < settings.towerSpacingMetres;
      }

      if (tooClose)
      {
        continue;
      }

      out.cells[f].kind = CellKind::Tower;
      --out.metrics.wall;
      ++out.metrics.towers;
      placed.push_back(f);
    }
  }

  // ---- 8. Blocks ----------------------------------------------------------
  //
  // Whatever is left, as connected components. Because the classes above are a
  // partition of the cells, blocks are exhaustive by construction rather than
  // by search, and the test that says so is a set identity.
  const auto deriveBlocks = [&]() {
    out.blocks.clear();

    std::vector<bool> spare(count, false);

    for (uint32_t f = 0; f < count; ++f)
    {
      spare[f] = out.cells[f].kind == CellKind::Block || out.cells[f].kind == CellKind::Unassigned;
      out.cells[f].block = kNoBlock;
    }

    std::vector<bool> seen(count, false);

    for (uint32_t f = 0; f < count; ++f)
    {
      const std::vector<uint32_t> component = componentOf(adjacency, spare, f, seen);

      if (component.empty())
      {
        continue;
      }

      CityBlockInfo block;
      block.id = uint32_t(out.blocks.size());
      block.cells = component;

      for (const uint32_t cell : component)
      {
        out.cells[cell].kind = CellKind::Block;
        out.cells[cell].block = block.id;
        block.areaMetres += mesh.areaMetres(cell);

        // A court counts as frontage. A house whose only opening is onto a
        // yard is reached through a pend rather than off the street, which is
        // ordinary in the towns this is modelled on; what would be wrong is a
        // house with neither.
        for (uint32_t k = adjacency.offset[cell]; k < adjacency.offset[cell + 1]; ++k)
        {
          const uint32_t next = adjacency.neighbour[k];

          if (isRoad(next) || out.cells[next].kind == CellKind::Court)
          {
            block.frontage.push_back(cell);

            break;
          }
        }
      }

      block.landlocked = block.frontage.empty();
      out.blocks.push_back(std::move(block));
    }
  };

  deriveBlocks();

  // ---- 9. Alleys ----------------------------------------------------------
  //
  // Two jobs for the same router, one level down in spirit if not yet in the
  // hierarchy: split a block too big to be a block, and reach one that nothing
  // reaches. Without the first the city is a wall with four roads in it and
  // eight enormous fields - which is what the first run produced, and which
  // every invariant passed.
  {
    const StepCost alley = [&](const uint32_t previous, const uint32_t from, const uint32_t to) {
      const CellKind kind = out.cells[to].kind;

      // A tower is solid. A stretch of wall is not: a pocket of the city with
      // no other way in gets a postern cut through it, which is what a postern
      // is for. Forbidding the wall outright leaves those pockets unreachable,
      // and the metric that said otherwise was only hiding them.
      if (kind == CellKind::Tower)
      {
        return kBlocked;
      }

      const double base = distance(centre[from], centre[to]);
      const double step = base * (isRoad(to) ? settings.reuseFactor : 1.0) + turnPenalty(previous, from, to, base);

      if (kind == CellKind::Wall)
      {
        return step + settings.posternCost;
      }

      // Water is crossable at the bridge price rather than forbidden: a block
      // on the far bank is reachable, it just costs a bridge to reach.
      return kind == CellKind::Water ? step + settings.bridgeCost : step;
    };

    // Splitting. Only when the fabric was subdivided: an accreted city already
    // has plot-sized blocks, and cutting them again would be cutting up the
    // houses.
    //
    // A block is cut between the two frontage cells furthest apart,
    // through its own cells only, and the pieces are re-derived and considered
    // again. Working from the largest block each round keeps the result
    // independent of the order the blocks happened to be found in.
    for (uint32_t round = 0; settings.fabric == FabricMode::Subdivide && round < settings.maximumAlleys; ++round)
    {
      uint32_t worst = kNoFace;
      double worstArea = settings.maximumBlockAreaMetres;

      for (const CityBlockInfo &block : out.blocks)
      {
        if (block.areaMetres > worstArea && block.frontage.size() >= 2)
        {
          worstArea = block.areaMetres;
          worst = block.id;
        }
      }

      if (worst == kNoFace)
      {
        break;
      }

      const CityBlockInfo &block = out.blocks[worst];

      // Cut ACROSS the block's long axis, through its middle, not between the
      // two frontage cells furthest apart.
      //
      // The difference is the whole cost of the street network. A cut between
      // the furthest pair runs the length of the block and wanders diagonally;
      // a cut across the short axis is a fraction as long and leaves two pieces
      // that are each closer to square. Measured: the first version paved 42 %
      // of the city, which is a car park, not a town.
      Flat middle{0, 0};

      for (const uint32_t cell : block.cells)
      {
        middle.x += centre[cell].x;
        middle.y += centre[cell].y;
      }

      middle.x /= double(block.cells.size());
      middle.y /= double(block.cells.size());

      double xx = 0, xy = 0, yy = 0;

      for (const uint32_t cell : block.cells)
      {
        const double dx = centre[cell].x - middle.x, dy = centre[cell].y - middle.y;
        xx += dx * dx;
        xy += dx * dy;
        yy += dy * dy;
      }

      // Principal direction of the cell cloud, from the covariance: the
      // eigenvector of the larger eigenvalue, written without an eigensolver.
      const double trace = xx + yy;
      const double gap = std::sqrt(std::max(0.0, (xx - yy) * (xx - yy) + 4 * xy * xy));
      const double bigger = (trace + gap) * .5;

      Flat major{xx - (yy - bigger) == 0 && xy == 0 ? 1.0 : bigger - yy, xy};

      if (std::abs(major.x) + std::abs(major.y) < 1e-9)
      {
        major = {1, 0};
      }

      const double majorLength = std::sqrt(major.x * major.x + major.y * major.y);
      major.x /= majorLength;
      major.y /= majorLength;

      const Flat minor{-major.y, major.x};

      // How wide the block is across its own short direction. A cut runs across
      // the minor axis and halves the major one, so the pieces keep this width -
      // and if it is already the width of a house row, there is nothing to gain
      // by making two shorter rows and another street between them.
      double narrowest = 0, widest = 0;
      bool firstCell = true;

      for (const uint32_t cell : block.cells)
      {
        const double across = (centre[cell].x - middle.x) * minor.x + (centre[cell].y - middle.y) * minor.y;
        narrowest = firstCell ? across : std::min(narrowest, across);
        widest = firstCell ? across : std::max(widest, across);
        firstCell = false;
      }

      if (widest - narrowest < settings.minimumBlockWidthMetres)
      {
        out.blocks[worst].areaMetres = 0;

        continue;
      }

      std::vector<double> along;

      for (const uint32_t cell : block.frontage)
      {
        along.push_back((centre[cell].x - middle.x) * major.x + (centre[cell].y - middle.y) * major.y);
      }

      std::vector<double> sorted = along;
      std::sort(sorted.begin(), sorted.end());
      const double median = sorted[sorted.size() / 2];
      const double reach = std::max(1.0, (sorted.back() - sorted.front()) * .22);

      uint32_t head = kNoFace, tail = kNoFace;
      double lowest = 0, highest = 0;

      for (size_t i = 0; i < block.frontage.size(); ++i)
      {
        if (std::abs(along[i] - median) > reach)
        {
          continue;
        }

        const uint32_t cell = block.frontage[i];
        const double across = (centre[cell].x - middle.x) * minor.x + (centre[cell].y - middle.y) * minor.y;

        if (head == kNoFace || across < lowest)
        {
          lowest = across;
          head = cell;
        }

        if (tail == kNoFace || across > highest)
        {
          highest = across;
          tail = cell;
        }
      }

      if (head == kNoFace || tail == kNoFace || head == tail)
      {
        out.blocks[worst].areaMetres = 0;

        continue;
      }

      const uint32_t owner = block.id;

      const StepCost inside = [&](const uint32_t previous, const uint32_t from, const uint32_t to) {
        if (out.cells[to].block != owner || out.cells[to].kind != CellKind::Block)
        {
          return kBlocked;
        }

        const double base = distance(centre[from], centre[to]);

        return base + turnPenalty(previous, from, to, base);
      };

      std::vector<uint32_t> path;
      RouteReport report;

      if (!routeCells(mesh, adjacency, head, tail, inside, routing, path, report) || path.size() < 3)
      {
        // Nothing can be cut here; stop considering this block by letting it
        // keep its size rather than looping on it.
        out.blocks[worst].areaMetres = 0;

        continue;
      }

      for (const uint32_t cell : path)
      {
        out.cells[cell].kind = CellKind::Road;
        out.cells[cell].roadClass = 3;
      }

      ++out.metrics.alleys;
      deriveBlocks();
    }

    // Reaching. A landlocked block is a real failure and must be visible, so it
    // is routed to rather than tolerated.
    for (uint32_t attempt = 0; attempt < out.blocks.size(); ++attempt)
    {
      uint32_t stuck = kNoFace;

      for (const CityBlockInfo &block : out.blocks)
      {
        if (block.landlocked && !block.cells.empty())
        {
          stuck = block.id;

          break;
        }
      }

      if (stuck == kNoFace)
      {
        break;
      }

      const uint32_t seed = out.blocks[stuck].cells.front();
      std::vector<uint32_t> path;
      RouteReport report;

      if (!routeCells(mesh, adjacency, seed, plazaCell, alley, routing, path, report))
      {
        out.ledger.push_back("block " + std::to_string(stuck) + " is landlocked and could not be reached");
        out.blocks[stuck].landlocked = false; // Stop retrying it...
        out.blocks[stuck].unreachable = true; // ...but say so in the metrics.

        continue;
      }

      ++out.metrics.alleys;

      size_t index = 0;

      for (const uint32_t cell : path)
      {
        if (out.cells[cell].kind == CellKind::Water)
        {
          out.cells[cell].kind = CellKind::Bridge;
          out.cells[cell].roadClass = 3;
          out.cells[cell].orientation = index > 0 ? sideTowards(mesh, cell, path[index - 1]) : kNoSide8;
          ++index;

          continue;
        }

        ++index;

        // A wall cell the alley had to pass through becomes a postern: still
        // part of the circuit, and now a way in.
        if (out.cells[cell].kind == CellKind::Wall)
        {
          out.cells[cell].kind = CellKind::Gate;
          out.cells[cell].roadClass = 3;
          out.cells[cell].orientation = index > 1 ? sideTowards(mesh, cell, path[index - 2]) : kNoSide8;
          out.gates.push_back(cell);

          continue;
        }

        if (out.cells[cell].kind == CellKind::Block && out.cells[cell].block != stuck)
        {
          out.cells[cell].kind = CellKind::Road;
          out.cells[cell].roadClass = 3;
        }
      }

      deriveBlocks();
    }
  }

  // ---- 9a. One cell thick ------------------------------------------------
  //
  // Routing lays a path per route, and where two routes run beside each other
  // the result is two cells of road doing one cell of work. On the real city
  // that was 6 782 road cells against a perfect one-wide network's ~4 700, and
  // the render was more street than town.
  //
  // Two passes. A dead end that serves nothing is pruned; then any road cell
  // whose neighbours would stay connected without it goes back to being ground.
  // The second test is local - three hops, not a flood of the whole city - which
  // is what makes it affordable at 27 000 cells.
  if (settings.fabric == FabricMode::Subdivide)
  {
    const auto paved = [&](const uint32_t f) {
      const CellKind kind = out.cells[f].kind;

      return kind == CellKind::Road || kind == CellKind::Steps;
    };

    const auto onNetwork = [&](const uint32_t f) { return isRoad(f); };

    // Dead ends first: a road cell with at most one road neighbour, leading
    // nowhere, is an artefact of routing rather than a street.
    for (uint32_t pass = 0; pass < 8; ++pass)
    {
      uint32_t pruned = 0;

      for (uint32_t f = 0; f < count; ++f)
      {
        if (!paved(f))
        {
          continue;
        }

        uint32_t neighbours = 0;

        for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
        {
          neighbours += onNetwork(adjacency.neighbour[k]) ? 1 : 0;
        }

        if (neighbours <= 1)
        {
          out.cells[f].kind = CellKind::Unassigned;
          out.cells[f].roadClass = 0;
          out.cells[f].orientation = kNoSide8;
          ++pruned;
        }
      }

      if (pruned == 0)
      {
        break;
      }
    }

    // Then thinning, repeated until it settles.
    //
    // One sweep is not enough: a cell is judged against the network as it
    // stands, and removing its neighbour later in the same sweep can make it
    // removable too. Measured at one pass, 23 % of road cells still had road on
    // every side.
    //
    // Cells are taken in descending order of how many road neighbours they
    // have, so the middle of a wide patch goes before its edge; ties break on
    // the index, so the order is fixed and the result does not depend on the
    // sweep.
    for (uint32_t sweep = 0; sweep < 8; ++sweep)
    {
    uint32_t removed = 0;

    std::vector<std::pair<uint32_t, uint32_t>> order;

    for (uint32_t f = 0; f < count; ++f)
    {
      if (!paved(f))
      {
        continue;
      }

      uint32_t neighbours = 0;

      for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
      {
        neighbours += onNetwork(adjacency.neighbour[k]) ? 1 : 0;
      }

      order.push_back({neighbours, f});
    }

    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) {
      return a.first != b.first ? a.first > b.first : a.second < b.second;
    });

    for (const auto &entry : order)
    {
      const uint32_t f = entry.second;

      if (!paved(f))
      {
        continue;
      }

      std::vector<uint32_t> attached;

      for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
      {
        if (onNetwork(adjacency.neighbour[k]))
        {
          attached.push_back(adjacency.neighbour[k]);
        }
      }

      if (attached.size() < 2)
      {
        continue;
      }

      // Would the neighbours still reach each other without this cell? Searched
      // only a few hops out, which is enough to tell a redundant cell in a wide
      // patch from the one cell holding two streets together.
      std::map<uint32_t, uint32_t> depth;
      std::vector<uint32_t> queue{attached.front()};
      depth[attached.front()] = 0;

      for (size_t at = 0; at < queue.size(); ++at)
      {
        const uint32_t here = queue[at];

        if (depth[here] >= 4)
        {
          continue;
        }

        for (uint32_t k = adjacency.offset[here]; k < adjacency.offset[here + 1]; ++k)
        {
          const uint32_t next = adjacency.neighbour[k];

          if (next == f || !onNetwork(next) || depth.count(next) != 0)
          {
            continue;
          }

          depth[next] = depth[here] + 1;
          queue.push_back(next);
        }
      }

      bool joined = true;

      for (const uint32_t other : attached)
      {
        joined = joined && depth.count(other) != 0;
      }

      if (joined)
      {
        out.cells[f].kind = CellKind::Unassigned;
        out.cells[f].roadClass = 0;
        out.cells[f].orientation = kNoSide8;
        ++removed;
      }
    }

    if (removed == 0)
    {
      break;
    }
    }

    // Pruning again: thinning can leave a stub that led somewhere before.
    for (uint32_t pass = 0; pass < 8; ++pass)
    {
      uint32_t pruned = 0;

      for (uint32_t f = 0; f < count; ++f)
      {
        if (!paved(f))
        {
          continue;
        }

        uint32_t neighbours = 0;

        for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
        {
          neighbours += onNetwork(adjacency.neighbour[k]) ? 1 : 0;
        }

        if (neighbours <= 1)
        {
          out.cells[f].kind = CellKind::Unassigned;
          out.cells[f].roadClass = 0;
          out.cells[f].orientation = kNoSide8;
          ++pruned;
        }
      }

      if (pruned == 0)
      {
        break;
      }
    }

    // Finally, anything the thinning cut adrift. A ring of four cells in the
    // middle of a block has two road neighbours each, so pruning will not touch
    // it and it survives as a little paved island connected to nothing.
    {
      std::vector<bool> network(count, false);

      for (uint32_t f = 0; f < count; ++f)
      {
        network[f] = onNetwork(f);
      }

      std::vector<bool> seen(count, false);
      const std::vector<uint32_t> main = componentOf(adjacency, network, plazaCell, seen);
      std::vector<bool> connected(count, false);

      for (const uint32_t f : main)
      {
        connected[f] = true;
      }

      uint32_t stranded = 0;

      for (uint32_t f = 0; f < count; ++f)
      {
        if (!paved(f) || connected[f])
        {
          continue;
        }

        out.cells[f].kind = CellKind::Unassigned;
        out.cells[f].roadClass = 0;
        out.cells[f].orientation = kNoSide8;
        ++stranded;
      }

      if (stranded != 0)
      {
        out.ledger.push_back("removed " + std::to_string(stranded) + " road cell(s) cut adrift by thinning");
      }
    }

    deriveBlocks();
  }

  // ---- 9c. Routes, rebuilt on the network that actually exists ------------
  //
  // Gate to plaza, restricted to cells that came through the thinning, so the
  // chain is on the road by construction rather than by having been on it
  // earlier. Then Chaikin over the chain: a cell sequence turns in ninety degree
  // steps and the curve a mesher follows should not.
  {
    const StepCost along = [&](const uint32_t previous, const uint32_t from, const uint32_t to) {
      if (!isRoad(to))
      {
        return kBlocked;
      }

      const double base = distance(centre[from], centre[to]);

      return base + turnPenalty(previous, from, to, base);
    };

    // Where a route starts. With a wall it is a gate; without one it is the
    // street cell on the rim of the grid nearest each of the world's own road
    // exits, which is the same question asked of a town that never walled
    // itself.
    std::vector<uint32_t> entries = out.gates;

    if (entries.empty())
    {
      for (const Position &exit : input.exits)
      {
        uint32_t best = kNoFace;
        double bestDistance = 0;

        for (uint32_t f = 0; f < count; ++f)
        {
          if (!isRoad(f) || adjacency.offset[f + 1] - adjacency.offset[f] >= 4)
          {
            continue;
          }

          const double d = distance(centre[f], {exit.x, exit.z});

          if (best == kNoFace || d < bestDistance)
          {
            best = f;
            bestDistance = d;
          }
        }

        if (best != kNoFace && std::find(entries.begin(), entries.end(), best) == entries.end())
        {
          entries.push_back(best);
        }
      }
    }

    for (const uint32_t gate : entries)
    {
      std::vector<uint32_t> path;
      RouteReport report;

      if (!routeCells(mesh, adjacency, gate, plazaCell, along, routing, path, report))
      {
        out.ledger.push_back("gate at cell " + std::to_string(gate) + " has no route left after thinning");

        continue;
      }

      CityRoute route;
      route.gate = gate;
      route.roadClass = out.cells[gate].roadClass != 0 ? out.cells[gate].roadClass : uint16_t(1);
      route.cells = path;

      for (const uint32_t cell : path)
      {
        route.line.push_back({centre[cell].x, double(out.cells[cell].height), centre[cell].y});
      }

      // Open corner cutting: the two ends stay put so the curve still starts at
      // the gate and finishes at the plaza, and everything between is rounded.
      for (uint32_t pass = 0; pass < settings.routeSmoothingPasses && route.line.size() > 2; ++pass)
      {
        std::vector<Position> cut;
        cut.reserve(route.line.size() * 2);
        cut.push_back(route.line.front());

        for (size_t i = 0; i + 1 < route.line.size(); ++i)
        {
          const Position &a = route.line[i];
          const Position &b = route.line[i + 1];

          cut.push_back({a.x * .75 + b.x * .25, a.y * .75 + b.y * .25, a.z * .75 + b.z * .25});
          cut.push_back({a.x * .25 + b.x * .75, a.y * .25 + b.y * .75, a.z * .25 + b.z * .75});
        }

        cut.push_back(route.line.back());
        route.line.swap(cut);
      }

      out.routes.push_back(std::move(route));
    }
  }

  // ---- 9b. Smoothed feature outlines --------------------------------------
  //
  // The boundary of a cell set is a staircase of quad edges, and anything built
  // straight along it - a wall, a river bank, a kerb - inherits every corner of
  // the grid. Chaikin's corner cutting turns that into a curve. It is applied
  // to a COPY: the cells are still what every topological question is asked of,
  // and this is only where the geometry runs.
  if (settings.smoothOutlines)
  {
    const auto extract = [&](const std::function<bool(uint32_t)> &inSet, const OutlineRole role, const uint32_t feature) {
      // A boundary side has its face in the set and its twin's face outside it.
      std::map<uint32_t, uint32_t> outgoing; // Vertex -> the side leaving it.

      for (uint32_t f = 0; f < count; ++f)
      {
        if (!inSet(f))
        {
          continue;
        }

        for (uint32_t i = 0; i < 4; ++i)
        {
          const uint32_t side = sideOf(f, i);
          const uint32_t twin = mesh.sides[side].twin;

          if (twin == kNoSide || !inSet(geom2::faceOf(twin)))
          {
            outgoing.emplace(mesh.sides[side].origin, side);
          }
        }
      }

      std::vector<bool> walked(mesh.sides.size(), false);

      for (const auto &entry : outgoing)
      {
        if (walked[entry.second])
        {
          continue;
        }

        std::vector<Position> loop;
        uint32_t side = entry.second;

        for (uint32_t guard = 0; guard < mesh.sides.size(); ++guard)
        {
          if (walked[side])
          {
            break;
          }

          walked[side] = true;

          const geom2::Point &p = mesh.vertices[mesh.sides[side].origin];
          loop.push_back({toMetres(p.x), 0, toMetres(p.y)});

          const auto next = outgoing.find(mesh.destination(side));

          if (next == outgoing.end())
          {
            break;
          }

          side = next->second;
        }

        if (loop.size() < 4)
        {
          continue;
        }

        CityOutline outline;
        outline.role = role;
        outline.feature = feature;

        for (size_t i = 0; i < loop.size(); ++i)
        {
          const Position &a = loop[i];
          const Position &b = loop[(i + 1) % loop.size()];
          outline.rawLengthMetres += std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
        }

        // Chaikin: replace each vertex by the points a quarter and three
        // quarters along its two edges. Closed, so the loop stays closed.
        for (uint32_t pass = 0; pass < settings.outlineSmoothingPasses; ++pass)
        {
          std::vector<Position> cut;
          cut.reserve(loop.size() * 2);

          for (size_t i = 0; i < loop.size(); ++i)
          {
            const Position &a = loop[i];
            const Position &b = loop[(i + 1) % loop.size()];

            cut.push_back({a.x * .75 + b.x * .25, 0, a.z * .75 + b.z * .25});
            cut.push_back({a.x * .25 + b.x * .75, 0, a.z * .25 + b.z * .75});
          }

          loop.swap(cut);
        }

        outline.loop = std::move(loop);
        out.outlines.push_back(std::move(outline));
      }
    };

    extract([&](const uint32_t f) { const CellKind k = out.cells[f].kind; return k == CellKind::Wall || k == CellKind::Tower || k == CellKind::Gate; }, OutlineRole::Wall, 0);
    extract([&](const uint32_t f) { const CellKind k = out.cells[f].kind; return k == CellKind::Water || k == CellKind::Bridge || k == CellKind::Ford; }, OutlineRole::Water, 0);

    // The kerb. A road cell sequence is as straight as this grid allows - the
    // mean turn floors at about twenty degrees whatever the turn penalty, which
    // is the angular noise between adjacent centroids rather than a bad route -
    // so the remaining jaggedness is the cells themselves, and the only thing
    // that can smooth it is a curve that does not have to pass through their
    // corners.
    extract([&](const uint32_t f) { return isRoad(f); }, OutlineRole::Road, 0);

    for (const CityBlockInfo &block : out.blocks)
    {
      if (block.cells.empty())
      {
        continue;
      }

      const uint32_t id = block.id;
      extract([&](const uint32_t f) { return out.cells[f].kind == CellKind::Block && out.cells[f].block == id; }, OutlineRole::Block, id);
    }
  }

  // ---- 10. Measure --------------------------------------------------------
  //
  // Counted from the finished cells rather than accumulated as they were
  // assigned: a cell can change class three times on the way here, and a
  // running total would record every one of them.
  const uint32_t alleys = out.metrics.alleys;
  const uint32_t droppedGates = out.metrics.droppedGates;

  out.metrics = {};
  out.metrics.alleys = alleys;
  out.metrics.droppedGates = droppedGates;
  out.metrics.cells = count;
  out.metrics.minimumPassageMetres = 1e9;

  for (uint32_t f = 0; f < count; ++f)
  {
    switch (out.cells[f].kind)
    {
    case CellKind::Water:
      ++out.metrics.water;
      break;
    case CellKind::Ford:
      ++out.metrics.ford;
      break;
    case CellKind::Wall:
      ++out.metrics.wall;
      break;
    case CellKind::Tower:
      ++out.metrics.towers;
      break;
    case CellKind::Gate:
      ++out.metrics.gates;
      break;
    case CellKind::Road:
      ++out.metrics.road;
      break;
    case CellKind::Bridge:
      ++out.metrics.bridge;
      break;
    case CellKind::Steps:
      ++out.metrics.steps;
      break;
    case CellKind::Plaza:
      ++out.metrics.plaza;
      break;
    case CellKind::Block:
      ++out.metrics.block;
      break;
    case CellKind::Court:
      ++out.metrics.court;
      break;
    default:
      ++out.metrics.unassigned;
      break;
    }

    out.metrics.maximumLayer = std::max(out.metrics.maximumLayer, out.cells[f].layerGround);

    if (isRoad(f) || out.cells[f].kind == CellKind::Wall || out.cells[f].kind == CellKind::Tower)
    {
      out.metrics.minimumPassageMetres = std::min(out.metrics.minimumPassageMetres, passageWidth(mesh, f));

      if (isRoad(f))
      {
        out.metrics.roadLengthMetres += std::sqrt(mesh.areaMetres(f));
      }
    }
  }

  // How straight the chains actually came out, measured the same way for roads
  // and for the wall: the turn at every cell with exactly two neighbours of its
  // own kind.
  {
    const auto chainTurn = [&](const std::function<bool(uint32_t)> &member) {
      double total = 0;
      uint32_t counted = 0;

      for (uint32_t f = 0; f < count; ++f)
      {
        if (!member(f))
        {
          continue;
        }

        std::vector<uint32_t> along;

        for (uint32_t k = adjacency.offset[f]; k < adjacency.offset[f + 1]; ++k)
        {
          if (member(adjacency.neighbour[k]))
          {
            along.push_back(adjacency.neighbour[k]);
          }
        }

        if (along.size() != 2)
        {
          continue;
        }

        const Flat &a = centre[along[0]], &b = centre[f], &c = centre[along[1]];
        const double ux = b.x - a.x, uy = b.y - a.y;
        const double vx = c.x - b.x, vy = c.y - b.y;
        const double lu = std::sqrt(ux * ux + uy * uy), lv = std::sqrt(vx * vx + vy * vy);

        if (lu <= 0 || lv <= 0)
        {
          continue;
        }

        const double cosine = std::clamp((ux * vx + uy * vy) / (lu * lv), -1.0, 1.0);
        total += std::acos(cosine) * 180.0 / 3.14159265358979323846;
        ++counted;
      }

      return counted != 0 ? total / double(counted) : 0.0;
    };

    out.metrics.meanRoadTurnDegrees = chainTurn([&](const uint32_t f) { return isRoad(f); });
    out.metrics.meanWallTurnDegrees = chainTurn([&](const uint32_t f) {
      const CellKind kind = out.cells[f].kind;

      return kind == CellKind::Wall || kind == CellKind::Tower || kind == CellKind::Gate;
    });
  }

  out.metrics.blocks = uint32_t(out.blocks.size());
  out.metrics.plots = uint32_t(out.plots.size());

  for (const CityPlot &plot : out.plots)
  {
    out.metrics.plotless += plot.cells.empty() ? 1 : 0;
  }

  for (const CityBlockInfo &block : out.blocks)
  {
    out.metrics.landlocked += block.landlocked ? 1 : 0;
    out.metrics.unreachable += block.unreachable ? 1 : 0;
  }

  if (out.metrics.minimumPassageMetres > 1e8)
  {
    out.metrics.minimumPassageMetres = 0;
  }

  return true;
}

} // namespace procedural::world::citywfc
