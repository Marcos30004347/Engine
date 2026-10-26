#pragma once
#include "WorldRuntimePublication.hpp"
namespace procedural::world::rewrite
{
struct RuntimeBuildInput
{
  uint64_t revision = 1, samplingSeed = 1; // Geometry randomness is independent of edit revision.
  RuntimeDependencies dependencies;
  std::vector<WorldContourBatch> producers;
  SurfaceMapSettings map;
  std::function<double(double, double)> terrain;
  PatchSurface surface; // Empty: terrain callback or owning engineered plane.
  std::vector<SurfaceFeatureSample> features;
  std::vector<SurfaceErrorSettings> errors;
  SurfaceRefinement initial; // Freeze-time interior sampling; defaults suit fixture-sized domains.
  std::vector<SurfaceRefinement> densities;
  std::vector<CoverDefinition> covers;
  CoverDetailSettings cover;
  double coverSpacingMetres = 1, coverFootprintMetres = 1;
  SplineFaceMesh structures;
  std::vector<uint32_t> structuralMaterials;
};
struct RuntimeBuildResult
{
  RuntimeWorldAssets assets;
  std::vector<uint32_t> changedTiles;
  bool structuresChanged = false;
};
// Rebuild actual contours/CDT/height/cover products as one candidate, never mutate
// the current snapshot. Changed tiles identify payload invalidation for consumers;
// generation is conservatively global (not an incremental solver).
bool buildRuntimeWorld(const RuntimeBuildInput &, const RuntimeWorldAssets *previous, RuntimeBuildResult &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
