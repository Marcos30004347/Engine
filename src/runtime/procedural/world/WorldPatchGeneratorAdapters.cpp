#include "WorldPatchGeneratorAdapters.hpp"
#include "WorldNativeGenerator.hpp"
namespace procedural::world::rewrite
{
GenerationTask farmFieldsGenerator(uint64_t domain)
{
  return nativeGenerator<FarmGenerationBrief, FarmFieldProduct>(
      domain,
      "farm.fields",
      "farm.brief",
      "farm.fields",
      GenerationScope::Patch,
      [](auto &b, auto &p, auto *e)
      {
        return generateFarmFields(b.geometry, b.settings, p, e);
      },
      true);
}
GenerationTask dwellingPlanGenerator(uint64_t domain)
{
  return nativeGenerator<DwellingBrief, DwellingPlan>(domain, "dwelling.plan", "dwelling.brief", "dwelling.plan", GenerationScope::Patch, generateDwellingPlan, true);
}
GenerationTask dwellingMeshGenerator(uint64_t domain)
{
  return nativeGenerator<DwellingPlan, std::vector<DwellingTriangle>>(domain, "dwelling.mesh", "dwelling.plan", "dwelling.mesh", GenerationScope::Patch, meshDwellingPlan, true);
}
} // namespace procedural::world::rewrite
