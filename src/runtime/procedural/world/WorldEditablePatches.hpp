#pragma once
#include "WorldGenerationContracts.hpp"
#include <map>

namespace procedural::world::rewrite {
struct PatchBounds
{
  double minX = 0, minZ = 0, maxX = 0, maxZ = 0;
};
enum class PatchLandUse
{
  Unassigned,
  Farm,
  Urban,
  Protected
};

struct EditablePatch
{
  uint64_t id = 0, owner = 0, biome = 0, revision = 1;
  PatchLandUse use = PatchLandUse::Unassigned;
  std::vector<uint64_t> vertices; // Shared persistent vertices, CCW convex domain.
  bool occupied = false;
  uint64_t parent = 0; // Containing patch, zero at root. Children must tile the parent.
};

struct EditablePatchSet
{
  uint64_t revision = 1;
  std::map<uint64_t, Position> vertices;
  std::map<uint64_t, EditablePatch> patches;
};

struct PatchEditImpact
{
  std::vector<uint64_t> patches;
  PatchBounds envelope; // Union of complete old/new incident domains, not just vertex sweep.
};

struct PatchSpatialRead
{
  std::string invocation;
  uint64_t domain = 0;
  PatchBounds bounds; // Register even if the query returned no objects.
};

// Convex hierarchical subdivision with explicit shared IDs. Parent edits deform
// descendants through a deterministic piecewise-affine parent cage; invalid
// deformation is rejected. Split/merge and holes are separate topology operations.
bool validateEditablePatches(const EditablePatchSet &, std::string *error = nullptr);
bool movePatchVertex(const EditablePatchSet &, uint64_t vertex, Position, uint64_t expectedRevision, EditablePatchSet &, PatchEditImpact &, std::string *error = nullptr);
// Occupied/protected conversions are refused until an explicit relocation/policy transaction exists.
bool assignPatchLandUse(const EditablePatchSet &, uint64_t patch, PatchLandUse, uint64_t expectedRevision, EditablePatchSet &, PatchEditImpact &, std::string *error = nullptr);
std::vector<std::pair<std::string, uint64_t>> patchSpatialReaders(const std::vector<PatchSpatialRead> &, const PatchEditImpact &);
} // namespace procedural::world::rewrite
