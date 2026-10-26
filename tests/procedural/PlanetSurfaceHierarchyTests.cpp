#include "procedural/PlanetSurfaceHierarchy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  if (condition)
    std::cout << "  PASSED " << message << '\n';
  else
  {
    std::cerr << "  FAILED " << message << '\n';
    ++failures;
  }
}

procedural::SpherePoint normalized(const double x, const double y, const double z)
{
  const double inverseLength = 1.0 / std::sqrt(x * x + y * y + z * z);
  return {x * inverseLength, y * inverseLength, z * inverseLength};
}

bool samePoint(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return std::abs(a.x - b.x) < 1.0e-12 && std::abs(a.y - b.y) < 1.0e-12 && std::abs(a.z - b.z) < 1.0e-12;
}

void testHalfedgeRootsAndStableAddresses()
{
  std::cout << "planet surface halfedge bisectors\n";
  const std::vector<procedural::SpherePoint> vertices{
    normalized(1, 1, 1), normalized(-1, -1, 1), normalized(-1, 1, -1), normalized(1, -1, -1)};
  const std::vector<std::array<uint32_t, 3>> faces{{{0, 1, 2}}, {{0, 3, 1}}, {{0, 2, 3}}, {{1, 3, 2}}};
  procedural::PlanetSurfaceHalfedgeMesh mesh{};
  check(procedural::buildPlanetSurfaceHalfedgeMesh(vertices, faces, mesh), "closed triangle mesh converts to halfedges");
  check(mesh.vertex.size() == 12u, "one root bisector is allocated per halfedge");
  check(std::none_of(mesh.twin.begin(), mesh.twin.end(), [](const uint32_t twin) { return twin == procedural::PlanetSurfaceHalfedgeMesh::noHalfedge; }), "closed mesh roots all have twins");

  const procedural::PlanetSurfaceBisectorAddress root{0u, 0u, 0u};
  const auto left = procedural::planetSurfaceBisectorChild(root, 0u);
  const auto right = procedural::planetSurfaceBisectorChild(root, 1u);
  const auto rightLeft = procedural::planetSurfaceBisectorChild(right, 0u);
  const auto rightLeftRight = procedural::planetSurfaceBisectorChild(rightLeft, 1u);
  check(left.depth == 1u && left.path == 0u && right.depth == 1u && right.path == 1u, "child bits form stable bisection paths");
  check(procedural::planetSurfaceBisectorParent(rightLeft).depth == right.depth && procedural::planetSurfaceBisectorParent(rightLeft).path == right.path, "parent address does not depend on a CBT pool slot");
  procedural::PlanetSurfaceBisectorAddress chunk{};
  check(procedural::planetSurfaceBisectorAncestor(rightLeftRight, 1u, chunk) && chunk.rootHalfedge == 0u && chunk.depth == 1u && chunk.path == 1u, "fixed-depth path prefix is a stable streaming chunk key");
  check(!procedural::planetSurfaceBisectorAncestor(rightLeftRight, 4u, chunk), "chunk depth cannot exceed render-leaf depth");

  procedural::PlanetSurfaceTriangle leftTriangle{};
  procedural::PlanetSurfaceTriangle rightTriangle{};
  check(procedural::planetSurfaceBisectorTriangle(mesh, left, leftTriangle) && procedural::planetSurfaceBisectorTriangle(mesh, right, rightTriangle), "child triangles reconstruct from root plus path");
  check(samePoint(leftTriangle.vertices[1], rightTriangle.vertices[0]) && samePoint(leftTriangle.vertices[2], rightTriangle.vertices[2]), "binary children share their compatibility edge exactly");
}

void testAtlasMappingCarriesSourceRegionIdentity()
{
  std::cout << "planet surface atlas mapping\n";
  const std::vector<uint16_t> height{0u, 1000u, 2000u, 3000u, 4000u, 5000u, 6000u, 7000u};
  const std::vector<uint8_t> material{0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};
  const std::vector<uint32_t> region{10u, 11u, 12u, 13u, 20u, 21u, 22u, 23u};
  const procedural::PlanetSurfaceAtlasView atlas{
    4u, 2u, 2u,
    height.data(), height.size(), material.data(), material.size(), region.data(), region.size()};

  procedural::PlanetSurfaceSample sample{};
  check(procedural::samplePlanetSurfaceAtlas(atlas, {1.0, 0.0, 0.0}, sample), "unit direction samples the baked atlas");
  check(sample.texelX == 2u && sample.texelY == 1u, "sphere direction maps to deterministic equirectangular texel");
  check(sample.tileColumn == 1u && sample.tileRow == 0u && sample.tileLocalX == 0u && sample.tileLocalY == 1u, "texel maps to independently streamable tile coordinates");
  check(sample.sourceRegion == 22u && sample.material == 6u, "nearest discrete lookup preserves original Voronoi region and material");
  check(sample.height > 3000u && sample.height < 6000u, "height lookup remains bilinear across LOD vertices");

  procedural::PlanetSurfaceSample seam{};
  check(procedural::samplePlanetSurfaceAtlas(atlas, {-1.0, 0.0, 0.0}, seam), "antimeridian direction samples successfully");
  check(seam.texelX == 0u, "antimeridian wraps to the first atlas column");
}

} // namespace

int main()
{
  testHalfedgeRootsAndStableAddresses();
  testAtlasMappingCarriesSourceRegionIdentity();
  return failures == 0 ? 0 : 1;
}
