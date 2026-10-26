#include "procedural/world/generator/CellDwellingOpenings.hpp"

#include "procedural/geom2/CellShape.hpp"
#include "procedural/world/generator/CellDwellingProgramme.hpp"

#include <algorithm>
#include <iterator>
#include <cmath>

namespace procedural::world::generator {

using geom2::Board;
using geom2::Chain;
using geom2::Dir;
using geom2::kNoEdge;

namespace {

// A stretch of one room's wall, running one way, on the outside of the
// building. The unit a window is placed in.
struct Run
{
  std::vector<uint32_t> edges;
  uint32_t room = UINT32_MAX;
};

// Whether an edge may ever carry a window: on the outer wall, and not one of
// the ways in.
bool glazeable(const CellHouse &house, const CellFloor &floor, const uint32_t e)
{
  if (e == house.entrance)
  {
    return false;
  }

  if (house.spiral.has_value() && (e == house.spiral->entrance || e == house.spiral->exit))
  {
    return false;
  }

  // A party wall has a neighbour's house on the other side of it.
  if (std::binary_search(house.partyEdges.begin(), house.partyEdges.end(), e))
  {
    return false;
  }

  // Nor a wall that already has a door in it. One edge is one wall of one
  // cell, and a door and a window in two metres of wall is a door with a
  // window beside it that nobody could open; the way out into a yard is on an
  // outside edge and was being glazed as well as hung.
  for (const CellDoor &door : floor.doors)
  {
    if (door.edge == e || house.grid.twin(door.edge) == e)
    {
      return false;
    }
  }

  return exteriorEdge(floor, e);
}

// Maximal runs along a contour, broken wherever the direction changes, the
// owning room changes, or the wall stops being outside wall.
std::vector<Run> runsAlong(const CellGrid &grid,
                           const CellHouse &house,
                           const CellFloor &floor,
                           const Chain &contour,
                           const std::vector<uint8_t> &skipRoom)
{
  std::vector<Run> runs;

  if (contour.empty())
  {
    return runs;
  }

  const size_t n = contour.size();
  const size_t start = geom2::firstCorner(grid, contour);

  bool open = false;
  Dir lastDir = Dir::North;
  uint32_t lastRoom = UINT32_MAX;

  for (size_t step = 0; step < n; ++step)
  {
    const uint32_t e = contour[(start + step) % n];

    if (!glazeable(house, floor, e))
    {
      open = false;
      continue;
    }

    uint32_t room = UINT32_MAX;

    for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
    {
      if (grid.contains(floor.roomAreas[r], grid.edgeCell(e)))
      {
        room = r;
        break;
      }
    }

    if (room != UINT32_MAX && room < skipRoom.size() && skipRoom[room] != 0)
    {
      open = false;
      continue;
    }

    const Dir dir = grid.decode(e).dir;

    if (!open || dir != lastDir || room != lastRoom)
    {
      runs.push_back(Run{});
      runs.back().room = room;
      open = true;
    }

    runs.back().edges.push_back(e);
    lastDir = dir;
    lastRoom = room;
  }

  return runs;
}

// Takes whole runs until the target is met, weighted by length squared: a long
// unbroken wall is glazed before a short one, which is what makes the bays line
// up rather than scatter.
void takeRuns(std::vector<Run> &runs, const double target, Stream &stream, std::vector<uint32_t> &out)
{
  while (!runs.empty() && double(out.size()) < target)
  {
    std::vector<uint32_t> indices;
    std::vector<double> weights;

    for (uint32_t k = 0; k < uint32_t(runs.size()); ++k)
    {
      indices.push_back(k);
      const double length = double(runs[k].edges.size());
      weights.push_back(length * length);
    }

    const uint32_t k = stream.weighted(indices, weights);

    for (const uint32_t e : runs[k].edges)
    {
      out.push_back(e);
    }

    runs.erase(runs.begin() + k);
  }
}

} // namespace

bool assignDoorKinds(CellHouse &house, std::string *error)
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
    for (CellDoor &door : floor->doors)
    {
      door.kind = DoorKind::Open;
      door.leaves = 1;
    }

    // The thresholds that matter get a pair of doors.
    //
    // Four of them, and each is a threshold you would actually build wide: the
    // way in from the street, the way out into a court, the way from a nave to
    // a chancel, and the way into the one great room of the storey. A building
    // where every door is the same size has no hierarchy in it, and which door
    // is the important one is exactly the thing a plan is supposed to say.
    {
      uint32_t grandest = uint32_t(floor->roomAreas.size());
      uint32_t widest = 0;

      for (uint32_t r = 0; r < uint32_t(floor->roomAreas.size()); ++r)
      {
        const bool ordinary = (r >= floor->roomOutdoor.size() || floor->roomOutdoor[r] == 0) &&
                              (floor->circulation.empty() || floor->roomAreas[r] != floor->circulation);

        if (ordinary && floor->roomAreas[r].count() > widest)
        {
          widest = floor->roomAreas[r].count();
          grandest = r;
        }
      }

      // A great room is one that is great: a third again the size of the median
      // room, or there is no great room on this floor and nothing is widened
      // for it.
      std::vector<uint32_t> sizes;

      for (const Board &room : floor->roomAreas)
      {
        sizes.push_back(room.count());
      }

      std::sort(sizes.begin(), sizes.end());

      const uint32_t median = sizes.empty() ? 0 : sizes[sizes.size() / 2];

      if (median == 0 || widest * 2 < median * 3)
      {
        grandest = uint32_t(floor->roomAreas.size());
      }

      for (CellDoor &door : floor->doors)
      {
        const auto kindOf = [&](const uint32_t r) {
          return r < floor->roomKinds.size() ? floor->roomKinds[r] : rewrite::RoomKind::Count;
        };

        const auto outdoors = [&](const uint32_t r) {
          return r < floor->roomOutdoor.size() && floor->roomOutdoor[r] != 0;
        };

        const bool ontoACourt = outdoors(door.a) || outdoors(door.b);

        const bool naveToChancel =
          (kindOf(door.a) == rewrite::RoomKind::Nave && kindOf(door.b) == rewrite::RoomKind::Chancel) ||
          (kindOf(door.b) == rewrite::RoomKind::Nave && kindOf(door.a) == rewrite::RoomKind::Chancel);

        const bool intoTheGreatRoom = door.a == grandest || door.b == grandest;

        if (ontoACourt || naveToChancel || intoTheGreatRoom)
        {
          door.leaves = 2;
        }

        // The way out into a yard or a court is a DOOR, and it is shut.
        //
        // Every door starts as an open arch here and is made a leaf only where
        // a room is a dead end, and a yard is never a dead end - it is a room
        // in the graph with the house on one side - so the way out into it was
        // an arch, wide open at ground level on the back of the house, and it
        // read as a window somebody had knocked through to the floor. Weather
        // comes in through an arch. A back door has a leaf.
        if (ontoACourt)
        {
          door.kind = DoorKind::Solid;
        }

        // The chancel arch is central, and it is an arch.
        //
        // A church's one important threshold is the one you look through from
        // the door, so it is not a leaf hung wherever the routing found the
        // cheapest wall - it stands in the MIDDLE of the wall the two rooms
        // share, it has no door in it, and it is as wide as the wall will
        // carry.
        if (naveToChancel)
        {
          std::vector<uint32_t> shared;

          for (const uint32_t e : floor->roomContours[door.a])
          {
            const uint32_t beyond = house.grid.edgeCell(house.grid.twin(e));

            if (beyond != geom2::kNoCell && house.grid.contains(floor->roomAreas[door.b], beyond))
            {
              shared.push_back(e);
            }
          }

          if (!shared.empty())
          {
            door.edge = shared[shared.size() / 2];
            door.kind = DoorKind::Open;
          }
        }
      }
    }

    for (uint32_t r = 0; r < uint32_t(floor->roomAreas.size()); ++r)
    {
      // What the purpose asked for.
      bool solid = r < floor->roomSolidDoors.size() && floor->roomSolidDoors[r] != 0;

      if (!solid && floor->level > -1)
      {
        // ...or a dead end that is not a corridor. A room with one way in and
        // out is a room you would shut.
        //
        // One WAY, not one door: the front door, a flight standing in the room
        // and the turret's landing are all ways in, and a room that has a door
        // plus a staircase is not a dead end. Counting only the room-to-room
        // doors put a leaf on two openings in three.
        const RoomFacts facts = measureRoom(house.grid, house, *floor, r);
        const bool corridor =
          std::find(floor->corridors.begin(), floor->corridors.end(), r) != floor->corridors.end();

        // Castle circulation opens through masonry arches. Explicit private
        // room programmes still keep their solid doors.
        solid = facts.doors == 1 && !corridor && house.building!=BuildingClass::Castle;
      }

      if (!solid)
      {
        continue;
      }

      for (CellDoor &door : floor->doors)
      {
        if (door.a == r || door.b == r)
        {
          door.kind = DoorKind::Solid;
        }
      }
    }
  }

  // The axial sanctuary connection remains open even when the chancel is a
  // dead end in the circulation graph.
  for(auto *floor:all)for(auto &door:floor->doors)if(door.a<floor->roomKinds.size() && door.b<floor->roomKinds.size()) {
    auto a=floor->roomKinds[door.a],b=floor->roomKinds[door.b];
    if((a==rewrite::RoomKind::Nave && b==rewrite::RoomKind::Chancel)||(b==rewrite::RoomKind::Nave && a==rewrite::RoomKind::Chancel))door.kind=DoorKind::Open;
  }

  // Stair circulation is an open doorway on both sides, never a door leaf.
  if (house.stairwell.has_value())
    for (CellFloor *floor : all)
      for (CellDoor &door : floor->doors)
      {
        const auto &block = house.stairwell->block;
        const uint32_t a = house.grid.edgeCell(door.edge);
        const uint32_t twin = house.grid.twin(door.edge);
        const uint32_t b = twin == geom2::kNoEdge ? geom2::kNoCell : house.grid.edgeCell(twin);
        if (house.grid.contains(block, a) != house.grid.contains(block, b))
          door.kind = DoorKind::Open;
      }

  (void)error;

  return true;
}

bool planWindows(CellHouse &house, const CellDwellingSettings &settings, Stream &stream, std::string *error)
{
  const bool blank = std::find(house.tags.begin(), house.tags.end(), "blank") != house.tags.end();
  const bool transparent =
    std::find(house.tags.begin(), house.tags.end(), "transparent") != house.tags.end();

  if (house.basement.has_value())
  {
    house.basement->windows.clear();
  }

  for (CellFloor &floor : house.floors)
  {
    floor.windows.clear();

    if (blank)
    {
      continue;
    }

    // Rooms whose purpose named a ratio are glazed to it, and then left out of
    // the floor's own pass: a greenhouse that asked for every wall must not
    // then be topped up, and an armoury that asked for none must not be glazed
    // behind its own back.
    std::vector<uint8_t> claimed(floor.roomAreas.size(), 0);

    for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
    {
      if (r >= floor.roomWindowRatio.size() || floor.roomWindowRatio[r] < 0)
      {
        continue;
      }

      claimed[r] = 1;

      std::vector<uint8_t> others(floor.roomAreas.size(), 1);
      others[r] = 0;
      std::vector<Run> runs = runsAlong(house.grid, house, floor, floor.roomContours[r], others);
      const double ratio = transparent ? 1. : floor.roomWindowRatio[r];
      takeRuns(runs, std::round(double(floor.roomContours[r].size()) * ratio), stream, floor.windows);
    }

    // Then the rest of the floor, at a density that RISES with height. An
    // upper storey carries less and can afford more glass, and his facades
    // depend on it.
    const double ramp = settings.windowDensity + .1 * double(floor.level) -
                        .1 * double(int32_t(house.floors.size()) - 1);
    const double density = transparent ? 1. : std::clamp(ramp, 0., 1.);

    // Every loop. A range round a court is glazed on both faces, and taking
    // only the outer one leaves the court a blank wall.
    std::vector<Run> runs;

    for (const Chain &loop : floor.loops)
    {
      std::vector<Run> along = runsAlong(house.grid, house, floor, loop, claimed);
      runs.insert(runs.end(), std::make_move_iterator(along.begin()), std::make_move_iterator(along.end()));
    }

    double available = 0;

    for (const Run &run : runs)
    {
      available += double(run.edges.size());
    }

    // Every room with an outside wall gets light before the density is
    // spent: its longest outside run is glazed first. The density draw is
    // weighted by length squared, and on a tall house whose ground storey
    // rates half, it left whole room sides - and whole tower faces - blank
    // while the long walls took every window. A room without a window is
    // a store, and the programme says which rooms those are, not the draw.
    std::vector<uint8_t> lit(floor.roomAreas.size(), 0);

    for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
    {
      if (lit[r] != 0)
      {
        continue;
      }

      size_t best = runs.size();

      for (size_t k = 0; k < runs.size(); ++k)
      {
        if (runs[k].room == r && (best == runs.size() || runs[k].edges.size() > runs[best].edges.size()))
        {
          best = k;
        }
      }

      if (best == runs.size())
      {
        continue;
      }

      for (const uint32_t e : runs[best].edges)
      {
        floor.windows.push_back(e);
      }

      lit[r] = 1;
      runs.erase(runs.begin() + long(best));
    }

    takeRuns(runs, available * density, stream, floor.windows);
  }

  if(settings.maximumWindows>=0)
  {
    size_t remaining=size_t(settings.maximumWindows);
    for(auto &floor:house.floors) {
      if(floor.windows.size()>remaining) floor.windows.resize(remaining);
      remaining-=floor.windows.size();
    }
  }

  (void)error;

  return true;
}

bool planOpenings(CellHouse &house,
                  const CellDwellingSettings &settings,
                  Stream &stream,
                  Ledger &ledger,
                  std::string *error)
{
  if (!assignDoorKinds(house, error))
  {
    return false;
  }

  (void)ledger;

  return planWindows(house, settings, stream, error);
}

} // namespace procedural::world::generator
