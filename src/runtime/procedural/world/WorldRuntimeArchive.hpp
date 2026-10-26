#pragma once
#include "WorldRuntimePublication.hpp"
namespace procedural::world::rewrite
{
// Portable, versioned complete compiled assets. Does not serialize source callbacks.
// Decode is bounded, validates topology/versions/physical LODs, and publishes atomically.
bool saveRuntimeArchive(const RuntimeSnapshot &, std::string &, std::string *error = nullptr);
bool loadRuntimeArchive(const std::string &, RuntimeSnapshot &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
