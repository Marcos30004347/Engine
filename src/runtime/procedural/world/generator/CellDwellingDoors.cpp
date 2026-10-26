#include "procedural/world/generator/CellDwellingDoors.hpp"

#include "procedural/geom2/CellShape.hpp"

#include <algorithm>
#include <map>

namespace procedural::world::generator {

using geom2::Board;
using geom2::Chain;
using geom2::Dir;
using geom2::kNoCell;
using geom2::kNoEdge;

namespace {

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

uint32_t roomAt(const CellGrid &grid, const CellFloor &floor, const uint32_t cell)
{
  if (cell == kNoCell)
  {
    return UINT32_MAX;
  }

  for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
  {
    if (grid.contains(floor.roomAreas[r], cell))
    {
      return r;
    }
  }

  return UINT32_MAX;
}

// How many contour edges two rooms share.
uint32_t sharedEdges(const CellGrid &grid, const CellFloor &floor, const uint32_t a, const uint32_t b)
{
  uint32_t shared = 0;

  for (const uint32_t e : floor.roomContours[a])
  {
    const uint32_t twin = grid.twin(e);

    if (twin != kNoEdge && grid.contains(floor.roomAreas[b], grid.edgeCell(twin)))
    {
      ++shared;
    }
  }

  return shared;
}

// What it costs to pass through a room.
//
// A single cell is dear; a corridor is nearly free, because its area and its
// narrow count are the same number. That one expression is what makes the
// pruning below prefer a route down a hallway over a door cut straight between
// two rooms.
double passingCost(const CellGrid &grid, const CellFloor &floor, const uint32_t room)
{
  // A court costs nothing to cross.
  //
  // It is not a room you pass through on sufferance - it is how the building IS
  // crossed, which is what all three reference plans show. Priced by its area
  // like any other room it came out the dearest thing on the floor, the pruning
  // stripped the doors onto it, and a castle's far range ended up five doors
  // from the gate down a corridor that need not have existed.
  if (room < floor.roomOutdoor.size() && floor.roomOutdoor[room] != 0)
  {
    return 0;
  }

  const uint32_t size = floor.roomAreas[room].count();

  if (size == 1)
  {
    return 10;
  }

  return double(size) - double(geom2::narrow(grid, floor.roomAreas[room]).count());
}

} // namespace

bool roomsConnected(const CellFloor &floor)
{
  const size_t n = floor.roomAreas.size();

  if (n <= 1)
  {
    return true;
  }

  std::vector<std::vector<uint32_t>> links(n);

  for (const CellDoor &door : floor.doors)
  {
    links[door.a].push_back(door.b);
    links[door.b].push_back(door.a);
  }

  std::vector<uint8_t> seen(n, 0);
  std::vector<uint32_t> stack{0};
  seen[0] = 1;
  size_t reached = 1;

  while (!stack.empty())
  {
    const uint32_t at = stack.back();
    stack.pop_back();

    for (const uint32_t next : links[at])
    {
      if (seen[next] == 0)
      {
        seen[next] = 1;
        ++reached;
        stack.push_back(next);
      }
    }
  }

  return reached == n;
}

bool mergeCorridors(const CellGrid &grid, const CellHouse &house, CellFloor &floor, std::string *error)
{
  floor.corridors.clear();

  const uint32_t reserved =
    house.stairwell.has_value() ? roomAt(grid, floor, house.stairwell->stair) : UINT32_MAX;

  // A corridor is a room that cannot be anything else: every cell of it too
  // narrow to hold a two-by-two.
  const auto isCorridor = [&](const uint32_t r) {
    return r != reserved && floor.roomAreas[r].count() > 0 &&
           geom2::narrow(grid, floor.roomAreas[r]).count() == floor.roomAreas[r].count();
  };

  // A fixpoint, restarted after each merge. Watabou's own, and left as his: a
  // floor has three to ten corridors, so the incremental form would save
  // nothing measurable and the restart is obviously correct.
  for (uint32_t pass = 0; pass < uint32_t(floor.roomAreas.size()) + 1; ++pass)
  {
    bool merged = false;

    for (uint32_t a = 0; a < uint32_t(floor.roomAreas.size()) && !merged; ++a)
    {
      if (!isCorridor(a))
      {
        continue;
      }

      for (uint32_t b = a + 1; b < uint32_t(floor.roomAreas.size()) && !merged; ++b)
      {
        if (!isCorridor(b) || sharedEdges(grid, floor, a, b) != 1)
        {
          continue;
        }

        const Board joined = floor.roomAreas[a] | floor.roomAreas[b];
        const std::vector<Chain> loops = grid.outline(joined);

        if (loops.size() != 1 || !grid.simpleOutline(loops))
        {
          continue;
        }

        floor.roomAreas[a] = joined;
        floor.roomContours[a] = loops.front();
        floor.roomAreas.erase(floor.roomAreas.begin() + b);
        floor.roomContours.erase(floor.roomContours.begin() + b);
        merged = true;
      }
    }

    if (!merged)
    {
      break;
    }
  }

  for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
  {
    if (isCorridor(r))
    {
      floor.corridors.push_back(r);
    }
  }

  (void)error;

  return true;
}

bool connectRooms(const CellGrid &grid,
                  const CellHouse &house,
                  CellFloor &floor,
                  const bool prune,
                  Stream &stream,
                  std::string *error)
{
  floor.doors.clear();

  const uint32_t n = uint32_t(floor.roomAreas.size());

  if (n <= 1)
  {
    return true;
  }

  const uint32_t reserved =
    house.stairwell.has_value() ? roomAt(grid, floor, house.stairwell->stair) : UINT32_MAX;
  uint32_t landing =
    house.stairwell.has_value() ? roomAt(grid, floor, house.stairwell->landing) : UINT32_MAX;

  // The landing cell may not be on this floor at all.
  //
  // A cellar runs under part of the house, and the part it runs under can hold
  // the stair block and not the cell the stair was told to open into. Then the
  // reserved room had no landing to join and was kept out of the ordinary rule
  // too, so it joined nothing - and the whole plan was refused for a room
  // nobody could walk to, on three lots in a hundred. The stair opens where
  // its exit edge leads on THIS floor, and if that leads nowhere either, into
  // whichever neighbour it shares the most wall with.
  if (reserved != UINT32_MAX && landing == UINT32_MAX)
  {
    const uint32_t exit = grid.cellEdge(house.stairwell->stair, house.stairwell->exit);

    if (exit != kNoEdge && grid.twin(exit) != kNoEdge)
    {
      landing = roomAt(grid, floor, grid.edgeCell(grid.twin(exit)));
    }

    if (landing == UINT32_MAX || landing == reserved)
    {
      std::map<uint32_t, uint32_t> shared;

      for (const uint32_t e : floor.roomContours[reserved])
      {
        const uint32_t twin = grid.twin(e);

        if (twin == kNoEdge)
        {
          continue;
        }

        const uint32_t b = roomAt(grid, floor, grid.edgeCell(twin));

        if (b != UINT32_MAX && b != reserved)
        {
          ++shared[b];
        }
      }

      landing = UINT32_MAX;
      uint32_t most = 0;

      for (const auto &[room, count] : shared)
      {
        if (count > most)
        {
          most = count;
          landing = room;
        }
      }
    }
  }

  // Which edges join which pair of rooms.
  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> adjacency;

  for (uint32_t a = 0; a < n; ++a)
  {
    // The reserved stair joins one room and nothing else. Left to the ordinary
    // rule it would take a door on every wall it happens to have, and a stair
    // cell with three doors is a crossroads rather than a stair.
    if (a == reserved)
    {
      continue;
    }

    for (const uint32_t e : floor.roomContours[a])
    {
      const uint32_t twin = grid.twin(e);

      if (twin == kNoEdge)
      {
        continue;
      }

      const uint32_t b = roomAt(grid, floor, grid.edgeCell(twin));

      if (b == UINT32_MAX || b == a || b == reserved)
      {
        continue;
      }

      adjacency[{std::min(a, b), std::max(a, b)}].push_back(a < b ? e : twin);
    }
  }

  if (reserved != UINT32_MAX && landing != UINT32_MAX && landing != reserved)
  {
    const uint32_t exit = grid.cellEdge(house.stairwell->stair, house.stairwell->exit);
    const uint32_t low = std::min(reserved, landing);

    // Through the exit edge when that edge is the wall between them, and
    // through any wall they share when it is not - the cellar case above.
    const bool throughTheExit = exit != kNoEdge && grid.twin(exit) != kNoEdge &&
                                roomAt(grid, floor, grid.edgeCell(grid.twin(exit))) == landing;

    if (throughTheExit)
    {
      adjacency[{low, std::max(reserved, landing)}] = {low == reserved ? exit : grid.twin(exit)};
    }
    else
    {
      std::vector<uint32_t> walls;

      for (const uint32_t e : floor.roomContours[reserved])
      {
        const uint32_t twin = grid.twin(e);

        if (twin != kNoEdge && roomAt(grid, floor, grid.edgeCell(twin)) == landing)
        {
          walls.push_back(low == reserved ? e : twin);
        }
      }

      if (!walls.empty())
      {
        adjacency[{low, std::max(reserved, landing)}] = walls;
      }
    }
  }

  // Cells that already carry a door. Doubling their cost is what spreads doors
  // along a wall instead of stacking them in one corner.
  Board used;

  const auto openness = [&](const uint32_t cell, const uint32_t room) {
    double n2 = 1;

    for (const Dir d : geom2::kCardinal)
    {
      if (grid.contains(floor.roomAreas[room], grid.neighbour(cell, d)))
      {
        n2 += 1;
      }
    }

    return grid.contains(used, cell) ? n2 * 2 : n2;
  };

  // The most constrained wall first: a pair with one shared edge has no choice
  // to make, and making it before the roomy ones stops them taking the cell it
  // needed.
  std::vector<std::pair<uint32_t, uint32_t>> pairs;

  for (const auto &entry : adjacency)
  {
    if (!entry.second.empty())
    {
      pairs.push_back(entry.first);
    }
  }

  std::stable_sort(pairs.begin(), pairs.end(), [&](const auto &x, const auto &y) {
    return adjacency[x].size() < adjacency[y].size();
  });

  for (const auto &pair : pairs)
  {
    const std::vector<uint32_t> &edges = adjacency[pair];
    std::vector<uint32_t> best;
    double bestCost = 1e300;

    for (const uint32_t e : edges)
    {
      const uint32_t twin = grid.twin(e);
      const double cost = openness(grid.edgeCell(e), pair.first) + openness(grid.edgeCell(twin), pair.second);

      if (cost < bestCost - 1e-9)
      {
        bestCost = cost;
        best.assign(1, e);
      }
      else if (cost < bestCost + 1e-9)
      {
        best.push_back(e);
      }
    }

    if (best.empty())
    {
      continue;
    }

    const uint32_t chosen = best[stream.below(uint32_t(best.size()))];
    floor.doors.push_back(CellDoor{chosen, pair.first, pair.second});
    grid.add(used, grid.edgeCell(chosen));
    grid.add(used, grid.edgeCell(grid.twin(chosen)));
  }

  // The stair as the only bridge.
  //
  // A stair joins one room and nothing else - unless the rooms on its other
  // side can be reached no other way, in which case a stair with a door on
  // each side is not a crossroads, it is a through-stair, and the alternative
  // is a plan refused for a room nobody can walk to. So the graph is asked
  // whether it is whole, and while it is not and the reserved room touches an
  // unreached part, it takes one more door into that part through the wall it
  // shares most of.
  if (reserved != UINT32_MAX)
  {
    for (uint32_t extra = 0; extra < 4; ++extra)
    {
      std::vector<std::vector<uint32_t>> links(n);

      for (const CellDoor &door : floor.doors)
      {
        links[door.a].push_back(door.b);
        links[door.b].push_back(door.a);
      }

      std::vector<uint8_t> seen(n, 0);
      std::vector<uint32_t> stack{reserved};
      seen[reserved] = 1;

      while (!stack.empty())
      {
        const uint32_t at = stack.back();
        stack.pop_back();

        for (const uint32_t next : links[at])
        {
          if (seen[next] == 0)
          {
            seen[next] = 1;
            stack.push_back(next);
          }
        }
      }

      std::map<uint32_t, std::vector<uint32_t>> beyond;

      for (const uint32_t e : floor.roomContours[reserved])
      {
        const uint32_t twin = grid.twin(e);

        if (twin == kNoEdge)
        {
          continue;
        }

        const uint32_t b = roomAt(grid, floor, grid.edgeCell(twin));

        if (b != UINT32_MAX && b != reserved && seen[b] == 0)
        {
          beyond[b].push_back(e);
        }
      }

      if (beyond.empty())
      {
        break;
      }

      uint32_t into = UINT32_MAX;
      size_t most = 0;

      for (const auto &[room, walls] : beyond)
      {
        if (walls.size() > most)
        {
          most = walls.size();
          into = room;
        }
      }

      const std::vector<uint32_t> &walls = beyond[into];
      const uint32_t chosen = walls[walls.size() / 2];
      floor.doors.push_back(CellDoor{chosen, reserved, into});
      grid.add(used, grid.edgeCell(chosen));
      grid.add(used, grid.edgeCell(grid.twin(chosen)));
    }
  }

  if (!prune)
  {
    return true;
  }

  // Take back the doors a cheaper two-hop route already serves.
  //
  // Only rooms adjacent to BOTH ends can be that route, so the search runs over
  // the common neighbours of the pair rather than over every room. And the door
  // is removed one at a time with the graph re-checked, which Watabou does not
  // do: he computes every removal against the original graph and applies them
  // together, which can cut a room off. A plan with an unreachable room passes
  // every other test in this file.
  std::vector<std::vector<uint32_t>> links(n);

  for (const CellDoor &door : floor.doors)
  {
    links[door.a].push_back(door.b);
    links[door.b].push_back(door.a);
  }

  std::vector<double> cost(n, 0);

  for (uint32_t r = 0; r < n; ++r)
  {
    cost[r] = passingCost(grid, floor, r);
  }

  for (size_t k = floor.doors.size(); k-- > 0;)
  {
    const CellDoor door = floor.doors[k];

    // A door between two rooms the SHAPE named is never redundant.
    //
    // The nave opens onto the chancel because that is what a church is, not
    // because it is the cheapest way between them - and the pruner, which
    // prices routes and knows nothing about what the rooms are, was removing it
    // in favour of going round through a transept. Wherever a room's kind was
    // settled by where it stands rather than by the programme, the doors
    // between such rooms are part of the same decision.
    const bool named = door.a < floor.roomFixed.size() && floor.roomFixed[door.a] != 0 &&
                       door.b < floor.roomFixed.size() && floor.roomFixed[door.b] != 0;

    if (named)
    {
      continue;
    }

    const double direct = cost[door.a] + cost[door.b];
    bool redundant = false;

    for (const uint32_t through : links[door.a])
    {
      if (through == door.b)
      {
        continue;
      }

      if (std::find(links[door.b].begin(), links[door.b].end(), through) == links[door.b].end())
      {
        continue;
      }

      if (cost[door.a] + cost[through] < direct && cost[through] + cost[door.b] < direct)
      {
        redundant = true;
        break;
      }
    }

    if (!redundant)
    {
      continue;
    }

    const CellDoor removed = floor.doors[k];
    floor.doors.erase(floor.doors.begin() + long(k));

    if (roomsConnected(floor))
    {
      links[removed.a].erase(std::find(links[removed.a].begin(), links[removed.a].end(), removed.b));
      links[removed.b].erase(std::find(links[removed.b].begin(), links[removed.b].end(), removed.a));
    }
    else
    {
      floor.doors.insert(floor.doors.begin() + long(k), removed);
    }
  }

  (void)error;

  return true;
}

bool planDoors(CellHouse &house,
               const CellDwellingSettings &settings,
               Stream &stream,
               Ledger &ledger,
               std::string *error)
{
  std::vector<CellFloor *> all;

  for (CellFloor &floor : house.floors)
  {
    all.push_back(&floor);
  }

  if (house.basement.has_value())
  {
    all.push_back(&house.basement.value());
  }

  for (CellFloor *floor : all)
  {
    if (!mergeCorridors(house.grid, house, *floor, error))
    {
      return false;
    }

    if (!connectRooms(house.grid, house, *floor, settings.pruneRedundantDoors, stream, error))
    {
      return false;
    }

    // A yard has one door from the house.
    //
    // The ordinary rule gives a door to every pair of rooms that share a wall,
    // and a yard shares a wall with every room along the back of the house -
    // so it got three or four, each an open arch, and the back of the house
    // read as an arcade. A yard is a leaf of the plan: nothing is reached
    // THROUGH it, so one door is all it needs, and the widest wall it shares
    // is where that door goes. A court is the opposite case - a castle's court
    // is what its ranges are reached through - and keeps every door it has;
    // the two are told apart by whether the outdoor room has a side open to
    // the lot.
    for (uint32_t r = 0; r < uint32_t(floor->roomAreas.size()); ++r)
    {
      if (r >= floor->roomOutdoor.size() || floor->roomOutdoor[r] == 0)
      {
        continue;
      }

      bool open = false;

      for (const uint32_t e : floor->roomContours[r])
      {
        const uint32_t twin = house.grid.twin(e);
        const uint32_t beyond = twin == kNoEdge ? kNoCell : house.grid.edgeCell(twin);

        if (beyond == kNoCell ||
            (!house.grid.contains(floor->area, beyond) && !house.grid.contains(house.courts, beyond)))
        {
          open = true;
          break;
        }
      }

      if (!open)
      {
        continue;
      }

      // The door onto the room the yard shares the most wall with.
      std::map<uint32_t, uint32_t> shared;

      for (const uint32_t e : floor->roomContours[r])
      {
        const uint32_t twin = house.grid.twin(e);

        if (twin == kNoEdge)
        {
          continue;
        }

        const uint32_t beyond = house.grid.edgeCell(twin);

        for (uint32_t q = 0; q < uint32_t(floor->roomAreas.size()); ++q)
        {
          if (q != r && beyond != kNoCell && house.grid.contains(floor->roomAreas[q], beyond))
          {
            ++shared[q];
          }
        }
      }

      uint32_t keepWith = UINT32_MAX;
      uint32_t most = 0;

      for (const CellDoor &door : floor->doors)
      {
        if (door.a != r && door.b != r)
        {
          continue;
        }

        const uint32_t other = door.a == r ? door.b : door.a;

        if (shared[other] > most)
        {
          most = shared[other];
          keepWith = other;
        }
      }

      if (keepWith == UINT32_MAX)
      {
        continue;
      }

      // Taken away one at a time, and only while the floor stays whole. A yard
      // that reaches round the back of a house can be the only way between
      // two wings of it, and a court open on one side is exactly that on a
      // castle: pruning those to one door refused every such plan for a room
      // nobody could walk to.
      for (size_t k = floor->doors.size(); k-- > 0;)
      {
        const CellDoor door = floor->doors[k];

        if (door.a != r && door.b != r)
        {
          continue;
        }

        const uint32_t other = door.a == r ? door.b : door.a;

        if (other == keepWith)
        {
          continue;
        }

        std::vector<CellDoor> fewer = floor->doors;
        fewer.erase(fewer.begin() + long(k));

        CellFloor trial = *floor;
        trial.doors = fewer;

        if (roomsConnected(trial))
        {
          floor->doors = std::move(fewer);
        }
      }
    }

    if (!roomsConnected(*floor))
    {
      ledger.add(LedgerKind::Rejected, 0, "a floor has a room that cannot be walked to");

      return fail(error, "a floor has a room that cannot be walked to");
    }
  }

  return true;
}

} // namespace procedural::world::generator
