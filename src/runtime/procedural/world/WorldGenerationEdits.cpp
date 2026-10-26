#include "WorldGenerationEdits.hpp"
#include <set>
namespace procedural::world::rewrite
{
bool editGeneratedWorld(
    const PatchGenerationEdit &edit,
    uint64_t expectedGeometry,
    uint64_t expectedGeneration,
    uint64_t seed,
    const PatchGenerationFactory &factory,
    EditableGenerationWorld &world,
    GenerationUpdateReport &report,
    const std::function<bool(const EditableGenerationWorld &, std::string *)> &validatePublication,
    const std::function<bool()> &cancelled,
    std::string *error,
    uint32_t maximumWorkers)
{
  auto fail = [&](const char *message)
  {
    if (error)
      *error = message;
    return false;
  };
  if (expectedGeometry != world.geometry.revision || expectedGeneration != world.generation.revision)
    return fail("stale combined world edit");
  if (!factory)
    return fail("missing patch generation factory");
  try
  {
    auto candidate = world;
    PatchEditImpact impact;
    if (edit.kind == PatchGenerationEdit::Kind::MoveVertex)
    {
      if (!movePatchVertex(world.geometry, edit.id, edit.position, expectedGeometry, candidate.geometry, impact, error))
        return false;
    }
    else if (edit.kind == PatchGenerationEdit::Kind::AssignLandUse)
    {
      if (!assignPatchLandUse(world.geometry, edit.id, edit.use, expectedGeometry, candidate.geometry, impact, error))
        return false;
    }
    else if (edit.kind == PatchGenerationEdit::Kind::Rebuild)
    {
      if (!validateEditablePatches(world.geometry, error))
        return false;
    }
    else
      return fail("unknown patch edit");
    PatchGenerationBuild build;
    if (!factory(candidate.geometry, build, error))
      return false;
    std::set<std::pair<std::string, uint64_t>> current;
    for (auto &t : build.tasks)
      current.emplace(t.descriptor.id, t.descriptor.domain);
    std::vector<std::pair<std::string, uint64_t>> dirty;
    auto reads = world.spatialReads;
    reads.insert(reads.end(), build.spatialReads.begin(), build.spatialReads.end());
    if (!impact.patches.empty())
      for (auto id : patchSpatialReaders(reads, impact))
        if (current.count(id))
          dirty.push_back(id);
    candidate.spatialReads = build.spatialReads;
    GenerationUpdateReport result;
    if (!updateGenerationSession(build.tasks, build.inputs, seed, expectedGeneration, dirty, candidate.generation, result, cancelled, error, maximumWorkers))
      return false;
    if (validatePublication && !validatePublication(candidate, error))
      return false;
    if (cancelled && cancelled())
      return fail("world edit cancelled before publication");
    world = std::move(candidate);
    report = std::move(result);
    if (error)
      error->clear();
    return true;
  }
  catch (const std::exception &e)
  {
    if (error)
      *error = e.what();
    return false;
  }
  catch (...)
  {
    return fail("unknown world edit exception");
  }
}
} // namespace procedural::world::rewrite
