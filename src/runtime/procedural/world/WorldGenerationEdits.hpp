#pragma once
#include "WorldEditablePatches.hpp"
#include "WorldGenerationSession.hpp"
namespace procedural::world::rewrite
{
struct EditableGenerationWorld
{
  EditablePatchSet geometry;
  GenerationSession generation;
  std::vector<PatchSpatialRead> spatialReads;
};
struct PatchGenerationBuild
{
  std::vector<GenerationTask> tasks;
  GenerationSnapshot inputs;
  std::vector<PatchSpatialRead> spatialReads;
};
// Factory converts authored geometry/assignments into typed generator inputs.
// It must retain nongeometric context (people, water, roads) explicitly.
using PatchGenerationFactory = std::function<bool(const EditablePatchSet &, PatchGenerationBuild &, std::string *)>;
struct PatchGenerationEdit
{
  enum class Kind
  {
    Rebuild,
    MoveVertex,
    AssignLandUse
  } kind = Kind::Rebuild;
  uint64_t id = 0;
  Position position;
  PatchLandUse use = PatchLandUse::Unassigned;
};
// Commits both authored geometry and all generated products only after every
// generator and the optional final publication validator succeeds. Factories
// cannot publish. A stale edit or failed mesh leaves BOTH snapshots untouched.
bool editGeneratedWorld(
    const PatchGenerationEdit &,
    uint64_t expectedGeometryRevision,
    uint64_t expectedGenerationRevision,
    uint64_t seed,
    const PatchGenerationFactory &,
    EditableGenerationWorld &,
    GenerationUpdateReport &,
    const std::function<bool(const EditableGenerationWorld &, std::string *)> &validatePublication = {},
    const std::function<bool()> &cancelled = {},
    std::string *error = nullptr,
    uint32_t maximumWorkers = 1);
} // namespace procedural::world::rewrite
