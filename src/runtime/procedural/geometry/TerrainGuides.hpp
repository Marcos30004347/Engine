#pragma once
#include "procedural/geometry/TerrainMeshBuild.hpp"
namespace procedural::geometry
{
struct CorridorGuide
{
  Point2 a, b;
  double innerWidth = 0, outerWidth = 0;
  double innerWidthEnd = -1, outerWidthEnd = -1;
};
// Heights are never pinned: guide vertices sample the final eroded/carved field.
std::vector<Chain> ridgeSurfaceGuides(Point2 a, Point2 b, double width, double spacing, const HeightFunction &height, const std::function<bool(Point2)> &valid);
std::vector<Chain> riverSurfaceGuides(const std::vector<CorridorGuide> &segments, double spacing);
std::vector<Chain> slopeSurfaceGuides(double extent, double spacing, uint64_t seed, const HeightFunction &height, const std::function<bool(Point2)> &valid);
} // namespace procedural::geometry
