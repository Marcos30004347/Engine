#pragma once
#include "procedural/geometry/TerrainMeshBuild.hpp"
namespace procedural::geometry
{
struct RiverSurfaceReach
{
  Point2 a, b;
  double flow = 0, levelA = 0, levelB = 0, depthA = 1, depthB = 1;
  bool outletA = false, outletB = false;
};
struct RiverSurfaceSection
{
  Point2 a, b, leftA, leftB, rightA, rightB;
  double widthA = 0, widthB = 0, levelA = 0, levelB = 0, depthA = 1, depthB = 1;
  // Bounds of the six ribbon corners. A bucket cell is kilometres across and a
  // section is a hundred metres of it, so most of what a cell lists cannot
  // contain the sample; the four barycentric tests are for the ones that can.
  Point2 lo, hi;
};
struct RiverSurfaceSample
{
  bool inside = false;
  double level = 0, depth = 0, lateral = 0;
};
// Shared bank geometry for carving, CDT constraints and the separate water mesh.
class TerrainRiverSurface
{
public:
  void build(const std::vector<RiverSurfaceReach> &reaches, double extent, uint64_t seed);
  RiverSurfaceSample sample(Point2 point) const;
  std::vector<Chain> chains() const;
  bool buildMesh(TerrainMeshSettings settings, TerrainMesh &mesh, std::string *error = nullptr, const std::vector<Chain> &shorelines = {}, const std::function<bool(Point2)> &include = {}) const;
  const std::vector<RiverSurfaceSection> &sections() const
  {
    return sections_;
  }

private:
  double extent_ = 0;
  std::vector<RiverSurfaceSection> sections_;
  std::vector<std::vector<uint32_t>> buckets_;
  size_t bucket(Point2 p) const;
};
} // namespace procedural::geometry
