#pragma once

#include "procedural/world/WorldDwellingPlan.hpp"

#include <string>

namespace procedural::world::generator {

// Dwelling planner following the recorded Watabou Dwellings trace and the
// engine's dwelling study: reserve the entrance hall and a compact dog-leg
// stair core, cut the remaining plate into rooms with bounded recursive
// guillotine cuts, connect rooms through doors on shared walls from the hall
// outward, then assign purposes by programme. Straight walls in the wing's
// local frame; up to three floors. Fails explicitly instead of forcing rooms.
bool planDwelling(const rewrite::DwellingBrief &brief, rewrite::DwellingPlan &plan, std::string *error = nullptr);

} // namespace procedural::world::generator
