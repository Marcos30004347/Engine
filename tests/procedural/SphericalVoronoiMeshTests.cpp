#include "procedural/SphericalVoronoiMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  if (condition)
    std::cout << "  PASSED " << message << "\n";
  else
  {
    std::cerr << "  FAILED " << message << "\n";
    ++failures;
  }
}

double dot(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

procedural::SpherePoint subtract(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return procedural::SpherePoint{a.x - b.x, a.y - b.y, a.z - b.z};
}

procedural::SpherePoint cross(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return procedural::SpherePoint{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

uint64_t edgeKey(const uint32_t a, const uint32_t b)
{
  const uint32_t lo = std::min(a, b);
  const uint32_t hi = std::max(a, b);
  return (static_cast<uint64_t>(lo) << 32u) | hi;
}

procedural::SphericalVoronoiMesh makeMesh(const uint64_t seed)
{
  procedural::SphericalVoronoiMeshSettings settings{};
  settings.seed = seed;
  settings.regionCount = 512u;
  settings.jitter = 0.75;
  procedural::SphericalVoronoiMesh mesh{};
  procedural::SphericalVoronoiMeshGenerator generator;
  check(generator.generate(settings, mesh), "spherical Voronoi mesh generation succeeds");
  return mesh;
}

void testMeshShapeAndClosure()
{
  std::cout << "spherical Voronoi mesh shape and closure\n";
  const procedural::SphericalVoronoiMesh mesh = makeMesh(1234u);
  check(mesh.regions.size() == 513u, "mesh includes requested regions plus stitched projection pole");
  check(mesh.triangles.size() > mesh.regions.size(), "mesh has a non-trivial Delaunay triangle set");
  check(mesh.triangleCenters.size() == mesh.triangles.size(), "every triangle has a center");
  check(mesh.regionNeighbours.size() == mesh.regions.size(), "every region has a neighbour list");
  check(mesh.regionTriangles.size() == mesh.regions.size(), "every region has Voronoi corner triangle references");

  std::map<uint64_t, uint32_t> edgeCounts;
  for (const auto &triangle : mesh.triangles)
  {
    for (uint32_t corner = 0u; corner < 3u; ++corner)
      ++edgeCounts[edgeKey(triangle[corner], triangle[(corner + 1u) % 3u])];
  }
  check(
      std::all_of(
          edgeCounts.begin(),
          edgeCounts.end(),
          [](const auto &entry)
          {
            return entry.second == 2u;
          }),
      "stitched sphere has no boundary edges");
}

void testOutwardWindingAndUnitCenters()
{
  std::cout << "spherical Voronoi mesh geometry\n";
  const procedural::SphericalVoronoiMesh mesh = makeMesh(5678u);
  bool allOutward = true;
  for (const auto &triangle : mesh.triangles)
  {
    const auto &a = mesh.regions[triangle[0]];
    const auto &b = mesh.regions[triangle[1]];
    const auto &c = mesh.regions[triangle[2]];
    const procedural::SpherePoint normal = cross(subtract(b, a), subtract(c, a));
    const procedural::SpherePoint center{a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z};
    if (dot(normal, center) <= 0.0)
      allOutward = false;
  }
  check(allOutward, "all triangles have outward winding");
  check(
      std::all_of(
          mesh.triangleCenters.begin(),
          mesh.triangleCenters.end(),
          [](const procedural::SpherePoint &point)
          {
            const double length = std::sqrt(dot(point, point));
            return std::abs(length - 1.0) < 1.0e-6;
          }),
      "triangle centers lie on the sphere");
}

void testRegionTopologyAndDeterminism()
{
  std::cout << "spherical Voronoi mesh topology and determinism\n";
  const procedural::SphericalVoronoiMesh first = makeMesh(2468u);
  const procedural::SphericalVoronoiMesh second = makeMesh(2468u);
  check(first.regions.size() == second.regions.size(), "deterministic region count");
  check(first.triangles == second.triangles, "deterministic triangle topology");
  check(first.regionNeighbours == second.regionNeighbours, "deterministic region adjacency");
  check(
      std::all_of(
          first.regionNeighbours.begin(),
          first.regionNeighbours.end(),
          [](const std::vector<uint32_t> &neighbours)
          {
            return neighbours.size() >= 3u;
          }),
      "every region has at least three neighbours");
  check(
      std::all_of(
          first.regionTriangles.begin(),
          first.regionTriangles.end(),
          [](const std::vector<uint32_t> &triangles)
          {
            return triangles.size() >= 3u;
          }),
      "every Voronoi region has at least three corners");
}

} // namespace

int main()
{
  testMeshShapeAndClosure();
  testOutwardWindingAndUnitCenters();
  testRegionTopologyAndDeterminism();
  return failures == 0 ? 0 : 1;
}
