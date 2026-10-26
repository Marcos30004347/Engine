#pragma once
#include "procedural/world/generator/CellDwellingPlanner.hpp"

namespace procedural::world::generator {
// Reserves one straight, through-going passage, atomically. The first upper
// floor receives its occupied cells; room subdivision must run afterward.
bool reserveCoveredPassage(const CoveredPassageRequest &,CellHouse &,std::string *error=nullptr);
// Shared lattice AND elevation, not merely equal architectural styles.
bool passageGridsAlign(const geom2::CellFrame &,double groundA,double storeyA,
                       const geom2::CellFrame &,double groundB,double storeyB);
}
