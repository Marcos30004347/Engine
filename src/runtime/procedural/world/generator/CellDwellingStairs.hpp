#pragma once

#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <string>
#include <utility>

namespace procedural::world::generator {

// Where a flight may stand, and what shape it takes.
//
// This is the half of the stair question that could not move to phase 3: the
// legal cells exclude door landings, so the search needs the doors to exist.
// Everything else about stairs - which scheme, where the turret goes, which
// cell the stairwell reserves - was settled before any room was cut.

// Every cell of the floor a flight could stand in.
//
// Five exclusions, and all five are about not breaking circulation: never a
// door landing, never the entrance or the spiral landing, never a cell of a
// one-wide passage (a flight there seals the corridor), and never a re-entrant
// corner - two cardinals open with the diagonal between them closed, which is
// a cell you have to walk round rather than through.
Board stairCells(const CellGrid &, const CellHouse &, const CellFloor &);

// Puts a flight between two consecutive floors.
//
// Watabou takes any cell legal on both floors and lets the form fall out of two
// independently chosen directions. We choose the pair, because the form decides
// the RUN and the run decides whether the flight can be walked up: a straight
// flight has one cell of it and a turning flight has two, and at our cell size
// the straight one does not fit a domestic storey. A trapdoor is the honest
// answer when nothing else is, and it is recorded rather than hidden.
bool linkFloors(const CellGrid &, const CellHouse &, CellFloor &lower, CellFloor &upper, const CellDwellingSettings &, Stream &, Ledger &);

// How far in from a well's cell edges its flights and landings stay: half
// the thickest wall and a hair. The blueprint frames the well by it.
double wellMargin(const CellDwellingSettings &);

// Length/width including level end landings and a lane beside the flight.
std::pair<uint32_t,uint32_t> straightStairRoom(double rise,double cellMetres,const CellDwellingSettings &);

// Every floor linked to the next, and the basement to the ground.
bool planStairs(CellHouse &, const CellDwellingSettings &, Stream &, Ledger &, std::string *error = nullptr);

} // namespace procedural::world::generator
