#pragma once
#include "WorldFinalPatches.hpp"
#include "WorldSplineLandforms.hpp"

namespace procedural::world::rewrite {
struct SurfaceErrorSettings
{
  double heightMetres = .1, normalDegrees = 20;
  double derivativeStepMetres = .001;
  uint32_t maximumPasses = 16, maximumAddedVertices = 20000;
};

struct SurfaceFeatureSample
{
  uint64_t face = 0;
  Position position;
};

struct RefinedWorldSurfaces
{
  WorldFinalPatches world;
  std::vector<double> triangleHeightErrors, triangleNormalErrors;
  double maximumHeightError = 0, maximumNormalError = 0;
  uint32_t addedVertices = 0, passes = 0;
};

using PatchSurface = std::function<double(const SurfaceMapFace &, double, double)>;
// Samples triangle interiors and edges, plus mandatory feature locations. Error bounds
// are measured at probes, not a mathematical bound for arbitrary user functions.
// Frozen boundaries cannot move: insufficient boundary sampling rejects the build.
bool refineWorldSurfaces(const WorldFinalPatches &, const PatchSurface &, const std::vector<SurfaceFeatureSample> &, const SurfaceErrorSettings &, RefinedWorldSurfaces &, std::string *error = nullptr);

struct AssembledSurfaceMesh
{
  SplineFaceMesh geometry; // Per-triangle vertices/normals retain hard creases.
  std::vector<uint32_t> materials;
  uint32_t openEdges = 0, sharedEdges = 0;
};

// Append explicit vertical faces, preserving their supplied outward orientation.
// Closed fixtures require zero open edges; open world/water products opt in explicitly.
bool assembleSurfaceMesh(const RefinedWorldSurfaces &, const SplineFaceMesh &structures, uint32_t structuralMaterial, bool allowOpenBoundaries, AssembledSurfaceMesh &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
