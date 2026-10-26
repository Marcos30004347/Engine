#include "editor/virtualgeometry/TerrainVirtualGeometrySceneSchedule.hpp"

#include <algorithm>
#include <cmath>

namespace voxel::svraster
{
namespace
{

constexpr double kEpsilonMetres = 1.0e-3;

bool containsSquare(const TerrainVGGroundFootprint &cover, const TerrainVGGroundFootprint &area)
{
  const double halfCover = cover.edge * 0.5 + kEpsilonMetres;
  const double halfArea = area.edge * 0.5;
  return std::fabs(area.centerX - cover.centerX) + halfArea <= halfCover &&
      std::fabs(area.centerZ - cover.centerZ) + halfArea <= halfCover;
}

bool coveredRecursive(
    const TerrainVGGroundFootprint &area,
    const std::vector<const TerrainVGGroundFootprint *> &covers,
    const uint32_t depth)
{
  std::vector<const TerrainVGGroundFootprint *> touching;
  touching.reserve(covers.size());
  for (const auto *cover : covers)
  {
    if (containsSquare(*cover, area))
      return true;
    if (terrainVGFootprintsOverlap(*cover, area))
      touching.push_back(cover);
  }
  if (touching.empty() || depth == 0u)
    return false;
  const double quarter = area.edge * 0.25;
  for (const double dx : {-quarter, quarter})
    for (const double dz : {-quarter, quarter})
    {
      TerrainVGGroundFootprint quadrant{};
      quadrant.centerX = area.centerX + dx;
      quadrant.centerZ = area.centerZ + dz;
      quadrant.edge = area.edge * 0.5;
      quadrant.lod = area.lod;
      if (!coveredRecursive(quadrant, touching, depth - 1u))
        return false;
    }
  return true;
}

} // namespace

TerrainVGGroundKey terrainVGGroundKey(const TerrainVGGroundFootprint &footprint)
{
  TerrainVGGroundKey key{};
  key.centerX = static_cast<int64_t>(std::llround(footprint.centerX * 1000.0));
  key.centerZ = static_cast<int64_t>(std::llround(footprint.centerZ * 1000.0));
  key.lod = footprint.lod;
  return key;
}

bool terrainVGFootprintContains(const TerrainVGGroundFootprint &footprint, const double x, const double z)
{
  const double half = footprint.edge * 0.5 + kEpsilonMetres;
  return std::fabs(x - footprint.centerX) <= half && std::fabs(z - footprint.centerZ) <= half;
}

bool terrainVGFootprintsOverlap(const TerrainVGGroundFootprint &a, const TerrainVGGroundFootprint &b)
{
  const double halfA = a.edge * 0.5;
  const double halfB = b.edge * 0.5;
  return std::fabs(a.centerX - b.centerX) < halfA + halfB - kEpsilonMetres &&
      std::fabs(a.centerZ - b.centerZ) < halfA + halfB - kEpsilonMetres;
}

bool terrainVGFootprintCoveredBy(
    const TerrainVGGroundFootprint &area,
    const std::vector<const TerrainVGGroundFootprint *> &covers,
    const uint32_t maxDepth)
{
  if (area.edge <= 0.0)
    return true;
  return coveredRecursive(area, covers, maxDepth);
}

} // namespace voxel::svraster
