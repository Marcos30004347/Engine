#include "WorldFinalPatches.hpp"

#include "procedural/world/finalpatches/FreezeStage.hpp"

namespace procedural::world::rewrite {

// Both entry points are now orchestrators. The audit is eight small classes in
// finalpatches/PatchAudit, one per claim about the product; the freeze is six
// in finalpatches/FreezeStage, one per step of the pipeline. What used to be
// three hundred lines of straight-line code is a list of named things, each of
// which can be read, tested or skipped on its own.

bool auditWorldFinalPatches(const WorldFinalPatches &patches, std::string *error)
{
  for (const auto &check : finalpatches::worldPatchChecks())
  {
    const std::string fault = check->fault(patches);

    if (!fault.empty())
    {
      if (error != nullptr)
      {
        // The check names itself, so a failure says which claim broke as well
        // as how - which the old flat function could not.
        *error = std::string(check->name()) + ": " + fault;
      }

      return false;
    }
  }

  return true;
}

bool freezeWorldPatches(
    std::vector<WorldContourBatch> batches,
    SurfaceMapSettings settings,
    const SurfaceRefinement &refinement,
    const std::function<double(double, double)> &terrain,
    uint64_t revision,
    WorldFinalPatches &out,
    std::string *error)
{
  finalpatches::FreezeBuild build{std::move(batches), std::move(settings), refinement, terrain, revision, {}, {}};

  for (const auto &stage : finalpatches::freezePipeline())
  {
    if (!stage->run(build, error))
    {
      return false;
    }
  }

  // Published only after the audit passes: a product that cannot be proven
  // sound is not handed on, whatever produced it.
  if (!auditWorldFinalPatches(build.result, error))
  {
    return false;
  }

  out = std::move(build.result);

  return true;
}

} // namespace procedural::world::rewrite
