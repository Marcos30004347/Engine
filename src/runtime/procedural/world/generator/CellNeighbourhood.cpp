#include "procedural/world/generator/CellNeighbourhood.hpp"

#include "procedural/world/generator/CellDwellingRealise.hpp"
#include "procedural/world/generator/DwellingStairs.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace procedural::world::generator {

using geom2::Chain;
using geom2::Point;
using rewrite::Position;

namespace {

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

// The neighbourhood's frame: `u` along the alley from `a`, `v` to the left of
// a->b. Row 0 is at v > 0, the alley at -alley < v < 0, row 1 below that.
struct Frame
{
  double ax = 0, az = 0, ux = 1, uz = 0;

  Position at(const double u, const double v, const double y) const
  {
    return Position{ax + u * ux - v * uz, y, az + u * uz + v * ux};
  }

  void local(const double x, const double z, double &u, double &v) const
  {
    const double dx = x - ax, dz = z - az;
    u = dx * ux + dz * uz;
    v = -dx * uz + dz * ux;
  }
};

std::vector<Position> quad(const Frame &frame, const double u0, const double v0, const double u1, const double v1, const double y)
{
  return {frame.at(u0, v0, y), frame.at(u1, v0, y), frame.at(u1, v1, y), frame.at(u0, v1, y)};
}

std::vector<Position> gradedQuad(const Frame &frame,
                                 const double u0,
                                 const double v0,
                                 const double u1,
                                 const double v1,
                                 const double above,
                                 const std::function<double(double, double)> &ground)
{
  std::vector<Position> out = quad(frame, u0, v0, u1, v1, 0);
  for (Position &p : out) p.y = ground(p.x, p.z) + above;
  return out;
}

// Whether two level rectangles in the frame touch or overlap.
bool touching(const Frame &frame, const NeighbourhoodDeck &p, const NeighbourhoodDeck &q)
{
  if (p.footprint.size() < 3 || q.footprint.size() < 3)
  {
    return false;
  }

  const auto box = [&](const NeighbourhoodDeck &deck, double &u0, double &v0, double &u1, double &v1) {
    u0 = v0 = 1e300;
    u1 = v1 = -1e300;

    for (const Position &pt : deck.footprint)
    {
      double u = 0, v = 0;
      frame.local(pt.x, pt.z, u, v);
      u0 = std::min(u0, u);
      u1 = std::max(u1, u);
      v0 = std::min(v0, v);
      v1 = std::max(v1, v);
    }
  };

  double pu0, pv0, pu1, pv1, qu0, qv0, qu1, qv1;
  box(p, pu0, pv0, pu1, pv1);
  box(q, qu0, qv0, qu1, qv1);

  const double overlapU = (std::max(pu0, qu0) + std::min(pu1, qu1)) * .5;
  const double overlapV = (std::max(pv0, qv0) + std::min(pv1, qv1)) * .5;
  const Position sample = frame.at(overlapU, overlapV, 0);
  if (std::abs(p.levelAt(sample.x, sample.z) - q.levelAt(sample.x, sample.z)) > 1e-3) return false;

  const bool uOverlap = std::min(pu1, qu1) - std::max(pu0, qu0) > .05;
  const bool vOverlap = std::min(pv1, qv1) - std::max(pv0, qv0) > .05;
  const bool uTouch = std::min(pu1, qu1) - std::max(pu0, qu0) > -1e-3;
  const bool vTouch = std::min(pv1, qv1) - std::max(pv0, qv0) > -1e-3;

  // Sharing a run of edge, not a corner: touching only at a point is not a
  // way through.
  return (uOverlap && vTouch) || (vOverlap && uTouch);
}

void lift(std::vector<Position> &ring, const double dy)
{
  for (Position &p : ring) p.y += dy;
}

void liftPlan(rewrite::DwellingPlan &plan, const double dy)
{
  if (std::abs(dy) < 1e-9) return;

  for (rewrite::DwellingRoom &room : plan.rooms)
  {
    lift(room.boundary, dy);
    if (room.ceilingHeight != 0) room.ceilingHeight += dy;
  }
  for (rewrite::DwellingWall &wall : plan.walls) { wall.a.y += dy; wall.b.y += dy; }
  for (rewrite::DwellingDoor &door : plan.doors) door.center.y += dy;
  for (rewrite::DwellingOpening &opening : plan.openings) { opening.a.y += dy; opening.b.y += dy; }
  for (rewrite::DwellingStair &stair : plan.stairs)
  {
    lift(stair.opening, dy); lift(stair.lowerLanding, dy); lift(stair.upperLanding, dy);
  }
  for (rewrite::DwellingTimber &timber : plan.timbers) { timber.a.y += dy; timber.b.y += dy; }
  for (rewrite::DwellingFurniture &item : plan.furniture) lift(item.footprint, dy);
  for (rewrite::DwellingWing &wing : plan.wings)
  {
    lift(wing.footprint, dy); wing.ridgeFrom.y += dy; wing.ridgeTo.y += dy;
    wing.eavesHeight += dy; wing.ridgeHeight += dy;
  }
  for (rewrite::DwellingRoofFace &roof : plan.roofs) lift(roof.polygon, dy);
  for (rewrite::DwellingStorey &storey : plan.storeys)
  {
    lift(storey.outer, dy); for (auto &hole : storey.holes) lift(hole, dy);
    storey.base += dy; storey.top += dy;
  }
  const auto liftPillar = [&](rewrite::DwellingPillar &pillar) {
    lift(pillar.footprint, dy); pillar.base += dy; pillar.top += dy;
  };
  for (rewrite::DwellingPillar &pillar : plan.pillars) liftPillar(pillar);
  for (rewrite::DwellingPillar &chimney : plan.chimneys) liftPillar(chimney);
  for (rewrite::DwellingFloor &floor : plan.floors)
  {
    floor.elevation += dy;
    for (auto &ring : floor.circulation) lift(ring, dy);
    for (auto &ring : floor.slabs) lift(ring, dy);
    for (auto &ring : floor.paving) lift(ring, dy);
  }
  for (auto &ceiling : plan.ceilings) lift(ceiling, dy);
  for (rewrite::DwellingFace &face : plan.ceilingFaces)
  {
    lift(face.outer, dy); for (auto &hole : face.holes) lift(hole, dy); face.height += dy;
  }
}

} // namespace

const char *name(const DeckKind kind)
{
  switch (kind)
  {
    case DeckKind::Alley:
      return "alley";
    case DeckKind::Gallery:
      return "gallery";
    case DeckKind::Landing:
      return "landing";
  }

  return "?";
}

double NeighbourhoodDeck::levelAt(const double x, const double z) const
{
  if (footprint.size() < 3) return level;

  // A deck is emitted as a planar quad. Recover its plane from any three
  // non-collinear plan points; this also gives the old constant `level` on
  // flat ground.
  const Position &a = footprint[0];
  for (size_t k = 1; k + 1 < footprint.size(); ++k)
  {
    const Position &b = footprint[k], &c = footprint[k + 1];
    const double det = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
    if (std::abs(det) < 1e-9) continue;
    const double u = ((x - a.x) * (c.z - a.z) - (c.x - a.x) * (z - a.z)) / det;
    const double v = ((b.x - a.x) * (z - a.z) - (x - a.x) * (b.z - a.z)) / det;
    return a.y + u * (b.y - a.y) + v * (c.y - a.y);
  }

  return level;
}

bool planCellNeighbourhood(const CellNeighbourhoodBrief &brief,
                           const CellDwellingSettings &settings,
                           CellNeighbourhood &out,
                           Ledger &ledger,
                           std::string *error)
{
  out = CellNeighbourhood();
  out.id = brief.id;
  out.a = brief.a;
  out.b = brief.b;
  out.alleyMetres = std::max(1.2, brief.alleyMetres);

  const double dx = geom2::toMetres(brief.b.x - brief.a.x);
  const double dz = geom2::toMetres(brief.b.y - brief.a.y);
  const double length = std::sqrt(dx * dx + dz * dz);

  if (length < 6.)
  {
    return fail(error, "a neighbourhood needs an alley longer than six metres");
  }

  Frame frame;
  frame.ax = geom2::toMetres(brief.a.x);
  frame.az = geom2::toMetres(brief.a.y);
  frame.ux = dx / length;
  frame.uz = dz / length;

  const auto terrain = [&](const double x, const double z) {
    return brief.terrain ? brief.terrain(x, z) : 0.;
  };

  // The two rows, each a street of stacks with gaps. Row 1's frontage runs the
  // other way along the far side of the alley, so its block is on ITS left.
  const auto row = [&](const uint32_t which, CellStreet &street) {
    CellStreetBrief sb;
    sb.id = brief.id * 10 + which;
    sb.seed = brief.seed * 131u + which;
    sb.depthMetres = brief.depthMetres;
    sb.building = brief.building;
    sb.gapCells = std::max(1u, brief.gapCells);
    sb.storeys = brief.storeys;
    sb.narrowestCells = 2;
    sb.widestCells = 3;

    if (which == 0)
    {
      sb.a = brief.a;
      sb.b = brief.b;
    }
    else
    {
      const Position pa = frame.at(0, -out.alleyMetres, 0);
      const Position pb = frame.at(length, -out.alleyMetres, 0);
      sb.a = Point{geom2::toUnit(pb.x), geom2::toUnit(pb.z)};
      sb.b = Point{geom2::toUnit(pa.x), geom2::toUnit(pa.z)};
    }

    return planCellStreet(sb, settings, street, ledger, error);
  };

  if (!row(0, out.rows[0]) || !row(1, out.rows[1]))
  {
    return false;
  }

  out.cellMetres = out.rows[0].cellMetres;
  const double cell = out.cellMetres;
  const double storey = out.rows[0].plots.empty() ? 3. : out.rows[0].plots.front().house.gridClass.storeyMetres;

  // Where each plot stands in the neighbourhood's frame.
  struct Stand
  {
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0; // v0 is the alley face.
    uint32_t storeys = 0;
    bool planned = false;
  };

  std::vector<Stand> stands[2];

  for (uint32_t r = 0; r < 2; ++r)
  {
    for (CellPlot &plot : out.rows[r].plots)
    {
      Stand stand;
      const double from = double(plot.fromCell) * cell;
      const double to = double(plot.fromCell + plot.widthCells) * cell;

      if (r == 0)
      {
        stand.u0 = from;
        stand.u1 = to;
        stand.v0 = 0;
        stand.v1 = double(plot.depthCells) * cell;
      }
      else
      {
        stand.u0 = length - to;
        stand.u1 = length - from;
        stand.v0 = -out.alleyMetres;
        stand.v1 = -out.alleyMetres - double(plot.depthCells) * cell;
      }

      stand.planned = plot.planned;
      stand.storeys = plot.planned ? uint32_t(plot.house.floors.size()) : 0u;
      const Position middle = frame.at((stand.u0 + stand.u1) * .5, stand.v0, 0);
      plot.foundation = terrain(middle.x, middle.z);
      stands[r].push_back(stand);

      if (plot.planned)
      {
        double under = 1e300;
        NeighbourhoodPlinth plinth;
        plinth.row = r;
        plinth.plot = uint32_t(stands[r].size() - 1);
        for (const Point &q : plot.lot.outer)
        {
          const double x = geom2::toMetres(q.x), z = geom2::toMetres(q.y);
          under = std::min(under, terrain(x, z));
          plinth.footprint.push_back(Position{x, plot.foundation, z});
        }
        if (plot.foundation - under > .05)
        {
          plinth.base = under;
          plinth.top = plot.foundation;
          out.plinths.push_back(std::move(plinth));
        }
      }
    }
  }

  // ---- the decks -----------------------------------------------------------
  //
  // The alley first, so it is deck 0 and the root of the walk.
  {
    NeighbourhoodDeck alley;
    alley.kind = DeckKind::Alley;
    alley.footprint = gradedQuad(frame, 0, -out.alleyMetres, length, 0, 0, terrain);
    alley.level = terrain(frame.at(length * .5, -out.alleyMetres * .5, 0).x,
                          frame.at(length * .5, -out.alleyMetres * .5, 0).z);
    out.decks.push_back(std::move(alley));
  }

  // The galleries: one per row per upper storey, the length of the row, half
  // the alley deep, so the two rows' galleries at one level meet in the middle
  // and are one deck with doors on both sides. Half the alley and not a fixed
  // depth: the pulled planner's fixed gallery over a narrow alley measured
  // every wall as blocked.
  const double galleryDepth = out.alleyMetres * .5;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> galleryAt; // (row, storey) -> deck

  for (uint32_t r = 0; r < 2; ++r)
  {
    uint32_t tallest = 0;

    for (const Stand &stand : stands[r])
    {
      tallest = std::max(tallest, stand.storeys);
    }

    for (uint32_t s = 1; s < tallest; ++s)
    {
      NeighbourhoodDeck gallery;
      gallery.kind = DeckKind::Gallery;
      gallery.row = r;
      gallery.storey = s;
      gallery.level = terrain(frame.at(length * .5, r == 0 ? 0 : -out.alleyMetres, 0).x,
                              frame.at(length * .5, r == 0 ? 0 : -out.alleyMetres, 0).z) + double(s) * storey;
      gallery.footprint = r == 0 ? gradedQuad(frame, 0, -galleryDepth, length, 0, double(s) * storey, terrain)
                                 : gradedQuad(frame, 0, -out.alleyMetres, length, -out.alleyMetres + galleryDepth,
                                              double(s) * storey, terrain);
      galleryAt[{r, s}] = uint32_t(out.decks.size());
      out.decks.push_back(std::move(gallery));
    }
  }

  // ---- the stairs ----------------------------------------------------------
  //
  // One dog-leg per gap that has two neighbours, serving both: two flights a
  // storey and a half landing at the far end, returning to the alley end at
  // every level, which is where the galleries are. A flight outside a building
  // runs BESIDE it, in the gap, and arrives at the gallery's short end - never
  // across the alley, never along the front the doors are on.
  const double strip = std::min(.9, (double(std::max(1u, brief.gapCells)) * cell - .3) * .5);

  for (uint32_t r = 0; r < 2; ++r)
  {
    for (size_t k = 0; k + 1 < stands[r].size(); ++k)
    {
      const Stand &left = stands[r][k];
      const Stand &right = stands[r][k + 1];

      if (!left.planned || !right.planned)
      {
        continue;
      }

      const double gapU0 = r == 0 ? left.u1 : right.u1;
      const double gapU1 = r == 0 ? right.u0 : left.u0;

      if (gapU1 - gapU0 < strip * 2. + .2)
      {
        continue;
      }

      const uint32_t top = std::max(left.storeys, right.storeys);

      if (top < 2)
      {
        continue;
      }

      // The gap's own ground: a deck, level with the alley and touching it.
      const double deep = std::min(std::abs(left.v1 - left.v0), std::abs(right.v1 - right.v0));
      const double into = r == 0 ? 1. : -1.; // Which way v grows into the block.

      {
        NeighbourhoodDeck ground;
        ground.kind = DeckKind::Alley;
        ground.row = r;
        const Position groundMiddle = frame.at((gapU0 + gapU1) * .5, r == 0 ? 0 : -out.alleyMetres, 0);
        ground.level = terrain(groundMiddle.x, groundMiddle.z);
        const double vNear = r == 0 ? 0 : -out.alleyMetres;
        const double vFar = vNear + into * deep;
        ground.footprint = gradedQuad(frame, gapU0, std::min(vNear, vFar), gapU1, std::max(vNear, vFar), 0, terrain);
        out.decks.push_back(std::move(ground));
      }

      NeighbourhoodStair stair;
      stair.row = r;
      stair.afterPlot = uint32_t(r == 0 ? k : k + 1);
      stair.u0 = gapU0;
      stair.u1 = gapU1;

      const double vNear = r == 0 ? 0 : -out.alleyMetres;
      const double climb = storey * .5;
      const double available = deep - strip - .4;

      for (uint32_t s = 0; s + 1 < top; ++s)
      {
        const Position stairGround = frame.at((gapU0 + gapU1) * .5, vNear, 0);
        const double base = terrain(stairGround.x, stairGround.z) + double(s) * storey;

        // Up the first strip, away from the alley.
        const StairSteps steps = stepsFor(climb, settings.comfortableRiserMetres, available);
        const double run = std::min(available, steps.going);

        const auto flight = [&](const double uStrip, const double vFrom, const double dir, const double y0, const bool up) {
          rewrite::DwellingStair f;
          const double vTo = vFrom + dir * run;
          const double left0 = uStrip, right0 = uStrip + strip;
          f.opening = {frame.at(left0, vFrom, y0), frame.at(right0, vFrom, y0), frame.at(right0, vTo, y0),
                       frame.at(left0, vTo, y0)};
          f.lowerLanding = {};
          f.upperLanding = {};
          f.lowerFloor = 0;
          f.risers = steps.risers;
          f.tread = steps.tread;
          f.rise = climb;
          f.startRise = y0;
          f.width = strip;
          f.privateAccess = false;
          (void)up;
          return f;
        };

        const double vFar = vNear + into * (run + .2);

        stair.flights.push_back(flight(gapU0 + .1, vNear + into * .2, into, base, true));

        // The half landing at the far end, a deck of its own.
        {
          NeighbourhoodDeck landing;
          landing.kind = DeckKind::Landing;
          landing.row = r;
          landing.level = base + climb;
          landing.footprint = quad(frame, gapU0 + .1, std::min(vFar, vFar + into * strip), gapU0 + .1 + strip * 2. + .1,
                                   std::max(vFar, vFar + into * strip), landing.level);
          out.decks.push_back(std::move(landing));
        }

        // Back down the second strip toward the alley, arriving at the gallery.
        stair.flights.push_back(flight(gapU0 + .1 + strip + .1, vFar, -into, base + climb, true));

        const uint32_t from = s == 0 ? uint32_t(out.decks.size() - 2) : galleryAt[{r, s}];
        const auto found = galleryAt.find({r, s + 1});

        if (found != galleryAt.end())
        {
          stair.joins.emplace_back(from, found->second);
        }
      }

      out.stairs.push_back(std::move(stair));
    }
  }

  // ---- the units and their doors ---------------------------------------------
  for (uint32_t r = 0; r < 2; ++r)
  {
    for (size_t k = 0; k < stands[r].size(); ++k)
    {
      const Stand &stand = stands[r][k];

      if (!stand.planned)
      {
        continue;
      }

      const CellPlot &plot = out.rows[r].plots[k];

      for (uint32_t s = 0; s < stand.storeys; ++s)
      {
        CellUnit unit;
        unit.row = r;
        unit.plot = uint32_t(k);
        unit.storey = s;

        if (s == 0)
        {
          unit.deck = 0;
          double mx = 0, mz = 0;
          geom2::edgePoint(plot.house.grid, plot.house.frame, plot.house.entrance, .5, mx, mz);
          unit.door = Position{mx, plot.foundation, mz};
        }
        else
        {
          const auto found = galleryAt.find({r, s});

          if (found == galleryAt.end())
          {
            continue;
          }

          unit.deck = found->second;

          // On the storey's own alley-facing wall, which is not always the
          // plot's alley face: a storey set back by a cell has its wall a cell
          // in, and a door placed on the plot line would be cut into air. The
          // exterior edge of that storey nearest the middle of the plot's
          // alley face is where the door goes.
          const CellFloor &floor = plot.house.floors[s];
          const Position want = frame.at((stand.u0 + stand.u1) * .5, stand.v0, plot.foundation + double(s) * storey);
          double best = 1e300;
          unit.door = want;

          for (const Chain &loop : floor.loops)
          {
            for (const uint32_t e : loop)
            {
              if (!exteriorEdge(floor, e))
              {
                continue;
              }

              double mx = 0, mz = 0;
              geom2::edgePoint(plot.house.grid, plot.house.frame, e, .5, mx, mz);
              const double reach = std::hypot(mx - want.x, mz - want.z);

              if (reach < best)
              {
                best = reach;
                unit.door = Position{mx, plot.foundation + double(s) * storey, mz};
              }
            }
          }
        }

        out.units.push_back(unit);
      }
    }
  }

  // ---- the walk ------------------------------------------------------------
  //
  // Seeded from the alley, and the graph proves the rest. Seeding from every
  // ground deck at once is how a block cut cleanly in two reported every door
  // reachable.
  {
    const uint32_t n = uint32_t(out.decks.size());
    std::vector<std::vector<uint32_t>> links(n);

    for (uint32_t p = 0; p < n; ++p)
    {
      for (uint32_t q = p + 1; q < n; ++q)
      {
        if (touching(frame, out.decks[p], out.decks[q]))
        {
          links[p].push_back(q);
          links[q].push_back(p);
        }
      }
    }

    for (const NeighbourhoodStair &stair : out.stairs)
    {
      for (const auto &[from, to] : stair.joins)
      {
        links[from].push_back(to);
        links[to].push_back(from);
      }
    }

    std::vector<uint32_t> component(n, UINT32_MAX);
    uint32_t islands = 0;

    for (uint32_t root = 0; root < n; ++root)
    {
      if (component[root] != UINT32_MAX)
      {
        continue;
      }

      std::vector<uint32_t> stack{root};
      component[root] = islands;

      while (!stack.empty())
      {
        const uint32_t at = stack.back();
        stack.pop_back();

        for (const uint32_t next : links[at])
        {
          if (component[next] == UINT32_MAX)
          {
            component[next] = islands;
            stack.push_back(next);
          }
        }
      }

      ++islands;
    }

    out.islands = islands;
    out.reachedUnits = 0;

    for (const CellUnit &unit : out.units)
    {
      out.reachedUnits += unit.deck < n && component[unit.deck] == component[0] ? 1u : 0u;
    }

    out.solved = !out.units.empty() && out.reachedUnits == out.units.size();
  }

  if (!out.solved)
  {
    ledger.add(LedgerKind::Rejected, PatchId(brief.id),
               "neighbourhood: " + std::to_string(out.units.size() - out.reachedUnits) + " of " +
                 std::to_string(out.units.size()) + " doors cannot be reached from the alley");
  }

  return true;
}

bool realiseCellNeighbourhood(const CellNeighbourhood &hood,
                              const CellDwellingSettings &settings,
                              std::vector<rewrite::DwellingPlan> &plans,
                              std::string *error)
{
  plans.clear();

  std::vector<rewrite::DwellingPlan> rowPlans[2];

  for (uint32_t r = 0; r < 2; ++r)
  {
    if (!realiseCellStreet(hood.rows[r], settings, rowPlans[r], error))
    {
      return false;
    }

    for (size_t k = 0; k < rowPlans[r].size() && k < hood.rows[r].plots.size(); ++k)
    {
      liftPlan(rowPlans[r][k], hood.rows[r].plots[k].foundation);
    }
  }

  const double dx = geom2::toMetres(hood.b.x - hood.a.x);
  const double dz = geom2::toMetres(hood.b.y - hood.a.y);
  const double length = std::sqrt(dx * dx + dz * dz);

  Frame frame;
  frame.ax = geom2::toMetres(hood.a.x);
  frame.az = geom2::toMetres(hood.a.y);
  frame.ux = dx / std::max(1e-9, length);
  frame.uz = dz / std::max(1e-9, length);
  const double storey = hood.rows[0].plots.empty() ? 3. : hood.rows[0].plots.front().house.gridClass.storeyMetres;

  // The upper doors, cut where the unit said its door is: the wall of that
  // storey whose run holds the door's middle.
  for (const CellUnit &unit : hood.units)
  {
    if (unit.storey == 0 || unit.plot >= rowPlans[unit.row].size())
    {
      continue;
    }

    rewrite::DwellingPlan &plan = rowPlans[unit.row][unit.plot];
    const CellHouse &house = hood.rows[unit.row].plots[unit.plot].house;
    const uint32_t floorIndex = unit.storey + (house.basement.has_value() ? 1u : 0u);

    for (const rewrite::DwellingWall &wall : plan.walls)
    {
      if (wall.floor != floorIndex || !wall.exterior || wall.kind != rewrite::WallKind::Solid)
      {
        continue;
      }

      const double wx = wall.b.x - wall.a.x, wz = wall.b.z - wall.a.z;
      const double len2 = wx * wx + wz * wz;

      if (len2 < 1e-9)
      {
        continue;
      }

      const double t = ((unit.door.x - wall.a.x) * wx + (unit.door.z - wall.a.z) * wz) / len2;
      const double px = wall.a.x + wx * t, pz = wall.a.z + wz * t;

      if (t < 0 || t > 1 || std::hypot(px - unit.door.x, pz - unit.door.z) > .05)
      {
        continue;
      }

      const double width = std::min(settings.doorWidthMetres, std::sqrt(len2) * .8);
      const double half = width * .5 / std::sqrt(len2);

      rewrite::DwellingOpening cut;
      cut.wall = wall.id;
      cut.room = 0;
      cut.floor = floorIndex;
      cut.a = Position{wall.a.x + wx * (t - half), wall.a.y, wall.a.z + wz * (t - half)};
      cut.b = Position{wall.a.x + wx * (t + half), wall.a.y, wall.a.z + wz * (t + half)};
      cut.base = 0;
      cut.top = 2.1;
      cut.sill = 0;
      cut.head = 2.1;
      cut.thickness = wall.thickness;
      cut.kind = rewrite::OpeningKind::Door;
      cut.exterior = true;
      plan.openings.push_back(cut);

      rewrite::DwellingDoor leaf;
      leaf.wall = wall.id;
      leaf.floor = floorIndex;
      leaf.center = Position{px, wall.a.y, pz};
      leaf.width = width;
      leaf.privateAccess = true;
      plan.doors.push_back(leaf);
      break;
    }
  }

  // The galleries and the landings: decking, posts to the ground, and a rail
  // where there is a drop and nothing across it. Given to the first plan of the
  // row that reaches that storey, which is a bookkeeping choice and not a
  // claim of ownership: the deck is the row's.
  // The rigid terraces that keep buildings level while the ground falls.
  for (const NeighbourhoodPlinth &plinth : hood.plinths)
  {
    if (plinth.row >= 2 || plinth.plot >= rowPlans[plinth.row].size() || plinth.footprint.size() < 3) continue;
    rewrite::DwellingPillar mass;
    mass.footprint = plinth.footprint;
    for (Position &p : mass.footprint) p.y = plinth.base;
    mass.base = plinth.base;
    mass.top = plinth.top;
    mass.masonry = true;
    rowPlans[plinth.row][plinth.plot].pillars.push_back(std::move(mass));
  }

  for (const NeighbourhoodDeck &deck : hood.decks)
  {
    if (deck.kind == DeckKind::Alley || deck.footprint.size() < 4)
    {
      continue;
    }

    rewrite::DwellingPlan *plan = nullptr;
    uint32_t floorIndex = 0;

    for (size_t k = 0; k < rowPlans[deck.row].size() && plan == nullptr; ++k)
    {
      const CellHouse &house = hood.rows[deck.row].plots[k].house;

      if (hood.rows[deck.row].plots[k].planned && house.floors.size() > deck.storey)
      {
        plan = &rowPlans[deck.row][k];
        floorIndex = deck.storey + (house.basement.has_value() ? 1u : 0u);
      }
    }

    if (plan == nullptr)
    {
      continue;
    }

    const std::vector<Position> &f = deck.footprint;
    const double board = settings.floorBoardMetres;

    if (floorIndex < plan->floors.size())
    {
      plan->floors[floorIndex].slabs.push_back({Position{f[0].x, f[0].y + board, f[0].z}, Position{f[1].x, f[1].y + board, f[1].z}, Position{f[2].x, f[2].y + board, f[2].z}});
      plan->floors[floorIndex].slabs.push_back({Position{f[0].x, f[0].y + board, f[0].z}, Position{f[2].x, f[2].y + board, f[2].z}, Position{f[3].x, f[3].y + board, f[3].z}});
    }

    if (deck.kind != DeckKind::Gallery)
    {
      continue;
    }

    // Posts under the outer edge, and a rail along it unless the facing row's
    // gallery meets it there. The edge walked rather than fenced by rule:
    // "every side fenced" put a rail across the front doors.
    bool met = false;

    for (const NeighbourhoodDeck &other : hood.decks)
    {
      met = met || (&other != &deck && other.kind == DeckKind::Gallery && other.row != deck.row &&
                    std::abs(other.level - deck.level) < 1e-6);
    }

    // The outer edge is the one at the alley's middle: for row 0 the low-v edge
    // (f[0]-f[1]), for row 1 the high-v edge (f[2]-f[3]).
    const Position &e0 = deck.row == 0 ? f[0] : f[3];
    const Position &e1 = deck.row == 0 ? f[1] : f[2];
    const double ex = e1.x - e0.x, ez = e1.z - e0.z;
    const double span = std::hypot(ex, ez);
    const int32_t bays = std::max(1, int32_t(std::lround(span / std::max(settings.balconyPostSpacingMetres, .6))));
    const double half = settings.balconyPostSideMetres * .5;

    for (int32_t b = 0; b <= bays; ++b)
    {
      const double t = double(b) / double(bays);
      const double px = e0.x + ex * t, pz = e0.z + ez * t;

      rewrite::DwellingPillar post;
      post.top = deck.levelAt(px, pz);
      post.base = post.top - double(deck.storey) * storey;
      post.footprint = {Position{px - half, post.base, pz - half}, Position{px + half, post.base, pz - half},
                        Position{px + half, post.base, pz + half}, Position{px - half, post.base, pz + half}};
      plan->pillars.push_back(std::move(post));
    }

    if (!met)
    {
      rewrite::DwellingWall rail;
      rail.id = plan->walls.size() + 1;
      rail.floor = floorIndex;
      rail.a = Position{e0.x, e0.y + board, e0.z};
      rail.b = Position{e1.x, e1.y + board, e1.z};
      rail.thickness = settings.balconyPostSideMetres;
      rail.exterior = true;
      rail.kind = rewrite::WallKind::Open;
      rail.height = settings.balconyRailHeightMetres;
      plan->walls.push_back(std::move(rail));
    }
  }

  // The stairs, into the plan of the plot before their gap.
  for (const NeighbourhoodStair &stair : hood.stairs)
  {
    const uint32_t k = stair.afterPlot == UINT32_MAX ? 0u : std::min<uint32_t>(stair.afterPlot, uint32_t(rowPlans[stair.row].size()) - 1u);

    if (k >= rowPlans[stair.row].size())
    {
      continue;
    }

    for (const rewrite::DwellingStair &flight : stair.flights)
    {
      rowPlans[stair.row][k].stairs.push_back(flight);
    }
  }

  for (uint32_t r = 0; r < 2; ++r)
  {
    for (rewrite::DwellingPlan &plan : rowPlans[r])
    {
      plans.push_back(std::move(plan));
    }
  }

  (void)frame;

  return true;
}

} // namespace procedural::world::generator
