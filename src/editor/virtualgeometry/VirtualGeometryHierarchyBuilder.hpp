#pragma once

#include "virtualgeometry/VirtualGeometryBounds.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include <limits>
#include <vector>

namespace virtualgeometry
{

// Per-cluster / per-node build logging.
//
// The builder prints a line per hierarchy node and per cluster. That is useful
// when authoring an offline asset and ruinous when a streaming page build calls
// the same code once per resident page: the printf traffic alone outweighs the
// meshing. Off by default for runtime callers, which set it explicitly.
void setVirtualGeometryBuildLogging(bool enabled);
bool virtualGeometryBuildLoggingEnabled();

// Asserts that every cluster DAG edge agrees on its shared bound: a child's
// `parent` is its parent's `self`. Run before and after page packing.
void validateClusterDAGParentSelfConsistency(
    const std::vector<VirtualGeometryCluster> &clusters,
    const std::vector<std::vector<uint32_t>> &clusterDAG,
    const char *stage);

struct BVHNode
{
  AABB aabb;
  std::vector<uint32_t> children;

  LODBounds minBounds;
  LODBounds maxBounds;

  float minLodError;
  float maxParentLodError;
  uint32_t meshPartIndex;
  uint32_t subtreeTriangleCount;
  uint32_t lodLevel;
  bool  isLeaf;

  BVHNode()
      : minLodError(std::numeric_limits<float>::max()),
        maxParentLodError(std::numeric_limits<float>::lowest()),
        meshPartIndex(UINT32_MAX),
        subtreeTriangleCount(0u),
        lodLevel(0u),
        isLeaf(false)
  {
  }
};

class VirtualGeometryHierarchyBuilder
{
public:
  static VirtualGeometryBuildData buildHierarchy(
      const std::vector<Vertex>                          &vertices,
      std::vector<VirtualGeometryCluster>               &clusters,
      std::vector<ClusterGroupInfo>                     &groupInfos,
      const std::vector<std::vector<std::vector<int32_t>>> &groupsPerLevel,
      const rendering::animation::Skeleton             *skeleton = nullptr);

  // The first back-half stage. Kept for its callers; it is
  // `VirtualGeometryPacker::packPages` (phase 9a).
  static void finalizePagesAndRewrites(
      VirtualGeometryBuildData &buildData,
      const VirtualGeometryBuildSettings &settings);

private:
  static uint32_t buildHierarchyLevel(
      const std::vector<Vertex>               &vertices,
      std::vector<VirtualGeometryCluster>     &clusters,
      const std::vector<std::vector<int32_t>> &groups,
      std::vector<BVHNode>                    &nodes,
      uint32_t                                lodLevel,
      const rendering::animation::Skeleton    *skeleton);

  static uint32_t buildBVHIntermediateNodes(
      std::vector<BVHNode> &nodes,
      std::vector<uint32_t> indices,
      bool sort,
      const rendering::animation::Skeleton *skeleton);
  static void     sortBVHNodes(std::vector<BVHNode> &nodes, std::vector<uint32_t> &indices, const std::vector<uint32_t> &childSizes);
  static float    computeBVHCost(const std::vector<BVHNode> &nodes, const std::vector<uint32_t> &indices);

  static void linearizeBVH(
      const std::vector<BVHNode>          &bvhNodes,
      const std::vector<VirtualGeometryCluster> &buildClusters,
      const std::vector<Vertex>           &vertices,
      uint32_t                             root,
      std::vector<VirtualGeometryHierarchy> &outHierarchy,
      std::vector<VirtualGeometryCluster>  &outClusters,
      std::vector<uint32_t>               &outBuildToLinearized);

  static std::vector<std::vector<uint32_t>> buildGroupDAG(
      const std::vector<ClusterGroupInfo>         &groupInfos,
      const std::vector<VirtualGeometryCluster>   &clusters);

  static std::vector<std::vector<uint32_t>> buildClusterDAG(
      uint32_t clusterCount,
      const std::vector<ClusterGroupInfo> &groupInfos);
};

} // namespace virtualgeometry
