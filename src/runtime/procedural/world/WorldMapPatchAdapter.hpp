#pragma once
#include "MapGraph.hpp"
#include "WorldEditablePatches.hpp"
namespace procedural::world::rewrite
{
// Imports an existing hierarchy once. Assigned IDs persist in the editable
// snapshot; source node indices are not recomputed after edits. Geometry is
// welded at sub-millimetre precision across all hierarchy levels.
bool importWorldPatches(const procedural::world::MapGraph &, EditablePatchSet &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
