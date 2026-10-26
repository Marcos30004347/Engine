#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace procedural
{

struct SpherePoint
{
  double x = 0.0;
  double y = 0.0;
  double z = 1.0;
};

struct SphericalVoronoiMeshSettings
{
  uint64_t seed = 0x6a09e667f3bcc909ull;
  uint32_t regionCount = 4096u;
  double jitter = 0.75;
  uint32_t workerCount = 0u;
};

struct SphericalVoronoiMesh
{
  std::vector<SpherePoint> regions;
  std::vector<SpherePoint> triangleCenters;
  std::vector<std::array<uint32_t, 3>> triangles;
  std::vector<std::vector<uint32_t>> regionNeighbours;
  std::vector<std::vector<uint32_t>> regionTriangles;
};

class SphericalVoronoiMeshGenerator
{
public:
  bool generate(const SphericalVoronoiMeshSettings &settings, SphericalVoronoiMesh &mesh) const;
};

} // namespace procedural
