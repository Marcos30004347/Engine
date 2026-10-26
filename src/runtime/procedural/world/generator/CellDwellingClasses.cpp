#include "procedural/world/generator/CellDwellingClasses.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace procedural::world::generator {

namespace {

// Every cell of an axis-aligned block, appended.
void block(std::vector<std::pair<int32_t, int32_t>> &into,
           const int32_t i0,
           const int32_t j0,
           const int32_t rows,
           const int32_t columns)
{
  for (int32_t i = 0; i < rows; ++i)
  {
    for (int32_t j = 0; j < columns; ++j)
    {
      into.emplace_back(i0 + i, j0 + j);
    }
  }
}

void unique(std::vector<std::pair<int32_t, int32_t>> &cells)
{
  std::sort(cells.begin(), cells.end());
  cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
}

// How deep a range is, in cells.
//
// Bounded, because a range is a range: two rooms deep is a house one room thick
// with a passage down it, and past four the middle of the plan is ground no
// window reaches. The bound rises with the building because a castle's ranges
// are the width of a great hall and a cottage's are the width of a parlour.
int32_t wingDepth(const double target, Stream &stream)
{
  const int32_t deepest = std::clamp(int32_t(std::lround(std::sqrt(target) / 4.)), 2, 4);

  return deepest - (stream.unit() < .45 && deepest > 2 ? 1 : 0);
}

// One archetype at one length. The length is what the search below varies.
std::vector<std::pair<int32_t, int32_t>> pieces(const FootprintKind kind,
                                                const int32_t depth,
                                                const int32_t run,
                                                const double wingShare,
                                                const double northShare,
                                                const double southShare)
{
  std::vector<std::pair<int32_t, int32_t>> out;

  const int32_t wing = std::max(depth + 1, int32_t(std::lround(double(run) * wingShare)));

  switch (kind)
  {
    case FootprintKind::Polyomino:
    case FootprintKind::Box:
    {
      // A plain rectangle, which is what a warehouse or a terrace house is. The
      // wing share doubles as the proportion, so a box drawn long stays long.
      const int32_t rows = std::max(2, int32_t(std::lround(double(run) * (.45 + wingShare))));
      block(out, 0, 0, rows, run);
      break;
    }

    case FootprintKind::RangeL:
    {
      block(out, 0, 0, depth, run);
      block(out, depth, 0, wing - depth, depth);
      break;
    }

    case FootprintKind::RangeH:
    {
      block(out, 0, 0, depth, run);
      block(out, depth, 0, wing - depth, depth);
      block(out, depth, run - depth, wing - depth, depth);
      break;
    }

    case FootprintKind::RangeE:
    {
      block(out, 0, 0, depth, run);
      block(out, depth, 0, wing - depth, depth);
      block(out, depth, run - depth, wing - depth, depth);
      block(out, depth, (run - depth) / 2, wing - depth, depth);
      break;
    }

    case FootprintKind::Courtyard:
    {
      // A ring, `run` cells across the outside. The court is what the ring is
      // for, so the ranges are as thick as any other range and whatever is left
      // in the middle is the court - never the other way round, which gives a
      // court the size of a light well and a building the size of a block.
      const int32_t court = run - 2 * depth;

      if (court < 2)
      {
        return {};
      }

      for (int32_t i = 0; i < run; ++i)
      {
        for (int32_t j = 0; j < run; ++j)
        {
          const bool inCourt = i >= depth && i < depth + court && j >= depth && j < depth + court;

          if (!inCourt)
          {
            out.emplace_back(i, j);
          }
        }
      }

      break;
    }

    case FootprintKind::Axial:
    {
      // Nave, crossing, transepts, chancel: two arms of unequal length with the
      // transepts at their junction.
      //
      // Written as two arms rather than as four named pieces, and that is the
      // shape the partition downstream knows how to read. A version that laid
      // the pieces out explicitly - bar, two transepts near the far end, tower
      // bays - is a better structure and produced a worse building, because the
      // partition infers the parts from the silhouette and a cross with its
      // transepts at the end does not read as one. Both halves want rewriting
      // together; until then this is the pair that agree.
      const int32_t width = depth + 1;
      const int32_t far = std::max(5, (run * 3) / 2);
      const int32_t arm = std::max(2, width - 1);

      // The two arms are drawn independently, and either may be nothing.
      //
      // A cross with two identical transepts is a diagram. Real ones grew: a
      // chapel added on one side, a vestry on the other and smaller, sometimes
      // only one arm at all. The shares come from the seed before the length
      // search, so the same church keeps the same arms however long it ends up.
      int32_t north = int32_t(std::lround(double(arm) * northShare));
      int32_t south = int32_t(std::lround(double(arm) * southShare));

      // One arm may go; both may not. A cross with neither is a bar, and while
      // the partition handles that now, a church class that draws a plain box
      // one time in thirty-six is not variation, it is a mistake.
      if (north < 2 && south < 2)
      {
        (northShare >= southShare ? north : south) = arm;
      }
      const int32_t deepest = std::max(north, south);
      const int32_t crossing = arm + far - width;

      block(out, 0, arm, width, far);       // the long arm
      block(out, 0, arm + far, width, run); // the short arm

      if (north >= 2)
      {
        block(out, -north, crossing, north, width);
      }

      if (south >= 2)
      {
        block(out, width, crossing, south, width);
      }

      (void)deepest;
      break;
    }
  }

  unique(out);

  return out;
}

// The bounding box of a cell list.
void extent(const std::vector<std::pair<int32_t, int32_t>> &cells, int32_t &rows, int32_t &columns)
{
  rows = 0;
  columns = 0;

  if (cells.empty())
  {
    return;
  }

  int32_t lowI = cells.front().first;
  int32_t lowJ = cells.front().second;
  int32_t highI = lowI;
  int32_t highJ = lowJ;

  for (const auto &[i, j] : cells)
  {
    lowI = std::min(lowI, i);
    lowJ = std::min(lowJ, j);
    highI = std::max(highI, i);
    highJ = std::max(highJ, j);
  }

  rows = highI - lowI + 1;
  columns = highJ - lowJ + 1;
}

} // namespace

const char *name(const FootprintKind kind)
{
  switch (kind)
  {
    case FootprintKind::Polyomino:
      return "polyomino";
    case FootprintKind::Box:
      return "box";
    case FootprintKind::RangeL:
      return "L range";
    case FootprintKind::RangeH:
      return "H range";
    case FootprintKind::RangeE:
      return "E range";
    case FootprintKind::Courtyard:
      return "courtyard";
    case FootprintKind::Axial:
      return "axial";
  }

  return "?";
}

const ClassProfile &profileFor(const BuildingClass building)
{
  // Read against F.5. The cell size is the one number that cannot be changed
  // later without moving every size band with it, so it is set per class here
  // and nowhere else.
  static const std::array<ClassProfile, 21> table = [] {
    std::array<ClassProfile, 21> out{};

    const auto set = [&out](const BuildingClass which,
                            const uint32_t rows,
                            const double cell,
                            const uint32_t low,
                            const uint32_t high,
                            const double roomCells,
                            const uint32_t threshold,
                            const double courtChance,
                            const double courtShare,
                            const uint32_t courts,
                            const double symmetry,
                            const PlotStanding standing,
                            const uint32_t minStoreys,
                            const uint32_t maxStoreys) {
      ClassProfile &profile = out[size_t(which)];
      profile.grid.rows = rows;
      profile.grid.cellMetres = cell;
      profile.grid.minimumCells = low;
      profile.grid.maximumCells = high;
            // A church's storey is an ordinary storey now. It used to be six and a
      // half metres, which made the vestry six and a half metres too; the nave
      // and chancel get their height from being TALL ROOMS instead, so the
      // transepts beside them can be a normal height and the roofs step. A
      // shade taller than a house's, because even the aisle of a church is.
      profile.grid.storeyMetres =
        which == BuildingClass::Church || which == BuildingClass::Temple ? 3.25 : 3.;
      profile.circulationThresholdCells = threshold;

      // Two cells across, everywhere, at every scale. A cell is two to three
      // metres, so this is a room four metres in its smaller direction, and
      // below that there is nothing a room is for: you cannot put a bed across
      // it or a table in it, and on a blueprint it reads as a passage that
      // somebody has mislabelled. It costs a cottage its scullery and that is
      // the right trade - a cottage with two rooms is a cottage.
      //
      // The corridor widens later and for a different reason: it is the scale
      // at which a passage becomes a gallery.
      profile.minimumRoomCells = 2;

      // A tower where the building is big enough to need one. At a hundred and
      // fifty cells and four storeys the stair is carrying a household between
      // floors the size of a street, and one cell of it is a ladder.
      // Four cells across on a castle. Three looked like a chimney beside three
      // hundred cells of ranges; a stair tower is a structure, and at three
      // metres a cell that is twelve metres square.
      profile.stairTowerCells = high >= 400 ? 4u : (high >= 140 ? 3u : (high >= 90 ? 2u : 1u));
      // Three storeys on the largest, two on the rest, and never one. A tower
      // has to clear the rooms beside it by enough to read as a tower: one
      // storey above a mass four storeys tall is a bump on the roof, and
      // rendered it reads as a dormer. A tower is the thing you see from
      // outside the walls before you see the building.
      profile.stairTowerAbove = high >= 400 ? 3u : (high >= 140 ? 2u : 0u);

      // Four at the angles on a castle, two on the next tier down. A castle
      // without towers is a walled house.
      profile.cornerTowers = high >= 400 ? 4u : (high >= 140 ? 2u : 0u);
      profile.corridorCells = roomCells >= 9. ? 2u : 1u;
      profile.courtChance = courtChance;
      profile.courtShare = courtShare;
      profile.courts = courts;

      // A yard follows the standing. A craftsman's and a farmer's plots are
      // worked from the yard; a merchant's has one more often than not; and
      // above that the ground is grounds. An axial building has none: a
      // churchyard is the lot, not a room.
      profile.yardChance = standing == PlotStanding::Modest || standing == PlotStanding::Rural ? .65
                           : standing == PlotStanding::Comfortable                             ? .4
                                                                                              : 0.;
      profile.yardShare = standing == PlotStanding::Rural ? .18 : .14;
      profile.penChance = standing == PlotStanding::Rural ? .7 : 0.;
      profile.shedChance = standing == PlotStanding::Rural ? .6 : 0.;
      profile.symmetryChance = symmetry;
      profile.averageRoomCells = roomCells;
      profile.standing = standing;
      profile.minimumStoreys = minStoreys;
      profile.maximumStoreys = maxStoreys;
    };

    constexpr uint32_t never = 100000;

    // The circulation thresholds are all about five rooms' worth of cells for
    // the class, and that is not a coincidence: below five rooms the
    // subdivision's own narrow pieces are the corridor, and above it recursive
    // halving has no notion of a route from the door to the far wing.

    set(BuildingClass::PeasantCottage, 16, 2.0, 8, 14, 5, never, 0, 0, 0, 0, PlotStanding::Rural, 1, 2);
    // One storey, four to ten cells, rooms of six: one room or two, never a
    // stair. The subdivision cannot make more rooms than the cells allow.
    set(BuildingClass::Hut, 16, 2.0, 4, 10, 6, never, 0, 0, 0, 0, PlotStanding::Rural, 1, 1);
    // Keep the structural grid/stair clearances, but subdivide domestic
    // interiors into approximately 15–20 m² rooms rather than 25–40 m².
    set(BuildingClass::Dwelling, 16, 2.2, 10, 34, 3.5, never, 0, 0, 0, 0, PlotStanding::Modest, 1, 8);
    set(BuildingClass::ShopHouse, 16, 2.2, 12, 24, 4, never, 0, 0, 0, 0, PlotStanding::Modest, 2, 3);
    set(BuildingClass::Dormitory, 16, 2.2, 14, 30, 5, 25, 0.15, 0.12, 1, 0.2, PlotStanding::Slum, 2, 4);
    set(BuildingClass::Inn, 32, 2.4, 30, 60, 3.5, 20, 0.45, 0.14, 1, 0, PlotStanding::Comfortable, 2, 3);
    set(BuildingClass::ManorHouse, 32, 2.6, 60, 120, 9, 45, 0.5, 0.16, 1, 0.35, PlotStanding::Wealthy, 2, 3);
    set(BuildingClass::Mansion, 32, 2.8, 90, 170, 10, 50, 0.6, 0.18, 1, 0.6, PlotStanding::Grand, 2, 3);
    set(BuildingClass::GuildHall, 32, 2.8, 60, 110, 12, 50, 0.3, 0.14, 1, 0.7, PlotStanding::Comfortable, 2, 2);
    set(BuildingClass::CommerceHouse, 32, 2.6, 40, 90, 8, 40, 0.35, 0.14, 1, 0.25, PlotStanding::Comfortable, 2, 4);
    set(BuildingClass::Castle, 64, 3.0, 150, 420, 8, 60, 0.9, 0.2, 2, 0.2, PlotStanding::Grand, 2, 4);
    set(BuildingClass::Prison, 32, 3.0, 60, 140, 8, 40, 0.6, 0.18, 1, 0.5, PlotStanding::Modest, 2, 3);
    set(BuildingClass::School, 32, 2.6, 40, 90, 9, 45, 0.5, 0.16, 1, 0.5, PlotStanding::Comfortable, 2, 2);
    set(BuildingClass::Dispensary, 16, 2.4, 16, 36, 6, never, 0, 0, 0, 0.2, PlotStanding::Comfortable, 2, 2);
    set(BuildingClass::Farmstead, 32, 2.6, 30, 80, 8, 40, 0.4, 0.16, 1, 0, PlotStanding::Rural, 1, 2);
    set(BuildingClass::Warehouse, 32, 3.0, 40, 110, 24, never, 0.2, 0.14, 1, 0.3, PlotStanding::Modest, 1, 3);
    set(BuildingClass::Church, 32, 3.0, 36, 84, 18, never, 0, 0, 0, 0.8, PlotStanding::Comfortable, 1, 1);
    set(BuildingClass::Temple, 32, 3.0, 36, 84, 18, never, 0, 0, 0, 0.8, PlotStanding::Comfortable, 1, 1);
    set(BuildingClass::Chapel, 16, 2.8, 12, 34, 14, never, 0, 0, 0, 0.6, PlotStanding::Comfortable, 1, 1);

    // The two classes whose outline is not the seed's to choose.
    //
    // This is the one exception to the rule above, and it is worth stating why
    // it is not a relapse. "Church" is not a size of building, it is a SHAPE:
    // nave, crossing, transepts, chancel, laid on an east-west axis, and a
    // stencil that happens to come out cruciform is not a church any more than
    // one that happens to come out square is a tower. Every other class is a
    // household or a trade, and how big it is and how fine its rooms are is all
    // there is to say about it. A chapel is left alone deliberately: a
    // preaching box is a room, which is exactly what the stencil makes.
    out[size_t(BuildingClass::Church)].grid.footprint = FootprintKind::Axial;
    out[size_t(BuildingClass::Church)].yardChance = 0;
    out[size_t(BuildingClass::Temple)].yardChance = 0;
    out[size_t(BuildingClass::Temple)].grid.footprint = FootprintKind::Axial;

    return out;
  }();

  const size_t index = size_t(building);

  return index < table.size() ? table[index] : table[size_t(BuildingClass::Dwelling)];
}

std::vector<std::pair<int32_t, int32_t>> archetype(const FootprintKind kind,
                                                   const GridClass &grid,
                                                   const uint32_t maximumRows,
                                                   const uint32_t maximumColumns,
                                                   Stream &stream)
{
  // The size is drawn across the band rather than fixed at its middle. The band
  // is wide on purpose - a manor is not one size - and drawing across it is
  // what stops every manor in a town coming out the same building.
  const double target =
    double(grid.minimumCells) +
    (double(grid.maximumCells) - double(grid.minimumCells)) * (.3 + .5 * stream.unit());

  const double wingShare = .4 + .3 * stream.unit();

  // How far each transept reaches, drawn once and kept. One in six churches
  // loses an arm altogether, which is the difference between a plan type and a
  // building that was added to.
  const double northShare = stream.unit() < .16 ? 0. : .55 + .75 * stream.unit();
  const double southShare = stream.unit() < .16 ? 0. : .55 + .75 * stream.unit();

  const int32_t rowsAllowed = int32_t(std::min<uint32_t>(maximumRows, 512));
  const int32_t columnsAllowed = int32_t(std::min<uint32_t>(maximumColumns, 512));

  int32_t depth = wingDepth(target, stream);

  if (kind == FootprintKind::Axial)
  {
    // A cross is three ranges across its short way: a transept, the crossing,
    // and the other transept. So the short axis needs three times the range
    // depth, and on a churchyard that is only eight cells deep the nave has to
    // be narrower than the class would otherwise draw it - or nothing fits and
    // the church comes out a box.
    // A cross is three ranges across its short way: a transept, the crossing
    // and the other transept. One less than a third left the nave three cells
    // wide however much ground there was, and a church twenty bays long and
    // three wide is a corridor with a spire.
    depth = std::clamp(std::min(rowsAllowed, columnsAllowed) / 3, 1, std::max(depth, 3));
  }

  // The length is searched for, not solved for.
  //
  // Solving is what this did first and it is wrong in a way that only shows at
  // the top of the scale: each kind has its own area formula, every formula
  // rounds, and a castle's L range came out a hundred and twelve cells long
  // against a board sixty-four wide. Building the shape and counting it cannot
  // be wrong about either its size or its span, and the search is over a few
  // dozen candidates.
  const int32_t longest = std::max(rowsAllowed, columnsAllowed);

  std::vector<std::pair<int32_t, int32_t>> best;
  double bestMiss = 0;

  if (kind == FootprintKind::Box)
  {
    // A rectangle is two numbers, so it is searched over two.
    //
    // One number was enough while a box meant "a shop, roughly square". It is
    // not enough for a wing: a range against a curtain wall is three cells deep
    // and twenty long because the wall says so, and a box drawn to a fixed
    // proportion and then masked to that ground loses half its area and falls
    // out of its own size band.
    for (int32_t rows = 2; rows <= rowsAllowed; ++rows)
    {
      const int32_t columns =
        std::clamp(int32_t(std::lround(target / double(rows))), 2, columnsAllowed);
      const double miss = std::abs(double(rows) * double(columns) - target);

      if (best.empty() || miss < bestMiss)
      {
        best.clear();
        block(best, 0, 0, rows, columns);
        bestMiss = miss;
      }
    }

    if (!best.empty())
    {
      unique(best);

      return best;
    }
  }

  for (int32_t run = 2; run <= longest; ++run)
  {
    std::vector<std::pair<int32_t, int32_t>> shape = pieces(kind, depth, run, wingShare, northShare, southShare);

    if (shape.empty())
    {
      continue;
    }

    int32_t rows = 0;
    int32_t columns = 0;
    extent(shape, rows, columns);

    // Either way up, and TURNED when that is the way it fits. A range too long
    // for the board across may still stand along it - and merely accepting such
    // a shape without turning it was a bug: the placement loop only offsets, it
    // never rotates, so a shape that fits "the other way" and is handed over as
    // drawn fits nowhere at all.
    const bool asDrawn = rows <= rowsAllowed && columns <= columnsAllowed;
    const bool turned = columns <= rowsAllowed && rows <= columnsAllowed;

    if (!asDrawn && !turned)
    {
      // Everything longer is larger still.
      break;
    }

    if (!asDrawn)
    {
      for (auto &[i, j] : shape)
      {
        std::swap(i, j);
      }
    }

    // A church is sized to its band like everything else.
    //
    // It used to take the longest cross that would fit, on the grounds that a
    // short nave is a chapel - and that is true, but "as long as the churchyard
    // allows" is not a proportion, it is whatever the lot happened to be. On a
    // deep plot it produced a building you could not see the end of. The band
    // is the place to say how big a church is.
    const double miss = std::abs(double(shape.size()) - target);

    if (best.empty() || miss < bestMiss)
    {
      best = std::move(shape);
      bestMiss = miss;
    }
  }

  if (best.empty())
  {
    // Nothing of this kind fits the ground. A rectangle always does, and a
    // building that is the wrong shape is worth having where a building that
    // does not exist is not.
    const int32_t rows = std::max(2, std::min(rowsAllowed, int32_t(std::lround(std::sqrt(target / 1.6)))));
    const int32_t columns = std::max(2, std::min(columnsAllowed, int32_t(std::lround(target / double(rows)))));
    block(best, 0, 0, rows, columns);
    unique(best);
  }

  // Normalise to the origin.
  int32_t lowI = best.front().first;
  int32_t lowJ = best.front().second;

  for (const auto &[i, j] : best)
  {
    lowI = std::min(lowI, i);
    lowJ = std::min(lowJ, j);
  }

  for (auto &[i, j] : best)
  {
    i -= lowI;
    j -= lowJ;
  }

  return best;
}

CellDwellingSettings classed(const CellDwellingSettings &settings, const BuildingClass building, Stream &stream)
{
  const ClassProfile &profile = profileFor(building);

  CellDwellingSettings out = settings;

  out.grid = profile.grid;
  out.averageRoomCells = profile.averageRoomCells;
  out.coarseRoomCells = profile.averageRoomCells * 2.;
  out.circulationThresholdCells = profile.circulationThresholdCells;
  out.minimumRoomCells = profile.minimumRoomCells;
  out.corridorCells = profile.corridorCells;
  out.stairTowerCells = profile.stairTowerCells;
  out.stairTowerAbove = profile.stairTowerAbove;
  out.cornerTowers = profile.cornerTowers;
  out.cornerTowerStoreys = profile.cornerTowerStoreys;
  // Castles can use additional eligible corners; the roof-connectivity and
  // room-ownership checks in raiseTowers still decide which ones fit.
  if(building==BuildingClass::Castle)out.cornerTowers=6;

  // The only draw a class makes, and only when it has something to decide. A
  // class with no chance of symmetry leaves the stream exactly where it found
  // it, so a cottage's every downstream number is what it was before classes
  // existed - and stays that way the day a class above it is retuned.
  if (profile.symmetryChance > 0)
  {
    out.formalFront = stream.unit() < profile.symmetryChance;
  }

  // Likewise: a class with no appetite for a court draws nothing.
  out.courtShare = profile.courtShare;
  out.courts = 0;

  out.yardShare = profile.yardShare;
  out.yards = profile.yardChance > 0 && stream.unit() < profile.yardChance ? 1u : 0u;
  out.pens = profile.penChance > 0 && stream.unit() < profile.penChance ? 1u : 0u;
  out.cellars = profile.grid.maximumCells > 10; // A hut has no cellar.
  out.sheds = out.pens > 0 && profile.shedChance > 0 && stream.unit() < profile.shedChance ? 1u : 0u;

  if (profile.courtChance > 0 && profile.courts > 0)
  {
    out.courts = stream.unit() < profile.courtChance ? profile.courts : 0u;

    // And the second one is not a certainty even where two are allowed. A
    // castle with two courts is the reference plan; a castle with one is every
    // other castle.
    if (out.courts > 1 && stream.unit() < .2)
    {
      out.courts = 1;
    }
  }

  // The class caps the height; the caller may cap it lower. A chapel is one
  // storey whatever the street would allow, and a cottage is not a tenement.
  out.maximumFloors = std::min(settings.maximumFloors, profile.maximumStoreys);

  return out;
}

} // namespace procedural::world::generator
