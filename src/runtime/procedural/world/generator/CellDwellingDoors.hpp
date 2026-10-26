#pragma once

#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <string>

namespace procedural::world::generator {

// Corridors, doors, and whether you can walk from one to another.
//
// Watabou has no circulation planner. Corridors appear because the subdivision
// can be told to reward pieces too narrow to be rooms, those pieces are then
// merged end to end, and the door pricing routes traffic through them because
// a corridor costs almost nothing to pass through. Three local rules, and a
// plan of rooms off a hallway comes out of them.
//
// The invariant that matters here is the last one: every room reachable from
// every other. A disconnected room graph is a room you cannot walk to, it is
// invisible in any render, and every other check in this file passes on it.

// Merges the floor's corridor rooms end to end.
//
// Two corridors that meet end-on share exactly ONE contour edge; two that run
// alongside each other share many. That single test is the whole rule, and it
// is why `== 1` rather than `>= 1`: the second form fuses parallel corridors
// into a blob and loses the thing that made them corridors.
bool mergeCorridors(const CellGrid &, const CellHouse &, CellFloor &, std::string *error = nullptr);

// Cuts a door between every pair of rooms that share a wall, then takes back
// the ones a cheaper route already serves.
bool connectRooms(const CellGrid &, const CellHouse &, CellFloor &, bool prune, Stream &, std::string *error = nullptr);

// Both, on every floor.
bool planDoors(CellHouse &, const CellDwellingSettings &, Stream &, Ledger &, std::string *error = nullptr);

// Whether every room on the floor can be reached from every other through its
// doors.
bool roomsConnected(const CellFloor &);

} // namespace procedural::world::generator
