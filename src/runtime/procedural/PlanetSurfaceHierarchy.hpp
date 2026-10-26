#pragma once

#include "procedural/SphericalVoronoiMesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace procedural
{

// Stable newest-vertex-bisection address. The CBT memory-pool slot is
// intentionally not part of the identity: slots are recycled as patches split
// and merge, while (rootHalfedge, depth, path) remains stable across frames.
struct PlanetSurfaceBisectorAddress
{
  uint32_t rootHalfedge = 0u;
  uint32_t depth = 0u;
  uint64_t path = 0u;
};

struct PlanetSurfaceHalfedgeMesh
{
  static constexpr uint32_t noHalfedge = 0xffffffffu;

  std::vector<SpherePoint> vertices;
  std::vector<uint32_t> vertex;
  std::vector<uint32_t> next;
  std::vector<uint32_t> previous;
  std::vector<uint32_t> twin;
  std::vector<uint32_t> face;
  std::vector<SpherePoint> faceCentres;
};

struct PlanetSurfaceTriangle
{
  std::array<SpherePoint, 3> vertices{};
};

struct PlanetSurfaceAtlasView
{
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t tileSizePixels = 0u;
  const uint16_t *heightmap = nullptr;
  size_t heightCount = 0u;
  const uint8_t *materials = nullptr;
  size_t materialCount = 0u;
  const uint32_t *sourceRegions = nullptr;
  size_t sourceRegionCount = 0u;
};

struct PlanetSurfaceSample
{
  double u = 0.0;
  double v = 0.0;
  uint32_t texelX = 0u;
  uint32_t texelY = 0u;
  uint32_t tileColumn = 0u;
  uint32_t tileRow = 0u;
  uint32_t tileLocalX = 0u;
  uint32_t tileLocalY = 0u;
  uint32_t sourceRegion = 0u;
  uint16_t height = 0u;
  uint8_t material = 0u;
};

bool buildPlanetSurfaceHalfedgeMesh(
    const std::vector<SpherePoint> &vertices,
    const std::vector<std::array<uint32_t, 3>> &triangles,
    PlanetSurfaceHalfedgeMesh &mesh);
bool validatePlanetSurfaceHalfedgeMesh(const PlanetSurfaceHalfedgeMesh &mesh);

bool validPlanetSurfaceBisectorAddress(const PlanetSurfaceBisectorAddress &address);
PlanetSurfaceBisectorAddress planetSurfaceBisectorChild(const PlanetSurfaceBisectorAddress &parent, uint32_t child);
PlanetSurfaceBisectorAddress planetSurfaceBisectorParent(const PlanetSurfaceBisectorAddress &child);
// Produces the stable fixed-depth prefix used as a streaming chunk/cache key.
// A render leaf may be deeper than its chunk; all descendants share this key.
bool planetSurfaceBisectorAncestor(
    const PlanetSurfaceBisectorAddress &address,
    uint32_t ancestorDepth,
    PlanetSurfaceBisectorAddress &ancestor);
bool planetSurfaceBisectorTriangle(
    const PlanetSurfaceHalfedgeMesh &mesh,
    const PlanetSurfaceBisectorAddress &address,
    PlanetSurfaceTriangle &triangle);

// Samples the baked planet products. Height is bilinear; discrete material and
// source-region IDs use nearest sampling so identities are never interpolated.
bool samplePlanetSurfaceAtlas(
    const PlanetSurfaceAtlasView &atlas,
    const SpherePoint &direction,
    PlanetSurfaceSample &sample);

} // namespace procedural
