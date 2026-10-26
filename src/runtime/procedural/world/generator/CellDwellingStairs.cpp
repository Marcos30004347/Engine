#include "procedural/world/generator/CellDwellingStairs.hpp"

#include "procedural/geom2/CellShape.hpp"
#include "procedural/world/generator/DwellingStairs.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

using geom2::Board;
using geom2::Dir;
using geom2::kNoCell;

const char *name(const StairForm form)
{
  switch (form)
  {
    case StairForm::Straight:
      return "straight";
    case StairForm::HalfTurn:
      return "half turn";
    case StairForm::QuarterTurn:
      return "quarter turn";
    case StairForm::Trapdoor:
      return "trapdoor";
  }

  return "?";
}

double wellMargin(const CellDwellingSettings &settings)
{
  // A cell edge is a wall's centreline; a flight run to it stood inside the
  // wall on that side, and the user saw it come through.
  return std::max(settings.outerWallMetres, settings.innerWallMetres) * .5 + .02;
}

namespace {

uint32_t roomAt(const CellGrid &grid, const CellFloor &floor, const uint32_t cell)
{
  for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
  {
    if (grid.contains(floor.roomAreas[r], cell))
    {
      return r;
    }
  }

  return UINT32_MAX;
}

bool inRoom(const CellGrid &grid, const Board &room, const uint32_t cell, const int32_t di, const int32_t dj)
{
  return grid.contains(room, grid.cell(int32_t(grid.cellRow(cell)) + di, int32_t(grid.cellColumn(cell)) + dj));
}

// How many flights a form has.
uint32_t legsOf(const StairForm form)
{
  return form == StairForm::Straight ? 1u : 2u;
}

// What a form needs of its well beyond the flights' going: a half turn keeps
// its landing at the far end, a straight flight a strip to arrive on.
double reserveOf(const StairForm form, const CellDwellingSettings &settings)
{
  if (form == StairForm::HalfTurn) return std::max(settings.stairLandingDepthMetres, .8);
  // Compact stairs may share their arrival with the full level cell beyond
  // the well (validated by wellStands), instead of losing an inhabited floor.
  return settings.stairLandingDepthMetres >= 1. ? settings.stairLandingDepthMetres : .4;
}


double flightWidth(const StairForm form, const double cellMetres, const CellDwellingSettings &settings, uint32_t across);

// The stair a form makes in a well this long, if it can be walked: the tread
// gives down to the minimum and no further, and the number of risers is the
// climb's alone. `steps` is per flight.
bool fits(const StairForm form, const double rise, const uint32_t length, const uint32_t across, const double cellMetres, const CellDwellingSettings &settings, StairSteps &steps)
{
  const double available = double(length) * cellMetres - reserveOf(form, settings) - 2. * wellMargin(settings);

  if (available <= .5)
  {
    return false;
  }

  // A flight you cannot walk abreast of yourself is not a flight. A half turn
  // puts two flights side by side in one cell's width, so on a fine grid it
  // silently becomes a pair of half-metre ladders; refusing it here leaves the
  // straight flight, which has the whole width.
  if (flightWidth(form, cellMetres, settings, across) < settings.minimumFlightWidthMetres - 1e-9)
  {
    return false;
  }

  steps = stepsFor(rise / double(legsOf(form)), settings.comfortableRiserMetres, available);

  return steps.risers > 0 && steps.tread >= settings.minimumTreadMetres && steps.going <= available + 1e-6;
}

// How many cells across a form's well has to be for its flights to be walked.
// A straight flight wants one flight's width; a half turn two side by side.
uint32_t wellWidthFor(const StairForm form, const double cellMetres, const CellDwellingSettings &settings)
{
  // The same allowance `flightWidth` charges, or the two disagree and a well
  // one cell wide that would in fact hold a flight is never tried.
  const double flights = double(legsOf(form)) * settings.minimumFlightWidthMetres + (form == StairForm::HalfTurn ? .1 : .06);

  return std::max(1u, std::min(3u, uint32_t(std::ceil((flights + 2. * wellMargin(settings)) / std::max(.25, cellMetres) - 1e-9))));
}

// The longest well any form is allowed, in cells: about four and a half
// metres of run, which is what a storey's climb wants at a tread you can
// stand on, and never fewer than the three cells the coarse grids assumed.
uint32_t maximumWellCells(const double cellMetres)
{
  return std::max(3u, std::min(8u, uint32_t(std::ceil(4.6 / std::max(.25, cellMetres)))));
}

// The shortest well a form can be walked in, up to three cells; nought when
// none can.
uint32_t wellLengthFor(const StairForm form, const double rise, const uint32_t across, const double cellMetres, const CellDwellingSettings &settings, StairSteps &steps)
{
  // In METRES, not in cells. Three cells is four and a half metres of well on
  // a cottage's cell and three metres on a city lot's, and three metres of
  // well fits no walkable flight for a three metre storey - so every city
  // house fell through to a ladder. A well is as long as the climb needs.
  const double risers=std::ceil(rise/double(legsOf(form))/settings.comfortableRiserMetres);
  const double needed=std::max(0.,risers-1.)*settings.minimumTreadMetres+
    reserveOf(form,settings)+2.*wellMargin(settings);
  const uint32_t maximum=std::max(maximumWellCells(cellMetres),
    uint32_t(std::ceil(needed/std::max(.25,cellMetres))));
  for (uint32_t length = 1; length <= maximum; ++length)
  {
    if (fits(form, rise, length, across, cellMetres, settings, steps))
    {
      return length;
    }
  }

  return 0;
}

// The width one flight of a form gets across the well: a straight flight the
// stair's width, the two flights of a half turn half the cell each less a
// hair between them.
double flightWidth(const StairForm form, const double cellMetres, const CellDwellingSettings &settings, const uint32_t across)
{
  const double inside = double(std::max(1u, across)) * cellMetres - 2. * wellMargin(settings);

  // What is left after the walls: a half turn loses a hand's width between
  // its two flights, a straight one only its nosing. Charging a straight
  // flight a fifth of a metre made a one-cell well on a metre grid 0.56 m
  // wide, which is under any bar, so every city house demanded a two-cell
  // well and the ones that could not give it lost a storey.
  const double most = form == StairForm::HalfTurn ? inside * .5 : inside;

  return std::max(.5, most);
}

// The cells of a well from an entry cell, or nothing if it leaves the grid.
std::vector<uint32_t> line(const CellGrid &grid, const uint32_t entry, const Dir dir, const uint32_t length, const uint32_t across = 1)
{
  std::vector<uint32_t> out;
  uint32_t row = entry;

  // Down the well from the entry cell, and `across` cells to the clockwise
  // side of it: the well is a rectangle, and on a fine grid it has to be, or
  // the flights standing in it are narrower than the cell they stand on.
  for (uint32_t k = 0; k < std::max(1u, across); ++k)
  {
    uint32_t at = row;

    for (uint32_t n = 0; n < length; ++n)
    {
      if (at == kNoCell)
      {
        return {};
      }

      out.push_back(at);
      at = grid.neighbour(at, dir);
    }

    if (row == kNoCell)
    {
      return {};
    }

    row = grid.neighbour(row, cw(dir));
  }

  return out;
}

} // namespace

std::pair<uint32_t,uint32_t> straightStairRoom(double rise,double cell,const CellDwellingSettings &settings)
{
  StairSteps steps;
  const auto width=wellWidthFor(StairForm::Straight,cell,settings);
  const auto length=wellLengthFor(StairForm::Straight,rise,width,cell,settings,steps);
  return {length ? length+2 : 0,width+1};
}

std::vector<uint32_t> wellCells(const CellGrid &grid, const CellStair &stair)
{
  const std::vector<uint32_t> cells = line(grid, stair.cell, stair.dir, std::max(1u, stair.length), std::max(1u, stair.across));

  return cells.empty() ? std::vector<uint32_t>{stair.cell} : cells;
}

// The cells no flight may stand on: the entrance landing, the spiral's and
// the stairwell's, every door's landing on both sides, and every cell an
// earlier stair on this floor took with the eight round each.
Board blockedCells(const CellGrid &grid, const CellHouse &house, const CellFloor &floor, const bool withDoors = true)
{
  Board reserved;

  // The lane under a gateway house. It is floor on the storey above and open
  // air below; a flight standing in it has nothing to stand on.
  reserved = reserved | house.passageReserve;

  if (house.entranceLanding != kNoCell)
  {
    grid.add(reserved, house.entranceLanding);
  }

  if (house.spiral.has_value())
  {
    grid.add(reserved, house.spiral->landing);
  }

  if (house.stairwell.has_value())
  {
    grid.add(reserved, house.stairwell->landing);
  }

  // Never where a door lands: a flight there is a step you fall down as you
  // come through.
  for (const CellDoor &door : floor.doors)
  {
    if (!withDoors)
    {
      break;
    }

    grid.add(reserved, grid.edgeCell(door.edge));
    grid.add(reserved, grid.edgeCell(grid.twin(door.edge)));
  }

  // And never beside a flight this floor already carries.
  //
  // Watabou burns the eight neighbours of a taken cell on whichever floor
  // shares a room with it, and the reason is worth keeping: a middle storey has
  // a flight going down and another going up, and without the burn they land
  // in the same corner of the same room and read as one wide staircase that
  // goes nowhere.
  for (const CellStair &stair : floor.stairs)
  {
    for (const uint32_t cell : wellCells(grid, stair))
    {
      grid.add(reserved, cell);

      // A trapdoor is a hatch with a lid: it takes its cell and nothing
      // round it. Burning its neighbours left a small floor with a hatch
      // from below and no cell for a flight up, and the next floor the same
      // - one ladder became a house of ladders.
      if (stair.form == StairForm::Trapdoor)
      {
        continue;
      }

      const uint32_t room = roomAt(grid, floor, cell);

      for (const auto &[di, dj] : geom2::kRose)
      {
        const uint32_t near = grid.cell(int32_t(grid.cellRow(cell)) + di, int32_t(grid.cellColumn(cell)) + dj);

        if (near != kNoCell && room != UINT32_MAX && grid.contains(floor.roomAreas[room], near))
        {
          grid.add(reserved, near);
        }
      }
    }
  }

  return reserved;
}

Board stairCells(const CellGrid &grid, const CellHouse &house, const CellFloor &floor)
{
  Board out;
  const Board reserved = blockedCells(grid, house, floor);
  Board taken;

  for (const CellStair &stair : floor.stairs)
  {
    for (const uint32_t cell : wellCells(grid, stair))
    {
      grid.add(taken, cell);
    }
  }

  for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
  {
    const Board &room = floor.roomAreas[r];

    for (const auto &[i, j] : geom2::cells(room))
    {
      const uint32_t c = grid.cell(int32_t(i), int32_t(j));

      if (grid.contains(reserved, c))
      {
        continue;
      }

      const bool north = inRoom(grid, room, c, -1, 0);
      const bool south = inRoom(grid, room, c, 1, 0);
      const bool east = inRoom(grid, room, c, 0, 1);
      const bool west = inRoom(grid, room, c, 0, -1);

      // A one-wide passage cell. A flight standing here seals the corridor,
      // and the corridor is the only way past.
      if (!north && !south && east && west)
      {
        continue;
      }

      if (!east && !west && north && south)
      {
        continue;
      }

      // A re-entrant corner: two cardinals open and the diagonal between them
      // closed, so the cell is one you walk round rather than through.
      if (north && east && !inRoom(grid, room, c, -1, 1))
      {
        continue;
      }

      if (north && west && !inRoom(grid, room, c, -1, -1))
      {
        continue;
      }

      if (south && east && !inRoom(grid, room, c, 1, 1))
      {
        continue;
      }

      if (south && west && !inRoom(grid, room, c, 1, -1))
      {
        continue;
      }

      // And the room has to survive losing it, along with whatever flights it
      // already carries. Two cells that are each fine on their own can still
      // cut a room in half between them, which no local test would see.
      Board rest = room;
      grid.remove(rest, c);
      rest = rest & ~taken;

      if (!rest.empty() && !grid.connected(rest))
      {
        continue;
      }

      grid.add(out, c);
    }
  }

  return out;
}

bool linkFloors(const CellGrid &grid,
                const CellHouse &house,
                CellFloor &lower,
                CellFloor &upper,
                const CellDwellingSettings &settings,
                Stream &stream,
                Ledger &ledger)
{
  const double rise = house.gridClass.storeyMetres;
  const double cell = house.frame.cellMetres;

  const auto place = [&](const uint32_t at, const Dir dir, const uint32_t length, const StairForm form, const StairSteps &steps, const bool mirrored = false, const uint32_t across = 1) {
    CellStair below;
    below.cell = at;
    below.toLevel = upper.level;
    below.dir = dir;
    below.length = length;
    below.across = std::max(1u, across);
    below.form = form;
    below.up = true;
    below.risers = steps.risers;
    below.riser = steps.riser;
    below.tread = steps.tread;
    below.width = flightWidth(form, cell, settings, below.across);
    below.landing = form == StairForm::HalfTurn ? double(length) * cell - 2. * wellMargin(settings) - steps.going : reserveOf(form, settings);
    below.mirrored = mirrored;
    lower.stairs.push_back(below);

    CellStair above = below;
    above.toLevel = lower.level;
    above.up = false;
    upper.stairs.push_back(above);
  };

  // Whether a well of these cells can stand between the two floors: every
  // cell in one room on each floor, with the approach cell (before the
  // entry, on the lower floor) and the departure cell (on the upper floor:
  // before the entry for a half turn, past the far end for a straight
  // flight) in that room too, and each room whole without the well.
  const auto wellStands = [&](const std::vector<uint32_t> &cells, const Dir dir, const StairForm form, const Board *legalBelow, const Board *legalAbove, const bool /* mirrored */ = false) {
    if (cells.empty())
    {
      return false;
    }

    if(house.stairwell && house.stairwell->integratedTower)
      for(auto c:cells) if(!grid.contains(house.stairwell->block,c)) return false;

    for (const CellFloor *side : {&lower, &upper})
    {
      const bool isLower = side == &lower;
      const Board *legal = isLower ? legalBelow : legalAbove;
      const uint32_t room = roomAt(grid, *side, cells.front());

      if (room == UINT32_MAX)
      {
        return false;
      }

      Board rest = side->roomAreas[room];

      for (const uint32_t c : cells)
      {
        if (!grid.contains(side->roomAreas[room], c) || (legal != nullptr && !grid.contains(*legal, c)))
        {
          return false;
        }

        grid.remove(rest, c);
      }

      // Doorways on both floors need level landing cells outside the well.
      for (const CellDoor &door : side->doors)
      {
        for (const uint32_t edge : {door.edge, grid.twin(door.edge)})
        {
          const uint32_t landing = grid.edgeCell(edge);

          if (std::find(cells.begin(), cells.end(), landing) == cells.end())
          {
            continue;
          }

          // A door must lead onto a separate level cell, never directly onto
          // the first tread or the arrival strip inside the stair well.
          return false;
        }
      }

      // The well cells any earlier stair on this floor took, out too: two
      // wells that each leave the room whole can cut it in half between them.
      for (const CellStair &stair : side->stairs)
      {
        for (const uint32_t c : wellCells(grid, stair))
        {
          grid.remove(rest, c);
        }
      }

      // Require level floor along the actual end of the complete well.
      // Checking only cells.front() missed the other lane of wide/mirrored
      // return stairs; a lateral neighbour could also be behind a solid riser.
      for (const uint32_t c : cells) {
        const Dir exitDir = isLower || form == StairForm::HalfTurn ? opposite(dir) : dir;
        const uint32_t neighbour = grid.neighbour(c, exitDir);
        if (std::find(cells.begin(), cells.end(), neighbour) != cells.end()) continue;
        if (neighbour == kNoCell || !grid.contains(rest, neighbour)) return false;
      }

      if (rest.empty() || !grid.connected(rest))
      {
        return false;
      }
    }

    return true;
  };

  // Reserved stairwells use the same door, approach and room-connectivity
  // constraints as every other stair. Never put the flight on a doorway cell.
  // A well that came up from the floor below continues: the flights of the
  // next storey stand over the ones beneath, in the same well, which is how
  // a dog-leg stair is built through a house. Taken when it stands - the
  // well's own cells are not in its way, only everything else is.
  for (const CellStair &below : lower.stairs)
  {
    if (below.up || below.form == StairForm::Trapdoor)
    {
      continue;
    }

    CellFloor without = lower;
    without.stairs.clear();

    for (const CellStair &other : lower.stairs)
    {
      if (&other != &below)
      {
        without.stairs.push_back(other);
      }
    }

    const Board freeBelow = lower.area & ~blockedCells(grid, house, without, false);
    const Board freeAbove = upper.area & ~blockedCells(grid, house, upper, false);
    const std::vector<uint32_t> cells = wellCells(grid, below);
    StairSteps steps;

    if (fits(below.form, rise, below.length, below.across, cell, settings, steps) &&
        wellStands(cells, below.dir, below.form, &freeBelow, &freeAbove, below.mirrored))
    {
      place(below.cell, below.dir, below.length, below.form, steps, below.mirrored, below.across);

      return true;
    }

  }

  // Every legal well, twice: once at the dimensions a stair is supposed to
  // have, and if nothing fits, once at a STEEP one - a narrower flight with a
  // shallower tread, which is what a cramped house of this age actually has.
  // A steep stair is drawn and can be climbed; the ladder it replaces was
  // drawn as nothing at all and left the storey above unreachable.
  const auto search = [&](const CellDwellingSettings &settings) {
    const Board legalBelow = lower.area & ~blockedCells(grid, house, lower, false);
    const Board legalAbove = upper.area & ~blockedCells(grid, house, upper, false);

    // Every entry cell and direction, with the two forms and the well each
    // needs. Chosen rather than fallen into, because the form decides the run
    // and the run decides whether the flight can be walked up.
    struct Option
    {
      uint32_t cell = kNoCell;
      Dir dir = Dir::North;
      uint32_t length = 1;
      uint32_t across = 1;
      StairForm form = StairForm::Trapdoor;
      StairSteps steps;
      double weight = 0;
      bool mirrored = false;
    };

    std::vector<Option> options;

    for (const StairForm form : {StairForm::HalfTurn, StairForm::Straight})
    {
      StairSteps steps;
      const uint32_t across = wellWidthFor(form, cell, settings);
      const uint32_t length = wellLengthFor(form, rise, across, cell, settings, steps);

      if (length == 0)
      {
        continue;
      }

      for (const auto &[i, j] : geom2::cells(legalBelow & legalAbove))
      {
        const uint32_t at = grid.cell(int32_t(i), int32_t(j));

        for (const Dir dir : geom2::kCardinal)
        {
          const std::vector<uint32_t> cells = line(grid, at, dir, length, across);
          bool mirrored = false;

          if (!wellStands(cells, dir, form, &legalBelow, &legalAbove, false))
          {
            if (form != StairForm::HalfTurn || !wellStands(cells, dir, form, &legalBelow, &legalAbove, true))
            {
              continue;
            }

            mirrored = true;
          }

          Option option;
          option.cell = at;
          option.dir = dir;
          option.length = length;
          option.across = across;
          option.form = form;
          option.steps = steps;
          option.mirrored = mirrored;

          // A turning stair is the ordinary answer; a straight one the
          // exception, for a small room where the turn's two flights and the
          // landing would take more of the floor than one flight does. Both
          // prefer to stand against a wall: a well with the outside of the
          // room along its side is out of everyone's way.
          option.weight = form == StairForm::Straight ? 1. : 3.;
          if (house.stairwell && grid.contains(house.stairwell->block, at))
            option.weight *= 8.;

          const uint32_t room = roomAt(grid, lower, at);
          const auto circulation=[&](const CellFloor &floor) {
            if(grid.contains(floor.circulation,at))return true;
            const auto r=roomAt(grid,floor,at);
            if(r>=floor.roomKinds.size())return false;
            const auto kind=floor.roomKinds[r];
            return kind==rewrite::RoomKind::Passage || kind==rewrite::RoomKind::Hall ||
              kind==rewrite::RoomKind::Landing || kind==rewrite::RoomKind::Lobby ||
              kind==rewrite::RoomKind::Staircase;
          };
          if(circulation(lower))option.weight*=6.;
          if(circulation(upper))option.weight*=3.;
          bool backed = false;

          for (const Dir side : {cw(dir), opposite(cw(dir))})
          {
            bool wall = true;

            for (const uint32_t c : cells)
            {
              wall = wall && (room == UINT32_MAX || !grid.contains(lower.roomAreas[room], grid.neighbour(c, side)));
            }

            backed = backed || wall;
          }

          if (backed)
          {
            option.weight *= 2.;
          }

          // The straight flight is the small room's answer: where the room
          // has fewer than eight cells it is the one that leaves room to live.
          if (room != UINT32_MAX && lower.roomAreas[room].count() < 8)
          {
            option.weight *= form == StairForm::Straight ? 3. : .5;
          }

          options.push_back(option);
        }
      }
    }

    if (settings.preferStraightStairs && std::any_of(options.begin(),options.end(),
        [](const Option &option){return option.form==StairForm::Straight;})) {
      options.erase(std::remove_if(options.begin(),options.end(),
        [](const Option &option){return option.form!=StairForm::Straight;}),options.end());
    }
    if (!options.empty())
    {
      std::vector<double> weights;
      std::vector<uint32_t> indices;

      for (uint32_t k = 0; k < uint32_t(options.size()); ++k)
      {
        indices.push_back(k);
        weights.push_back(options[k].weight);
      }

      const Option &chosen = options[stream.weighted(indices, weights)];
      place(chosen.cell, chosen.dir, chosen.length, chosen.form, chosen.steps, chosen.mirrored, chosen.across);

      return true;
    }
    return false;
  };

  if (search(settings))
  {
    return true;
  }

  {
    CellDwellingSettings steep = settings;
    steep.minimumTreadMetres = std::min(settings.minimumTreadMetres, .20);
    steep.minimumFlightWidthMetres = std::min(settings.minimumFlightWidthMetres, .65);
    steep.comfortableRiserMetres = std::max(settings.comfortableRiserMetres, .21);
    steep.stairLandingDepthMetres = std::min(settings.stairLandingDepthMetres, .7);

    if (search(steep))
    {
      ledger.add(LedgerKind::Repaired, 0, "no ordinary flight fits; a steep one was built");

      return true;
    }
  }


  if(house.stairwell && house.stairwell->integratedTower) {
    ledger.add(LedgerKind::Rejected,0,"tower cannot fit a flight with a separate level entry landing");
    return false;
  }

  // Nothing fits. A trapdoor and a ladder, on any cell the two floors share
  // that is not already carrying a flight - and it goes in the ledger, because
  // a house full of trapdoors is a house whose cells are too small.
  Board shared = lower.area & upper.area;

  for (const CellFloor *side : {&lower, &upper})
  {
    for (const CellStair &stair : side->stairs)
    {
      for (const uint32_t c : wellCells(grid, stair))
      {
        grid.remove(shared, c);
      }
    }
  }

  if (shared.empty())
  {
    ledger.add(LedgerKind::Rejected, 0, "two floors share no cell a stair could stand in");

    return false;
  }

  // A trapdoor is a worse answer than a flight, not a licence to ignore the
  // rules a flight obeys. It still may not cut a room in half, and it still may
  // not land beside a flight already standing in the same room - a ladder in
  // the corner of a stair hall next to the stair is not a second way up, it is
  // the same way up drawn twice.
  Board decent;

  for (const auto &[ci, cj] : geom2::cells(shared))
  {
    const uint32_t c = grid.cell(int32_t(ci), int32_t(cj));
    bool ok = true;

    for (const CellFloor *side : {&lower, &upper})
    {
      const uint32_t room = roomAt(grid, *side, c);

      if (room == UINT32_MAX)
      {
        continue;
      }

      Board rest = side->roomAreas[room];
      grid.remove(rest, c);

      for (const CellStair &stair : side->stairs)
      {
        for (const uint32_t taken : wellCells(grid, stair))
        {
          if (grid.contains(side->roomAreas[room], taken))
          {
            grid.remove(rest, taken);

            for (const auto &[di, dj] : geom2::kRose)
            {
              if (grid.cell(int32_t(ci) + di, int32_t(cj) + dj) == taken)
              {
                ok = false;
              }
            }
          }
        }
      }

      if (!rest.empty() && !grid.connected(rest))
      {
        ok = false;
      }
    }

    if (ok)
    {
      grid.add(decent, c);
    }
  }

  const Board pool = decent.empty() ? shared : decent;
  const std::vector<std::pair<uint32_t, uint32_t>> candidates = geom2::cells(pool);
  const auto &[i, j] = candidates[stream.below(uint32_t(candidates.size()))];
  const uint32_t at = grid.cell(int32_t(i), int32_t(j));

  ledger.add(LedgerKind::Repaired, 0, "no walkable flight fits; a trapdoor was used instead");
  place(at, Dir::North, 1, StairForm::Trapdoor, StairSteps{});

  return true;
}

bool planStairs(CellHouse &house,
                const CellDwellingSettings &settings,
                Stream &stream,
                Ledger &ledger,
                std::string *error)
{
  for (CellFloor &floor : house.floors)
  {
    floor.stairs.clear();
  }

  if (house.basement.has_value())
  {
    house.basement->stairs.clear();
  }

  // A turret carries the whole climb. A house with one has no flights inside it
  // at all, which is the point of building one.
  if (house.scheme == StairScheme::Spiral)
  {
    return true;
  }

  for (size_t k = 1; k < house.floors.size(); ++k)
  {
    if (!linkFloors(house.grid, house, house.floors[k - 1], house.floors[k], settings, stream, ledger))
    {
      if (error != nullptr)
      {
        *error = "two floors could not be linked";
      }

      return false;
    }

    // A floor a flight cannot reach is not a floor.
    //
    // The fallback is a hatch and a ladder, and the realisation draws neither -
    // so the storey above stood there with no way into it and no way to see
    // that from the plan. A loft or a bell chamber may be climbed by a ladder
    // and says so (`allowTrapdoor`); an ordinary house loses the storey, which
    // is honest and is also the thing the massing should show.
    bool laddered = false;

    for (const CellStair &stair : house.floors[k - 1].stairs)
    {
      laddered = laddered || (stair.up && stair.toLevel == house.floors[k].level && stair.form == StairForm::Trapdoor);
    }

    if (!laddered || settings.allowTrapdoor)
    {
      continue;
    }

    const int32_t dropped = house.floors[k].level;

    const auto unlink = [dropped](std::vector<CellStair> &stairs) {
      stairs.erase(std::remove_if(stairs.begin(), stairs.end(), [dropped](const CellStair &stair) { return stair.toLevel == dropped; }), stairs.end());
    };

    unlink(house.floors[k - 1].stairs);
    ledger.add(LedgerKind::Omitted, 0, "no walkable flight reaches floor " + std::to_string(dropped) + "; it and everything above it were dropped");
    house.floors.resize(k);
    house.plannedFloors = uint32_t(k);

    break;
  }

  if (house.basement.has_value())
  {
    // The basement hangs off the ground floor, and if it cannot be reached it
    // is not a basement - it is dropped rather than left sealed.
    if (!linkFloors(house.grid, house, house.basement.value(), house.floors.front(), settings, stream, ledger))
    {
      ledger.add(LedgerKind::Omitted, 0, "the cellar could not be reached and was dropped");
      house.basement.reset();
    }
    else if (!settings.allowTrapdoor)
    {
      // A cellar reached by a hatch is a cellar reached by nothing: the hatch
      // is not drawn either. Same rule as a storey - it goes.
      bool laddered = false;

      for (const CellStair &stair : house.basement->stairs)
      {
        laddered = laddered || stair.form == StairForm::Trapdoor;
      }

      if (laddered)
      {
        const int32_t level = house.basement->level;

        house.floors.front().stairs.erase(
            std::remove_if(house.floors.front().stairs.begin(), house.floors.front().stairs.end(), [level](const CellStair &stair) { return stair.toLevel == level; }),
            house.floors.front().stairs.end());
        ledger.add(LedgerKind::Omitted, 0, "no walkable flight reaches the cellar; it was dropped");
        house.basement.reset();
      }
    }
  }

  return true;
}

} // namespace procedural::world::generator
