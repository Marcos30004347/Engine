#pragma once
#include "WorldPatchMesh.hpp"
namespace procedural::world::rewrite
{
enum class SurfaceRole : uint8_t
{
  Ground,
  Road,
  Shoulder,
  Cut,
  Fill,
  RiverBed,
  RiverBank,
  Water,
  Bridge,
  Ditch
};
struct SurfacePlane
{
  double x = 0, z = 0, constant = 0;
  double height(double px, double pz) const
  {
    return x * px + z * pz + constant;
  }
};
struct SurfaceContour
{
  uint64_t id = 0;
  uint32_t material = 0, layer = 0;
  SurfaceRole role = SurfaceRole::Ground;
  bool terrainSurface = true;
  SurfacePlane plane;
  std::vector<Position> loop; // Convex CCW canonical polyline; height comes from surface.
};
struct SurfaceMapSettings
{
  double minX = 0, minZ = 0, maxX = 120, maxZ = 80;
  double seamToleranceMetres = .001;
  uint32_t maximumFaces = 12000;
  // Global tile cuts become shared constraints before any patch is triangulated.
  std::vector<double> cutsX, cutsZ;
};
struct SurfaceMapFace
{
  uint64_t id = 0, sourceContour = 0;
  uint32_t material = 0, layer = 0;
  SurfaceRole role = SurfaceRole::Ground;
  bool terrainSurface = true;
  SurfacePlane plane;
  std::vector<uint32_t> vertices;
};
struct SurfaceMapEdge
{
  uint64_t id = 0;
  uint32_t a = 0, b = 0, layer = 0;
  uint64_t leftFace = 0, rightFace = 0;
  std::vector<uint64_t> sourceContours;
  bool materialBoundary = false, surfaceBreak = false;
};
struct SurfaceArrangement
{
  uint64_t revision = 0;
  std::vector<Position> vertices;
  std::vector<SurfaceMapFace> faces;
  std::vector<SurfaceMapEdge> edges;
  SurfaceMapSettings settings;
};
// Reconcile contour intersections, walk the shared edge graph, and label faces before CDT.
// Layer 0 covers the entire rectangle; higher layers cover only authored contours.
// Every input polygon is convex; nonconvex outlines can be supplied as convex pieces.
bool arrangeSurfaceContours(const std::vector<SurfaceContour> &, const SurfaceMapSettings &, const std::function<double(double, double)> &terrain, uint64_t revision, SurfaceArrangement &, std::string *error = nullptr);
struct SurfaceRefinement
{
  double groundSpacingMetres = 12, modifiedSpacingMetres = 3;
  uint32_t maximumInteriorPoints = 20000;
  uint64_t samplingSeed = 0; // Zero keeps legacy revision-seeded sampling; runtime recipes supply a stable seed.
};
struct SurfaceTriangle
{
  std::array<uint32_t, 3> vertices;
  uint64_t face = 0;
  uint32_t material = 0, layer = 0;
  SurfaceRole role = SurfaceRole::Ground;
};
struct SurfaceArrangementMesh
{
  std::vector<Position> vertices;
  std::vector<SurfaceTriangle> triangles;
  std::vector<PatchMesh> patches;
  double groundArea = 0;
};
// Mesh faces and their canonical edges without inferring materials from the raster.
bool meshSurfaceArrangement(const SurfaceArrangement &, const std::function<double(double, double)> &terrain, const SurfaceRefinement &, SurfaceArrangementMesh &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
