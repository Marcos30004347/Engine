#include "procedural/world/generator/CellCompound.hpp"

#include "procedural/world/generator/CellDwellingClasses.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

using geom2::Point;
using geom2::Polygon;
using geom2::Ring;

namespace {

bool fail(std::string *error, const std::string &why)
{
  if (error != nullptr)
  {
    *error = why;
  }

  return false;
}

// The compound's own frame: `u` runs along the front and `v` runs into the
// precinct. Every measurement below is taken in it, and the world only appears
// again when a ring is built.
struct Frame
{
  double ux = 1;
  double uy = 0;
  double u0 = 0;
  double v0 = 0;
  double u1 = 0;
  double v1 = 0;

  double width() const
  {
    return u1 - u0;
  }

  double depth() const
  {
    return v1 - v0;
  }

  Point at(const double u, const double v) const
  {
    return Point{geom2::toUnit(u * ux - v * uy), geom2::toUnit(u * uy + v * ux)};
  }
};

// A rectangle of the frame, counter-clockwise, with its edges in a known order:
// 0 is the front (low v), 1 the far side (high u), 2 the back (high v), 3 the
// near side (low u). Every `frontage` index below is one of those four.
Polygon rectangle(const Frame &frame, const double u0, const double v0, const double u1, const double v1)
{
  Polygon out;
  out.outer = {frame.at(u0, v0), frame.at(u1, v0), frame.at(u1, v1), frame.at(u0, v1)};

  if (!geom2::counterClockwise(out.outer))
  {
    geom2::reverse(out.outer);
  }

  return out;
}

// A regular polygon, for a tower. Six sides reads as round at this scale and is
// what the turret already uses, so a compound's towers and a house's turret are
// the same shape at different sizes.
Polygon drum(const Frame &frame, const double u, const double v, const double radius, const uint32_t sides)
{
  Polygon out;

  for (uint32_t k = 0; k < sides; ++k)
  {
    const double angle = 6.283185307179586 * double(k) / double(sides);
    out.outer.push_back(frame.at(u + radius * std::cos(angle), v + radius * std::sin(angle)));
  }

  if (!geom2::counterClockwise(out.outer))
  {
    geom2::reverse(out.outer);
  }

  return out;
}

// The board a wing is measured on.
//
// Settled here rather than by the wing's class, because the wing's size is a
// property of the wall it leans on. The band is generous downward and tight
// upward: a range should fill its lot, and the reason it may fall short is the
// setback, not a size the planner drew for itself.
GridClass boardFor(const BuildingClass building,
                   const FootprintKind footprint,
                   const double lotWidth,
                   const double lotDepth,
                   const double cellMetres)
{
  // The class's own board, with the measurements the wall dictates written over
  // it. The storey height stays the class's: a church nave is six and a half
  // metres tall because it is a nave, and that has nothing to do with how deep
  // the range it stands in happens to be.
  GridClass grid = profileFor(building).grid;

  grid.footprint = footprint;
  grid.cellMetres = cellMetres;

  // The band is bracketed rather than estimated, and the reason is alignment.
  // A cell counts as ground only when the whole of it is inside the setback, so
  // a fifteen metre lot on a three metre grid holds nine cells and not
  // twenty-five - the grid does not begin where the lot does, and a lot loses
  // the best part of a cell at each end. `most` is what could fit if it aligned
  // perfectly and `least` is what fits however it falls; the truth is between,
  // and a band is exactly the right shape for that answer.
  const auto most = [&](const double metres) {
    return std::max(1., std::floor(std::max(0., metres) / cellMetres));
  };

  const auto least = [&](const double metres) {
    return std::max(1., std::floor(std::max(0., metres - 1.) / cellMetres) - 1.);
  };

  // And how much of that ground the shape actually covers. A range fills its
  // lot; a cross covers about a fifth of the box it is drawn in, and holding it
  // to a rectangle's floor area is how a church comes out a box - the cross is
  // drawn, found to be under the minimum, and rejected sixty-four times before
  // the planner gives up and reports that nothing fits.
  const double fill = footprint == FootprintKind::Box     ? .8
                      : footprint == FootprintKind::Axial ? .22
                                                          : .4;

  grid.maximumCells = std::max(4u, uint32_t(most(lotWidth) * most(lotDepth)));
  grid.minimumCells =
    std::min(grid.maximumCells, std::max(4u, uint32_t(least(lotWidth) * least(lotDepth) * fill)));

  // The tier comes from how LONG the lot is, not from how many cells it holds.
  // A range is forty metres of wall and nine metres deep: sixty cells, which
  // fits a sixteen-row board comfortably and does not fit across one at all.
  const uint32_t span = uint32_t(std::ceil(std::max(lotWidth, lotDepth) / cellMetres)) + 2u;

  grid.rows = span > 32 ? 64u : (span > 16 ? 32u : 16u);

  return grid;
}

// One wing, laid on a rectangle of the frame and planned by the house planner.
void raise(CellCompound &out,
           const Frame &frame,
           const std::string &role,
           const BuildingClass building,
           const double u0,
           const double v0,
           const double u1,
           const double v1,
           const uint32_t frontage,
           const FootprintKind footprint,
           const std::vector<std::string> &tags,
           const double cellMetres,
           const uint64_t seed,
           const CellDwellingSettings &settings,
           Ledger &ledger)
{
  CellWing wing;
  wing.role = role;
  wing.building = building;
  wing.lot = rectangle(frame, u0, v0, u1, v1);
  wing.frontage = {frontage};

  CellDwellingBrief brief;
  brief.id = out.id;
  brief.seed = seed;
  brief.building = building;
  brief.lot = wing.lot;
  brief.frontage = wing.frontage;
  brief.setbackMetres = 0;
  brief.standing = PlotStanding::Modest;
  brief.grid = boardFor(building, footprint, u1 - u0, v1 - v0, cellMetres);
  brief.tags = tags;

  std::string why;
  wing.planned = planCellDwelling(brief, settings, wing.house, ledger, &why);

  if (!wing.planned)
  {
    wing.note = why;
    ledger.add(LedgerKind::Rejected, PatchId(out.id), "compound wing '" + role + "' was not planned: " + why);
  }

  out.wings.push_back(std::move(wing));
}

// The four sides of an enclosure, with a gate cut into the front.
void enclose(CellCompound &out, const Frame &frame, const double inset, const double gateWidth, const double height)
{
  const double u0 = frame.u0 + inset;
  const double v0 = frame.v0 + inset;
  const double u1 = frame.u1 - inset;
  const double v1 = frame.v1 - inset;

  out.enclosure = rectangle(frame, u0, v0, u1, v1);

  const double middle = (u0 + u1) * .5;
  const double half = std::min(gateWidth, (u1 - u0) * .4) * .5;

  const auto run = [&](const double au, const double av, const double bu, const double bv, const bool gate) {
    CellCurtain wall;
    wall.a = frame.at(au, av);
    wall.b = frame.at(bu, bv);
    wall.heightMetres = height;
    wall.gate = gate;
    out.walls.push_back(wall);
  };

  // The front, in three: wall, gate, wall.
  run(u0, v0, middle - half, v0, false);
  run(middle - half, v0, middle + half, v0, true);
  run(middle + half, v0, u1, v0, false);

  run(u1, v0, u1, v1, false);
  run(u1, v1, u0, v1, false);
  run(u0, v1, u0, v0, false);

  out.gate = frame.at(middle, v0);
}

} // namespace

const char *name(const CellCompoundKind kind)
{
  switch (kind)
  {
    case CellCompoundKind::Castle:
      return "castle";
    case CellCompoundKind::Manor:
      return "manor";
    case CellCompoundKind::Church:
      return "church";
    case CellCompoundKind::Hall:
      return "hall";
  }

  return "?";
}

bool planCellCompound(const CellCompoundBrief &brief,
                      const CellDwellingSettings &settings,
                      CellCompound &out,
                      Ledger &ledger,
                      std::string *error)
{
  out = CellCompound();
  out.id = brief.id;
  out.kind = brief.kind;
  out.precinct = brief.precinct;

  if (brief.precinct.outer.size() < 3)
  {
    return fail(error, "a compound needs a precinct");
  }

  // The front is the precinct edge the approach is nearest, and the axis runs
  // along it. A compound turned to its bounding box rather than to its approach
  // puts the gate round the back of the hill.
  const Ring &ring = brief.precinct.outer;

  double bestDistance = 1e300;
  size_t front = 0;

  for (size_t k = 0; k < ring.size(); ++k)
  {
    const Point &a = ring[k];
    const Point &b = ring[(k + 1) % ring.size()];

    const double ax = geom2::toMetres(a.x);
    const double ay = geom2::toMetres(a.y);
    const double bx = geom2::toMetres(b.x);
    const double by = geom2::toMetres(b.y);
    const double px = geom2::toMetres(brief.approach.x);
    const double py = geom2::toMetres(brief.approach.y);

    const double ex = bx - ax;
    const double ey = by - ay;
    const double length = ex * ex + ey * ey;
    const double t = length > 1e-9 ? std::clamp(((px - ax) * ex + (py - ay) * ey) / length, 0., 1.) : 0.;
    const double dx = ax + ex * t - px;
    const double dy = ay + ey * t - py;
    const double distance = dx * dx + dy * dy;

    if (distance < bestDistance)
    {
      bestDistance = distance;
      front = k;
    }
  }

  Frame frame;

  {
    const Point &a = ring[front];
    const Point &b = ring[(front + 1) % ring.size()];
    const double ex = geom2::toMetres(b.x) - geom2::toMetres(a.x);
    const double ey = geom2::toMetres(b.y) - geom2::toMetres(a.y);
    const double length = std::sqrt(ex * ex + ey * ey);

    if (length < 1e-6)
    {
      return fail(error, "the precinct has a degenerate front edge");
    }

    frame.ux = ex / length;
    frame.uy = ey / length;
  }

  // The ground the compound is actually laid on. A rectangle inside the
  // precinct rather than the precinct itself, because everything below is a
  // range along a wall and a range with a shaved corner is not a range.
  if (!geom2::largestInscribedRectangle(brief.precinct, frame.ux, frame.uy, .5, frame.u0, frame.v0, frame.u1, frame.v1))
  {
    return fail(error, "no usable rectangle inside the precinct");
  }

  // The precinct is counter-clockwise and the front edge runs with it, so the
  // interior is on the left of the axis, which is +v. Nothing to flip.
  const double width = frame.width();
  const double depth = frame.depth();

  const double smallest = brief.kind == CellCompoundKind::Castle   ? 34.
                          : brief.kind == CellCompoundKind::Manor  ? 24.
                          : brief.kind == CellCompoundKind::Church ? 22.
                                                                   : 18.;

  if (width < smallest || depth < smallest * .7)
  {
    ledger.add(LedgerKind::Rejected, PatchId(brief.id),
               std::string("a ") + name(brief.kind) + " precinct was too small to lay out");

    return fail(error, "the precinct is too small for this compound");
  }

  Stream stream(brief.seed);

  switch (brief.kind)
  {
    case CellCompoundKind::Castle:
    {
      const double wallHeight = 7.5 + 2.5 * stream.unit();
      const double inset = 1.;

      enclose(out, frame, inset, 7., wallHeight);

      const double u0 = frame.u0 + inset + .8;
      const double v0 = frame.v0 + inset + .8;
      const double u1 = frame.u1 - inset - .8;
      const double v1 = frame.v1 - inset - .8;

      const double radius = std::min(4.2, std::min(u1 - u0, v1 - v0) * .1);

      for (const auto &[tu, tv] : std::vector<std::pair<double, double>>{
             {frame.u0 + inset, frame.v0 + inset},
             {frame.u1 - inset, frame.v0 + inset},
             {frame.u1 - inset, frame.v1 - inset},
             {frame.u0 + inset, frame.v1 - inset}})
      {
        out.towers.push_back(drum(frame, tu, tv, radius, 6));
      }

      // The gatehouse straddles the gate; the ranges lean on the back wall and
      // one side; the keep stands free in the ward, which is what a keep is.
      const double gateDepth = std::min(8., depth * .16);
      const double backDepth = std::min(9.5, depth * .24);
      const double sideDepth = std::min(8.5, width * .2);
      const double middle = (u0 + u1) * .5;

      raise(out, frame, "gatehouse", BuildingClass::Prison, middle - 5., v0, middle + 5., v0 + gateDepth, 2,
            FootprintKind::Box, {"fortified", "square"}, 3., brief.seed * 31u + 1u, settings, ledger);

      raise(out, frame, "great hall", BuildingClass::GuildHall, u0, v1 - backDepth, u1, v1, 0,
            FootprintKind::Box, {"fortified", "square"}, 3., brief.seed * 31u + 2u, settings, ledger);

      raise(out, frame, "service range", BuildingClass::Warehouse, u0, v0 + gateDepth + 3., u0 + sideDepth,
            v1 - backDepth - 2., 1, FootprintKind::Box, {"fortified", "square"}, 3., brief.seed * 31u + 3u, settings, ledger);

      const double keepSide = std::min(15., std::min((u1 - u0 - sideDepth) * .4, (v1 - v0 - backDepth - gateDepth) * .5));
      const double keepU = u1 - 4. - keepSide;
      const double keepV = v1 - backDepth - 4. - keepSide;

      const bool keepFits = keepSide >= 9. && keepU > u0 + sideDepth + 3. && keepV > v0 + gateDepth + 3.;

      if (keepFits)
      {
        raise(out, frame, "keep", BuildingClass::ManorHouse, keepU, keepV, keepU + keepSide, keepV + keepSide, 0,
              FootprintKind::Box, {"fortified", "square", "tall"}, 3., brief.seed * 31u + 4u, settings, ledger);
      }

      // The ward: everything the ranges and the gatehouse left, with the keep as
      // a hole in it. It starts past the gatehouse rather than at the gate,
      // because the strip the gatehouse stands on is not open ground.
      Polygon ward = rectangle(frame, u0 + sideDepth, v0 + gateDepth, u1, v1 - backDepth);

      if (keepFits)
      {
        Ring hole = rectangle(frame, keepU, keepV, keepU + keepSide, keepV + keepSide).outer;

        if (geom2::counterClockwise(hole))
        {
          geom2::reverse(hole);
        }

        ward.holes.push_back(std::move(hole));
      }

      out.courts.push_back(std::move(ward));

      break;
    }

    case CellCompoundKind::Manor:
    {
      const double inset = .8;

      enclose(out, frame, inset, 5., 2.6);

      const double u0 = frame.u0 + inset + .6;
      const double v0 = frame.v0 + inset + .6;
      const double u1 = frame.u1 - inset - .6;
      const double v1 = frame.v1 - inset - .6;

      const double hallDepth = std::min(11., depth * .34);
      const double serviceWidth = std::min(8., width * .28);

      raise(out, frame, "hall", BuildingClass::ManorHouse, u0, v1 - hallDepth, u1, v1, 0, FootprintKind::Box, {"square"}, 2.6,
            brief.seed * 31u + 1u, settings, ledger);

      const bool serviceFits = v1 - hallDepth - 3. > v0 + 4. && serviceWidth >= 5.;

      if (serviceFits)
      {
        raise(out, frame, "service wing", BuildingClass::Farmstead, u0, v0 + 3., u0 + serviceWidth,
              v1 - hallDepth - 2., 1, FootprintKind::Box, {"square"}, 2.6, brief.seed * 31u + 2u, settings, ledger);
      }

      out.courts.push_back(rectangle(frame, serviceFits ? u0 + serviceWidth : u0, v0, u1, v1 - hallDepth));

      break;
    }

    case CellCompoundKind::Church:
    {
      const double inset = .6;

      enclose(out, frame, inset, 4., 1.8);

      const double u0 = frame.u0 + inset + 2.;
      const double v0 = frame.v0 + inset + 3.;
      const double u1 = frame.u1 - inset - 2.;
      const double v1 = frame.v1 - inset - 3.;

      // One building, and its shape is its own: the axial archetype makes the
      // cross. The churchyard is what surrounds it, and the church is a hole in
      // that ground rather than a building standing on it.
      raise(out, frame, "church", BuildingClass::Church, u0, v0, u1, v1, 0, FootprintKind::Axial, {}, 3.,
            brief.seed * 31u + 1u, settings, ledger);

      out.towers.push_back(drum(frame, (u0 + u1) * .5, v0 - 2., 3., 6));

      Polygon yard = rectangle(frame, frame.u0 + inset, frame.v0 + inset, frame.u1 - inset, frame.v1 - inset);
      Ring hole = rectangle(frame, u0, v0, u1, v1).outer;

      if (geom2::counterClockwise(hole))
      {
        geom2::reverse(hole);
      }

      yard.holes.push_back(std::move(hole));
      out.courts.push_back(std::move(yard));

      break;
    }

    case CellCompoundKind::Hall:
    {
      const double inset = .8;

      enclose(out, frame, inset, 6., 2.4);

      const double u0 = frame.u0 + inset + .6;
      const double v0 = frame.v0 + inset + .6;
      const double u1 = frame.u1 - inset - .6;
      const double v1 = frame.v1 - inset - .6;

      const double hallDepth = std::min(14., depth * .5);

      raise(out, frame, "hall", BuildingClass::GuildHall, u0, v1 - hallDepth, u1, v1, 0, FootprintKind::Box, {"square"}, 2.8,
            brief.seed * 31u + 1u, settings, ledger);

      out.courts.push_back(rectangle(frame, u0, v0, u1, v1 - hallDepth));

      break;
    }
  }

  out.solved = out.plannedWings() > 0;

  if (!out.solved)
  {
    return fail(error, "no wing of the compound could be planned");
  }

  return true;
}

} // namespace procedural::world::generator
