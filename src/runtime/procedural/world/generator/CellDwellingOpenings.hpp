#pragma once

#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <string>

namespace procedural::world::generator {

// Windows, door leaves, and the step at the front door.
//
// Windows go in whole RUNS of wall, never one at a time, and that single
// decision is why these facades read as architecture rather than as a wall with
// holes punched in it: a run is a bay, and bays are what an elevation is made
// of. Runs are drawn weighted by their length squared, so a long unbroken wall
// is glazed before a short one.

// Which openings carry a leaf.
//
// Almost none, by default. An open arch is the normal way from one room to the
// next in a building of this age; a door is what a room gets when it is a room
// you would shut - a bedroom, a bathroom, a store - or when it is a dead end.
bool assignDoorKinds(CellHouse &, std::string *error = nullptr);

// Glazes every floor.
bool planWindows(CellHouse &, const CellDwellingSettings &, Stream &, std::string *error = nullptr);

// Both.
bool planOpenings(CellHouse &, const CellDwellingSettings &, Stream &, Ledger &, std::string *error = nullptr);

} // namespace procedural::world::generator
