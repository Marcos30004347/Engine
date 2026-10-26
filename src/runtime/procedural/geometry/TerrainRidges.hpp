#pragma once
#include "procedural/world/MapEdges.hpp"
#include <functional>
namespace procedural::geometry
{
struct RidgeNode
{
  Point2 position;
  uint32_t mapNode = world::kInvalidNode;
  double riseMetres = 0;
  double widthMetres = 0;
};
struct RidgeEdge
{
  uint32_t from = 0, to = 0;
  uint8_t level = 0;
};
struct MountainSkeleton
{
  std::vector<RidgeNode> nodes;
  std::vector<RidgeEdge> edges;
  std::vector<uint32_t> passes;
  uint32_t rangeCount = 0;
  // The process that raised each range, as an `OrogenyKind`. Kept as a byte so
  // this header stays free of the strategy header that includes it.
  std::vector<uint8_t> rangeKinds;
};
// Continuous asymmetric cross-section, including the rounded end caps.
double ridgeUpliftHeight(Point2 p, const RidgeNode &a, const RidgeNode &b);
double ridgeProfileHeight(Point2 p, const RidgeNode &a, const RidgeNode &b);
// Builds the ridge network for the whole map. How high a range may stand comes
// from how much land there is to support it (see `naturalSummitMetres`), and
// what shape it takes comes from the `OrogenyStrategy` chosen per component.
MountainSkeleton buildMountainSkeleton(const world::MapGraph &graph,
    const world::MapEdgeSet &edges, const std::function<double(Point2)> &height,
    const std::function<double(Point2)> &relief, double seaLevel, uint64_t seed);
}
