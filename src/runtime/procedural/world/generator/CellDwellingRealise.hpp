#pragma once

#include "procedural/world/WorldDwellingPlan.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <string>

namespace procedural::world::generator {

// Cells into metres: a `CellHouse` emitted as the plan type every other planner
// here already produces.
//
// Deliberately the same output type as `planComplexDwelling`. The planner is
// new; the product is not, so the renderers, the mesh validation and everything
// downstream of a building work on this from the first phase and never learn
// which planner made it.
//
// At this phase it fills the storeys and the floor slabs, applies the jetty,
// and builds the balcony. Rooms, openings, stairs and roofs arrive with the
// phases that decide them.
bool realiseCellDwelling(const CellHouse &, const CellDwellingSettings &, rewrite::DwellingPlan &, std::string *error = nullptr);

} // namespace procedural::world::generator
