#include "procedural/world/generator/CellStreet.hpp"

#include "procedural/world/generator/CellDwellingClasses.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

using geom2::Point;
using geom2::Polygon;

namespace {

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

// The street's frame: `u` along the frontage from `a`, `v` into the block. The
// block is to the LEFT of a->b, which is where it is for someone standing in
// the road looking at the houses with the frontage read left to right.
struct Frame
{
  double ax = 0, az = 0;
  double ux = 1, uz = 0;

  Point at(const double u, const double v) const
  {
    // Left perpendicular of (ux, uz) in the plan's own reading.
    const double vx = -uz, vz = ux;

    return Point{geom2::toUnit(ax + u * ux + v * vx), geom2::toUnit(az + u * uz + v * vz)};
  }
};

Polygon rectangle(const Frame &frame, const double u0, const double v0, const double u1, const double v1)
{
  Polygon out;
  out.outer = {frame.at(u0, v0), frame.at(u1, v0), frame.at(u1, v1), frame.at(u0, v1)};

  // Edge 0 must stay the street edge whichever way the ring winds, so a ring
  // that has to be reversed is rotated back to start at the same corner.
  if (!geom2::counterClockwise(out.outer))
  {
    out.outer = {frame.at(u0, v0), frame.at(u0, v1), frame.at(u1, v1), frame.at(u1, v0)};
  }

  return out;
}

} // namespace

bool planCellStreet(const CellStreetBrief &brief,
                    const CellDwellingSettings &settings,
                    CellStreet &out,
                    Ledger &ledger,
                    std::string *error)
{
  out = CellStreet();
  out.id = brief.id;
  out.a = brief.a;
  out.b = brief.b;

  const ClassProfile &profile = profileFor(brief.building);
  const double cell = profile.grid.cellMetres;
  out.cellMetres = cell;

  const double dx = geom2::toMetres(brief.b.x - brief.a.x);
  const double dz = geom2::toMetres(brief.b.y - brief.a.y);
  const double length = std::sqrt(dx * dx + dz * dz);

  if (length < cell * 2. || brief.depthMetres < cell * 2.)
  {
    return fail(error, "a street needs at least two cells of frontage and of depth");
  }

  Frame frame;
  frame.ax = geom2::toMetres(brief.a.x);
  frame.az = geom2::toMetres(brief.a.y);
  frame.ux = dx / length;
  frame.uz = dz / length;

  out.cellsAlong = uint32_t(std::floor(length / cell));
  const uint32_t deepest = uint32_t(std::floor(brief.depthMetres / cell));

  Stream stream(brief.seed);

  const uint32_t narrowest = std::max(2u, brief.narrowestCells);
  const uint32_t widest = std::max(narrowest, brief.widestCells);

  // Whether edge 0 of the rectangle as built is the street edge: the ring is
  // wound counter-clockwise, and which way `rectangle` had to wind it decides
  // which corner edge 1 leaves from. Measured once on a probe rectangle.
  uint32_t farSide = 1;
  uint32_t nearSide = 3;

  {
    const Polygon probe = rectangle(frame, 0, 0, cell, cell);
    const Point far = frame.at(cell, cell * .5);
    const Point near = frame.at(0, cell * .5);

    for (uint32_t k = 0; k < 4; ++k)
    {
      const Point &p = probe.outer[k];
      const Point &q = probe.outer[(k + 1) % 4];
      const double mx = (double(p.x) + double(q.x)) * .5;
      const double my = (double(p.y) + double(q.y)) * .5;

      if (std::abs(mx - double(far.x)) < 1 && std::abs(my - double(far.y)) < 1)
      {
        farSide = k;
      }

      if (std::abs(mx - double(near.x)) < 1 && std::abs(my - double(near.y)) < 1)
      {
        nearSide = k;
      }
    }
  }

  // The frontage cut into plots. Whole cells, drawn between the narrowest and
  // the widest, and the last plot takes what is left rather than leaving a
  // sliver: a sliver is not a plot.
  uint32_t at = 0;
  const uint32_t gap = brief.gapCells;

  while (at < out.cellsAlong)
  {
    uint32_t width = narrowest + uint32_t(stream.next() % uint64_t(widest - narrowest + 1));

    // The last plot takes what is left, or nothing: in a terrace a sliver is
    // not a plot, and in a row of stacks the gap after the last one is ground.
    if (at + width > out.cellsAlong || out.cellsAlong - (at + width) < narrowest + gap)
    {
      width = out.cellsAlong - at;
    }

    if (width < narrowest)
    {
      break;
    }

    // Deep or shallow: a plot takes between six tenths of the block and all of
    // it, in whole cells, and never fewer than three.
    const uint32_t depth =
      std::clamp(uint32_t(std::lround(double(deepest) * (.6 + .4 * stream.unit()))), std::min(3u, deepest), deepest);

    CellPlot plot;
    plot.fromCell = at;
    plot.widthCells = width;
    plot.depthCells = depth;
    plot.lot = rectangle(frame, double(at) * cell, 0, double(at + width) * cell, double(depth) * cell);

    out.plots.push_back(std::move(plot));
    at += width + gap;
  }

  if (out.plots.empty())
  {
    return fail(error, "no plot fitted on the frontage");
  }

  // Every plot planned. The house is the plot: a slum's standing, no setback,
  // a box footprint, and a board sized to exactly the plot's cells.
  for (size_t k = 0; k < out.plots.size(); ++k)
  {
    CellPlot &plot = out.plots[k];

    CellDwellingBrief house;
    house.id = brief.id * 1000 + k;
    house.seed = stream.next();
    house.building = brief.building;
    house.lot = plot.lot;
    house.frontage = {0};
    house.setbackMetres = 0;
    house.standing = PlotStanding::Slum;
    house.tags = {"square"};

    // Party walls only where the plots touch.
    if (gap == 0 && k > 0)
    {
      house.party.push_back(nearSide);
    }

    if (gap == 0 && k + 1 < out.plots.size())
    {
      house.party.push_back(farSide);
    }

    if (brief.storeys > 0)
    {
      house.floors = brief.storeys;
    }

    GridClass grid = profile.grid;
    grid.footprint = FootprintKind::Box;
    grid.cellMetres = cell;
    grid.maximumCells = plot.widthCells * plot.depthCells;
    grid.minimumCells = std::max(4u, grid.maximumCells);
    grid.rows = std::max(plot.widthCells, plot.depthCells) + 2u > 16u ? 32u : 16u;
    house.grid = grid;

    std::string why;
    plot.planned = planCellDwelling(house, settings, plot.house, ledger, &why);

    if (!plot.planned)
    {
      plot.note = why;
      ledger.add(LedgerKind::Rejected, PatchId(brief.id), "street plot " + std::to_string(k) + " was not planned: " + why);
    }
  }

  out.solved = out.plannedPlots() == out.plots.size();

  return true;
}

bool realiseCellStreet(const CellStreet &street,
                       const CellDwellingSettings &settings,
                       std::vector<rewrite::DwellingPlan> &plans,
                       std::string *error)
{
  plans.clear();
  plans.resize(street.plots.size());

  const double dx = geom2::toMetres(street.b.x - street.a.x);
  const double dz = geom2::toMetres(street.b.y - street.a.y);
  const double length = std::sqrt(dx * dx + dz * dz);

  if (length < 1e-9)
  {
    return fail(error, "a street with no length");
  }

  const double ux = dx / length, uz = dz / length;
  const double ax = geom2::toMetres(street.a.x), az = geom2::toMetres(street.a.y);

  for (size_t k = 0; k < street.plots.size(); ++k)
  {
    const CellPlot &plot = street.plots[k];

    if (!plot.planned)
    {
      continue;
    }

    if (!realiseCellDwelling(plot.house, settings, plans[k], error))
    {
      return false;
    }

    if (k == 0 || street.plots[k].fromCell != street.plots[k - 1].fromCell + street.plots[k - 1].widthCells)
    {
      // The first plot, or one with a gap before it: nothing is shared.
      continue;
    }

    // The wall on the line shared with the previous plot, along the length the
    // two plots both reach, is the previous plot's to build.
    const CellPlot &before = street.plots[k - 1];
    const double lineU = double(plot.fromCell) * street.cellMetres;
    const double sharedV = double(std::min(plot.depthCells, before.depthCells)) * street.cellMetres;

    rewrite::DwellingPlan &plan = plans[k];

    std::vector<uint64_t> dropped;

    plan.walls.erase(std::remove_if(plan.walls.begin(),
                                    plan.walls.end(),
                                    [&](const rewrite::DwellingWall &wall) {
                                      const double mx = (wall.a.x + wall.b.x) * .5 - ax;
                                      const double mz = (wall.a.z + wall.b.z) * .5 - az;
                                      const double u = mx * ux + mz * uz;
                                      const double v = -mx * uz + mz * ux;

                                      if (std::abs(u - lineU) < .02 && v > -.02 && v < sharedV + .02)
                                      {
                                        dropped.push_back(wall.id);

                                        return true;
                                      }

                                      return false;
                                    }),
                     plan.walls.end());

    // ...and nothing that was cut in it.
    const auto gone = [&](const uint64_t wall) {
      return std::find(dropped.begin(), dropped.end(), wall) != dropped.end();
    };

    plan.openings.erase(std::remove_if(plan.openings.begin(), plan.openings.end(),
                                       [&](const rewrite::DwellingOpening &cut) { return gone(cut.wall); }),
                        plan.openings.end());
    plan.doors.erase(std::remove_if(plan.doors.begin(), plan.doors.end(),
                                    [&](const rewrite::DwellingDoor &leaf) { return gone(leaf.wall); }),
                     plan.doors.end());
  }

  return true;
}

} // namespace procedural::world::generator
