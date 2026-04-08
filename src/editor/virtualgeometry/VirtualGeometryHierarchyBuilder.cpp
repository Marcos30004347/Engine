#include "virtualgeometry/VirtualGeometryBoneSelection.hpp"
#include "VirtualGeometryHierarchyBuilder.hpp"
#include "VirtualGeometryHierarchyRewrites.hpp"

#include <cassert>
#include <cmath>
#include <map>
#include <queue>
#include <unordered_map>

const uint32_t MaxBVHChildren = 8u;
const uint32_t MaxBVHLevels = 2;

using namespace virtualgeometry;

namespace VecMath
{
inline float Length(const float v[3])
{
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}
inline float Dot(const float a[3], const float b[3])
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void Sub(float r[3], const float a[3], const float b[3])
{
  r[0] = a[0] - b[0];
  r[1] = a[1] - b[1];
  r[2] = a[2] - b[2];
}
inline void Add(float r[3], const float a[3], const float b[3])
{
  r[0] = a[0] + b[0];
  r[1] = a[1] + b[1];
  r[2] = a[2] + b[2];
}
inline void Scale(float r[3], const float v[3], float s)
{
  r[0] = v[0] * s;
  r[1] = v[1] * s;
  r[2] = v[2] * s;
}
inline void Copy(float d[3], const float s[3])
{
  d[0] = s[0];
  d[1] = s[1];
  d[2] = s[2];
}
inline float Distance(const float a[3], const float b[3])
{
  float d[3];
  Sub(d, a, b);
  return Length(d);
}
} // namespace VecMath

namespace
{

static uint32_t chooseDominantBoneFromClusters(
    const std::vector<VirtualGeometryCluster> &clusters,
    const std::vector<int32_t> &clusterIndices,
    const rendering::animation::Skeleton *skeleton,
    uint32_t selectionLevel)
{
  const detail::BoneSelectionContext selectionContext(skeleton, selectionLevel);
  std::unordered_map<uint32_t, float> scores;

  for (int32_t clusterIndex : clusterIndices)
  {
    if (clusterIndex < 0 || static_cast<size_t>(clusterIndex) >= clusters.size())
      continue;

    const VirtualGeometryCluster &cluster = clusters[clusterIndex];
    if (cluster.meshPartIndex == UINT32_MAX)
      continue;

    const float clusterWeight = static_cast<float>(cluster.indices.size() / 3u);
    detail::accumulateBoneScore(scores, selectionContext, nullptr, cluster.meshPartIndex, clusterWeight);
  }

  return detail::pickHighestScoringBone(scores);
}

static uint32_t chooseDominantBoneFromChildNodes(
    const std::vector<BVHNode> &nodes,
    const std::vector<uint32_t> &childIndices,
    const rendering::animation::Skeleton *skeleton,
    uint32_t selectionLevel)
{
  const detail::BoneSelectionContext selectionContext(skeleton, selectionLevel);
  std::unordered_map<uint32_t, float> scores;

  for (uint32_t childIndex : childIndices)
  {
    if (childIndex >= nodes.size())
      continue;

    const BVHNode &child = nodes[childIndex];
    if (child.meshPartIndex == UINT32_MAX)
      continue;

    const float childWeight = static_cast<float>(std::max<uint32_t>(1u, child.subtreeTriangleCount));
    detail::accumulateBoneScore(scores, selectionContext, nullptr, child.meshPartIndex, childWeight);
  }

  return detail::pickHighestScoringBone(scores);
}

} // namespace

static void joinBounds(LODBounds &a, const LODBounds &b)
{
  if (a.radius == 0.f)
  {
    a = b;
    return;
  }

  float toOther[3];
  VecMath::Sub(toOther, b.center, a.center);
  float distSqr = VecMath::Dot(toOther, toOther);

  if ((a.radius - b.radius) * (a.radius - b.radius) + 1e-4f >= distSqr)
  {
    if (a.radius < b.radius)
      a = b;
  }
  else
  {
    float dist = std::sqrt(distSqr);
    float radius = (dist + b.radius + a.radius) * 0.5f;
    float newC[3];
    VecMath::Copy(newC, a.center);
    if (dist > 1e-4f)
    {
      float off[3];
      VecMath::Scale(off, toOther, (radius - a.radius) / dist);
      VecMath::Add(newC, newC, off);
    }
    VecMath::Copy(a.center, newC);
    a.radius = radius;
  }
  a.error = std::max(a.error, b.error);
}

static LODBounds mergeBounds(const std::vector<LODBounds> &bounds)
{
  if (bounds.empty())
  {
    LODBounds r = {};
    return r;
  }

  int minIdx[3] = {0, 0, 0}, maxIdx[3] = {0, 0, 0};
  for (size_t i = 0; i < bounds.size(); ++i)
    for (int ax = 0; ax < 3; ++ax)
    {
      if (bounds[i].center[ax] - bounds[i].radius < bounds[minIdx[ax]].center[ax] - bounds[minIdx[ax]].radius)
        minIdx[ax] = i;
      if (bounds[i].center[ax] + bounds[i].radius > bounds[maxIdx[ax]].center[ax] + bounds[maxIdx[ax]].radius)
        maxIdx[ax] = i;
    }

  float bestExtent = 0.f;
  uint32_t bestAxis = 0;
  for (int ax = 0; ax < 3; ++ax)
  {
    float d = VecMath::Distance(bounds[maxIdx[ax]].center, bounds[minIdx[ax]].center) + bounds[minIdx[ax]].radius + bounds[maxIdx[ax]].radius;
    if (d > bestExtent)
    {
      bestExtent = d;
      bestAxis = ax;
    }
  }

  LODBounds result = bounds[minIdx[bestAxis]];
  joinBounds(result, bounds[maxIdx[bestAxis]]);
  for (size_t i = 0; i < bounds.size(); ++i)
    joinBounds(result, bounds[i]);
  return result;
}

static AABB aabbFromSphereBounds(const LODBounds &bounds)
{
  AABB result;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    result.minPoint[axis] = bounds.center[axis] - bounds.radius;
    result.maxPoint[axis] = bounds.center[axis] + bounds.radius;
  }
  return result;
}

float VirtualGeometryHierarchyBuilder::computeBVHCost(const std::vector<BVHNode> &nodes, const std::vector<uint32_t> &indices)
{
  AABB aabb = nodes[indices[0]].aabb;
  for (uint32_t idx : indices)
    aabb.expandBy(nodes[idx].aabb);
  return aabb.GetSurfaceArea();
}

void VirtualGeometryHierarchyBuilder::sortBVHNodes(std::vector<BVHNode> &nodes, std::vector<uint32_t> &indices, const std::vector<uint32_t> &childSizes)
{
  for (uint32_t level = 0; level < MaxBVHLevels; ++level)
  {
    const uint32_t numBuckets = 1u << level;
    const uint32_t childrenPerBucket = MaxBVHChildren >> level;
    const uint32_t halfChildren = childrenPerBucket >> 1;
    uint32_t indexOffset = 0;

    for (uint32_t bucketIdx = 0; bucketIdx < numBuckets; ++bucketIdx)
    {
      const uint32_t firstChild = bucketIdx * childrenPerBucket;
      uint32_t sizes[2] = {0, 0};
      for (uint32_t i = 0; i < halfChildren; ++i)
      {
        sizes[0] += childSizes[firstChild + i];
        sizes[1] += childSizes[firstChild + i + halfChildren];
      }
      uint32_t total = sizes[0] + sizes[1];
      std::vector<uint32_t> slice(indices.begin() + indexOffset, indices.begin() + indexOffset + total);

      auto sortByAxis = [&](uint32_t axis)
      {
        std::sort(
            slice.begin(),
            slice.end(),
            [&](uint32_t a, uint32_t b)
            {
              float ca[3], cb[3];
              nodes[a].aabb.GetCenter(ca);
              nodes[b].aabb.GetCenter(cb);
              return ca[axis] < cb[axis];
            });
      };

      float bestCost = std::numeric_limits<float>::max();
      uint32_t bestAxis = 0;
      for (uint32_t axis = 0; axis < 3; ++axis)
      {
        sortByAxis(axis);
        std::vector<uint32_t> left(slice.begin(), slice.begin() + sizes[0]);
        std::vector<uint32_t> right(slice.begin() + sizes[0], slice.end());
        float cost = computeBVHCost(nodes, left) + computeBVHCost(nodes, right);
        if (cost < bestCost)
        {
          bestCost = cost;
          bestAxis = axis;
        }
      }
      sortByAxis(bestAxis);
      std::copy(slice.begin(), slice.end(), indices.begin() + indexOffset);
      indexOffset += total;
    }
  }
}

uint32_t VirtualGeometryHierarchyBuilder::buildBVHIntermediateNodes(
    std::vector<BVHNode> &nodes,
    std::vector<uint32_t> indices,
    bool sort,
    const rendering::animation::Skeleton *skeleton)
{
  if (indices.size() == 1)
    return indices[0];

  uint32_t root = static_cast<uint32_t>(nodes.size());
  nodes.push_back(BVHNode());
  nodes[root].maxParentLodError = std::numeric_limits<float>::lowest();
  nodes[root].minLodError = std::numeric_limits<float>::max();

  if (indices.size() <= MaxBVHChildren)
  {
    nodes[root].children = indices;
    for (auto idx : indices)
    {
      nodes[root].aabb.expandBy(nodes[idx].aabb);
      nodes[root].subtreeTriangleCount += nodes[idx].subtreeTriangleCount;
      nodes[root].lodLevel = std::max(nodes[root].lodLevel, nodes[idx].lodLevel);
    }
    nodes[root].meshPartIndex = chooseDominantBoneFromChildNodes(nodes, nodes[root].children, skeleton, nodes[root].lodLevel);

    std::vector<LODBounds> minBounds;
    std::vector<LODBounds> maxBounds;

    for (auto idx : indices)
    {
      minBounds.push_back(nodes[idx].minBounds);
      maxBounds.push_back(nodes[idx].maxBounds);

      nodes[root].maxParentLodError = std::max(nodes[root].maxParentLodError, nodes[idx].maxParentLodError);
      nodes[root].minLodError = std::min(nodes[root].minLodError, nodes[idx].minLodError);
    }

    nodes[root].minBounds = mergeBounds(minBounds);
    nodes[root].maxBounds = mergeBounds(maxBounds);

    printf(
        "  node[%u] maxParentLodError = %f, minLodError = %f, minBounds(%f, %f, %f - %f) maxBounds(%f, %f, %f - %f) %s\n",
        root,
        nodes[root].maxParentLodError,
        nodes[root].minLodError,
        nodes[root].minBounds.center[0],
        nodes[root].minBounds.center[1],
        nodes[root].minBounds.center[2],
        nodes[root].minBounds.radius,
        nodes[root].maxBounds.center[0],
        nodes[root].maxBounds.center[1],
        nodes[root].maxBounds.center[2],
        nodes[root].maxBounds.radius,
        nodes[root].isLeaf ? "leaf" : "node");
    return root;
  }

  uint32_t noExcessLevelSize = MaxBVHChildren;
  while (noExcessLevelSize * MaxBVHChildren <= indices.size())
    noExcessLevelSize *= MaxBVHChildren;

  uint32_t full = noExcessLevelSize;
  uint32_t childCount = noExcessLevelSize / MaxBVHChildren;
  uint32_t maxExcess = full - childCount;
  std::vector<uint32_t> sizes(MaxBVHChildren, 0);
  uint32_t excess = static_cast<uint32_t>(indices.size()) - full;
  for (int32_t i = MaxBVHChildren - 1; i >= 0; --i)
  {
    sizes[i] = childCount + std::min(excess, maxExcess);
    excess -= std::min(excess, maxExcess);
  }
  assert(excess == 0);

  if (sort)
    sortBVHNodes(nodes, indices, sizes);

  uint32_t offset = 0;
  for (uint32_t i = 0; i < MaxBVHChildren; ++i)
  {
    uint32_t childIdx = buildBVHIntermediateNodes(nodes, std::vector<uint32_t>(indices.begin() + offset, indices.begin() + offset + sizes[i]), sort, skeleton);
    nodes[root].children.push_back(childIdx);
    offset += sizes[i];
  }

  for (auto idx : nodes[root].children)
  {
    nodes[root].aabb.expandBy(nodes[idx].aabb);
    nodes[root].subtreeTriangleCount += nodes[idx].subtreeTriangleCount;
    nodes[root].lodLevel = std::max(nodes[root].lodLevel, nodes[idx].lodLevel);
  }
  nodes[root].meshPartIndex = chooseDominantBoneFromChildNodes(nodes, nodes[root].children, skeleton, nodes[root].lodLevel);

  std::vector<LODBounds> minBounds;
  std::vector<LODBounds> maxBounds;

  for (auto idx : nodes[root].children)
  {
    minBounds.push_back(nodes[idx].minBounds);
    maxBounds.push_back(nodes[idx].maxBounds);

    nodes[root].maxParentLodError = std::max(nodes[root].maxParentLodError, nodes[idx].maxParentLodError);
    nodes[root].minLodError = std::min(nodes[root].minLodError, nodes[idx].minLodError);
  }

  nodes[root].minBounds = mergeBounds(minBounds);
  nodes[root].maxBounds = mergeBounds(maxBounds);

  printf(
      "  node[%u] maxParentLodError = %f, minLodError = %f, minBounds(%f, %f, %f - %f) maxBounds(%f, %f, %f - %f) %s\n",
      root,
      nodes[root].maxParentLodError,
      nodes[root].minLodError,
      nodes[root].minBounds.center[0],
      nodes[root].minBounds.center[1],
      nodes[root].minBounds.center[2],
      nodes[root].minBounds.radius,
      nodes[root].maxBounds.center[0],
      nodes[root].maxBounds.center[1],
      nodes[root].maxBounds.center[2],
      nodes[root].maxBounds.radius,
      nodes[root].isLeaf ? "leaf" : "node");
  return root;
}

uint32_t
VirtualGeometryHierarchyBuilder::buildHierarchyLevel(
    const std::vector<Vertex> &vertices,
    std::vector<VirtualGeometryCluster> &clusters,
    const std::vector<std::vector<int32_t>> &groups,
    std::vector<BVHNode> &nodes,
    uint32_t lodLevel,
    const rendering::animation::Skeleton *skeleton)
{
  uint32_t offset = static_cast<uint32_t>(nodes.size());

  for (const auto &groupClusters : groups)
  {
    uint32_t nodeIdx = static_cast<uint32_t>(nodes.size());
    nodes.push_back(BVHNode());
    std::vector<LODBounds> selfBounds;
    std::vector<LODBounds> parentBounds;

    nodes[nodeIdx].minLodError = std::numeric_limits<float>::max();
    nodes[nodeIdx].maxParentLodError = std::numeric_limits<float>::lowest();

    assert(groupClusters.size() <= MaxBVHChildren);
    nodes[nodeIdx].children.reserve(groupClusters.size());

    for (int32_t ci : groupClusters)
    {
      nodes[nodeIdx].children.push_back(static_cast<uint32_t>(ci));

      selfBounds.push_back(clusters[ci].self);
      parentBounds.push_back(clusters[ci].parent);

      nodes[nodeIdx].maxParentLodError = std::max(nodes[nodeIdx].maxParentLodError, clusters[ci].parent.error);
      nodes[nodeIdx].minLodError = std::min(nodes[nodeIdx].minLodError, clusters[ci].self.error);

      for (uint32_t vi : clusters[ci].indices)
        nodes[nodeIdx].aabb.expandBy(vertices[vi].pos);
    }
    nodes[nodeIdx].meshPartIndex = chooseDominantBoneFromClusters(clusters, groupClusters, skeleton, lodLevel);
    if (nodes[nodeIdx].meshPartIndex != UINT32_MAX)
    {
      for (int32_t ci : groupClusters)
        nodes[nodeIdx].aabb.expandBy(aabbFromSphereBounds(clusters[ci].self));
    }
    // Keep self and parent bounds separate:
    // - minBounds pairs with min_lod_error (self)
    // - maxBounds pairs with max_parent_lod_error (parent/coarser)
    nodes[nodeIdx].minBounds = mergeBounds(selfBounds);
    nodes[nodeIdx].maxBounds = mergeBounds(parentBounds);
    nodes[nodeIdx].lodLevel = lodLevel;
    for (int32_t ci : groupClusters)
      nodes[nodeIdx].subtreeTriangleCount += static_cast<uint32_t>(clusters[ci].indices.size() / 3u);

    nodes[nodeIdx].isLeaf = true;

    printf(
        "  node[%u] maxParentLodError = %f, minLodError = %f, minBounds(%f, %f, %f - %f) maxBounds(%f, %f, %f - %f) %s\n",
        nodeIdx,
        nodes[nodeIdx].maxParentLodError,
        nodes[nodeIdx].minLodError,
        nodes[nodeIdx].minBounds.center[0],
        nodes[nodeIdx].minBounds.center[1],
        nodes[nodeIdx].minBounds.center[2],
        nodes[nodeIdx].minBounds.radius,
        nodes[nodeIdx].maxBounds.center[0],
        nodes[nodeIdx].maxBounds.center[1],
        nodes[nodeIdx].maxBounds.center[2],
        nodes[nodeIdx].maxBounds.radius,
        nodes[nodeIdx].isLeaf ? "leaf" : "node");

    for (auto i : nodes[nodeIdx].children)
    {
      printf(
          "  cluster[%u] maxParentLodError = %f, minLodError = %f, minBounds(%f, %f, %f - %f) maxBounds(%f, %f, %f - %f)\n",
          i,
          clusters[i].parent.error,
          clusters[i].self.error,
          clusters[i].self.center[0],
          clusters[i].self.center[1],
          clusters[i].self.center[2],
          clusters[i].self.radius,
          clusters[i].parent.center[0],
          clusters[i].parent.center[1],
          clusters[i].parent.center[2],
          clusters[i].parent.radius);
    }
  }

  std::vector<uint32_t> indices;
  for (uint32_t i = offset; i < nodes.size(); ++i)
    indices.push_back(i);

  return buildBVHIntermediateNodes(nodes, indices, true, skeleton);
}

void VirtualGeometryHierarchyBuilder::linearizeBVH(
    const std::vector<BVHNode> &bvhNodes,
    const std::vector<VirtualGeometryCluster> &buildClusters,
    const std::vector<Vertex> &vertices,
    uint32_t root,
    std::vector<VirtualGeometryHierarchy> &outHierarchy,
    std::vector<VirtualGeometryCluster> &outClusters,
    std::vector<uint32_t> &outBuildToLinearized)
{
  std::queue<uint32_t> pending;
  pending.push(root);

  while (!pending.empty())
  {
    uint32_t current = pending.front();
    pending.pop();
    uint32_t nodeIdx = static_cast<uint32_t>(outHierarchy.size());
    outHierarchy.push_back(VirtualGeometryHierarchy());
    VirtualGeometryHierarchy &node = outHierarchy[nodeIdx];

    node.max_x = bvhNodes[current].aabb.maxPoint[0];
    node.max_y = bvhNodes[current].aabb.maxPoint[1];
    node.max_z = bvhNodes[current].aabb.maxPoint[2];
    node.min_x = bvhNodes[current].aabb.minPoint[0];
    node.min_y = bvhNodes[current].aabb.minPoint[1];
    node.min_z = bvhNodes[current].aabb.minPoint[2];
    node.min_lod_error = bvhNodes[current].minLodError;
    node.max_radius = bvhNodes[current].maxBounds.radius;
    node.max_center_x = bvhNodes[current].maxBounds.center[0];
    node.max_center_y = bvhNodes[current].maxBounds.center[1];
    node.max_center_z = bvhNodes[current].maxBounds.center[2];
    node.max_parent_lod_error = bvhNodes[current].maxParentLodError;
    node.flags = bvhNodes[current].isLeaf ? 1u : 0u;
    node.pageIndex = UINT32_MAX;
    node.meshPartIndex = bvhNodes[current].meshPartIndex;

    if (bvhNodes[current].isLeaf)
    {
      node.child_start = static_cast<uint32_t>(outClusters.size());
      node.child_count = static_cast<uint32_t>(bvhNodes[current].children.size());

      for (uint32_t buildCI : bvhNodes[current].children)
      {
        if (buildCI >= outBuildToLinearized.size())
          outBuildToLinearized.resize(buildCI + 1, UINT32_MAX);
        outBuildToLinearized[buildCI] = static_cast<uint32_t>(outClusters.size());

        // Convert from global indices to cluster-local indices + vertices
        const VirtualGeometryCluster &bc = buildClusters[buildCI];
        VirtualGeometryCluster fc;

        std::map<uint32_t, uint32_t> vertexMap;
        std::vector<Vertex> clusterVerts;
        for (uint32_t origIdx : bc.indices)
          if (vertexMap.find(origIdx) == vertexMap.end())
          {
            vertexMap[origIdx] = static_cast<uint32_t>(clusterVerts.size());
            clusterVerts.push_back(vertices[origIdx]);
          }

        fc.indices.reserve(bc.indices.size());
        for (uint32_t origIdx : bc.indices)
          fc.indices.push_back(vertexMap[origIdx]);
        fc.self = bc.self;
        fc.parent = bc.parent;
        fc.cone = bc.cone;
        fc.groupId = bc.groupId;
        fc.meshPartIndex = bc.meshPartIndex;
        fc.vertices = std::move(clusterVerts);
        outClusters.push_back(std::move(fc));
      }
    }
    else
    {
      node.child_start = static_cast<uint32_t>(outHierarchy.size() + pending.size());
      node.child_count = static_cast<uint32_t>(bvhNodes[current].children.size());
      for (uint32_t childIdx : bvhNodes[current].children)
        pending.push(childIdx);
    }
  }
}

std::vector<std::vector<uint32_t>> VirtualGeometryHierarchyBuilder::buildGroupDAG(const std::vector<ClusterGroupInfo> &groupInfos, const std::vector<VirtualGeometryCluster> &clusters)
{
  const uint32_t groupCount = static_cast<uint32_t>(groupInfos.size());
  std::vector<std::unordered_set<uint32_t>> dagSets(groupCount);

  for (uint32_t gid = 0; gid < groupCount; ++gid)
  {
    const ClusterGroupInfo &group = groupInfos[gid];
    for (int32_t ci : group.originalClusterIndices)
      for (int32_t si : group.simplifiedClusterIndices)
      {
        auto &c = clusters[ci], &s = clusters[si];
        assert(s.groupId != c.groupId);
        assert(c.groupId == gid);
        dagSets[s.groupId].insert(c.groupId);
      }
  }

  std::vector<std::vector<uint32_t>> dag(groupCount);
  for (uint32_t i = 0; i < groupCount; ++i)
    dag[i].assign(dagSets[i].begin(), dagSets[i].end());
  return dag;
}

std::vector<std::vector<uint32_t>> VirtualGeometryHierarchyBuilder::buildClusterDAG(uint32_t clusterCount, const std::vector<ClusterGroupInfo> &groupInfos)
{
  std::vector<std::unordered_set<uint32_t>> dagSets(clusterCount);

  for (const ClusterGroupInfo &group : groupInfos)
  {
    for (int32_t simplifiedIdx : group.simplifiedClusterIndices)
    {
      const uint32_t parentCluster = static_cast<uint32_t>(simplifiedIdx);
      assert(parentCluster < clusterCount);
      for (int32_t originalIdx : group.originalClusterIndices)
      {
        const uint32_t childCluster = static_cast<uint32_t>(originalIdx);
        assert(childCluster < clusterCount);
        if (parentCluster != childCluster)
          dagSets[parentCluster].insert(childCluster);
      }
    }
  }

  std::vector<std::vector<uint32_t>> dag(clusterCount);
  for (uint32_t i = 0; i < clusterCount; ++i)
    dag[i].assign(dagSets[i].begin(), dagSets[i].end());
  return dag;
}

static bool nearlyEqualFloat(float a, float b, float epsilon)
{
  return std::fabs(a - b) <= epsilon;
}

static void validateClusterDAGParentSelfConsistency(
    const std::vector<VirtualGeometryCluster> &clusters,
    const std::vector<std::vector<uint32_t>> &clusterDAG,
    const char *stageLabel)
{
  constexpr float kCenterEpsilon = 1e-5f;
  constexpr float kRadiusEpsilon = 1e-5f;
  constexpr float kErrorEpsilon = 1e-5f;
  constexpr uint32_t kMaxMismatchLogs = 32u;

  uint32_t edgeCount = 0u;
  uint32_t mismatchCount = 0u;

  for (uint32_t parentIdx = 0u; parentIdx < clusterDAG.size(); ++parentIdx)
  {
    if (parentIdx >= clusters.size())
      continue;

    const LODBounds &parentSelf = clusters[parentIdx].self;
    for (uint32_t childIdx : clusterDAG[parentIdx])
    {
      ++edgeCount;
      if (childIdx >= clusters.size())
      {
        ++mismatchCount;
        if (mismatchCount <= kMaxMismatchLogs)
          std::printf(
              "[clusterDAG:%s] INVALID_EDGE parent=%u child=%u (clusterCount=%zu)\n",
              stageLabel,
              parentIdx,
              childIdx,
              clusters.size());
        continue;
      }

      const LODBounds &childParent = clusters[childIdx].parent;
      const bool centerOk = nearlyEqualFloat(parentSelf.center[0], childParent.center[0], kCenterEpsilon) && nearlyEqualFloat(parentSelf.center[1], childParent.center[1], kCenterEpsilon) &&
                            nearlyEqualFloat(parentSelf.center[2], childParent.center[2], kCenterEpsilon);
      const bool radiusOk = nearlyEqualFloat(parentSelf.radius, childParent.radius, kRadiusEpsilon);
      const bool errorOk = nearlyEqualFloat(parentSelf.error, childParent.error, kErrorEpsilon);
      if (centerOk && radiusOk && errorOk)
        continue;

      ++mismatchCount;
      if (mismatchCount <= kMaxMismatchLogs)
      {
        std::printf(
            "[clusterDAG:%s] MISMATCH parent=%u child=%u | parent.self(c=(%.6f, %.6f, %.6f) r=%.6f e=%.6f) child.parent(c=(%.6f, %.6f, %.6f) r=%.6f e=%.6f)\n",
            stageLabel,
            parentIdx,
            childIdx,
            parentSelf.center[0],
            parentSelf.center[1],
            parentSelf.center[2],
            parentSelf.radius,
            parentSelf.error,
            childParent.center[0],
            childParent.center[1],
            childParent.center[2],
            childParent.radius,
            childParent.error);
      }
    }
  }

  std::printf(
      "[clusterDAG:%s] parent/self consistency: edges=%u mismatches=%u\n",
      stageLabel,
      edgeCount,
      mismatchCount);
}

VirtualGeometryBuildData VirtualGeometryHierarchyBuilder::buildHierarchy(
    const std::vector<Vertex> &vertices,
    std::vector<VirtualGeometryCluster> &clusters,
    std::vector<ClusterGroupInfo> &groupInfos,
    const std::vector<std::vector<std::vector<int32_t>>> &groupsPerLevel,
    const rendering::animation::Skeleton *skeleton)
{
  VirtualGeometryBuildData result;

  std::vector<BVHNode> bvhNodes;
  std::vector<uint32_t> roots;

  for (uint32_t levelIndex = 0u; levelIndex < groupsPerLevel.size(); ++levelIndex)
  {
    uint32_t root = buildHierarchyLevel(vertices, clusters, groupsPerLevel[levelIndex], bvhNodes, levelIndex, skeleton);
    roots.push_back(root);
  }

  uint32_t hierarchyRoot = buildBVHIntermediateNodes(bvhNodes, roots, false, skeleton);

  printf("Hierarchy size: %zu nodes\n", bvhNodes.size());

  std::vector<uint32_t> buildToLinearized;
  linearizeBVH(bvhNodes, clusters, vertices, hierarchyRoot, result.lodLevelHierarchy, result.clusters, buildToLinearized);
  result.buildToLinearizedClusterIndex = buildToLinearized;

  assert(clusters.size() == result.clusters.size());
  printf("Total clusters = %zu\n", result.clusters.size());

  for (auto &info : groupInfos)
  {
    for (auto &idx : info.originalClusterIndices)
    {
      assert(static_cast<uint32_t>(idx) < buildToLinearized.size());
      assert(buildToLinearized[idx] != UINT32_MAX && "Build cluster was not linearized");
      idx = static_cast<int32_t>(buildToLinearized[idx]);
    }
    for (auto &idx : info.simplifiedClusterIndices)
    {
      assert(static_cast<uint32_t>(idx) < buildToLinearized.size());
      assert(buildToLinearized[idx] != UINT32_MAX && "Simplified cluster was not linearized");
      idx = static_cast<int32_t>(buildToLinearized[idx]);
    }
  }

  result.groupDAG = buildGroupDAG(groupInfos, result.clusters);
  result.clusterDAG = buildClusterDAG(static_cast<uint32_t>(result.clusters.size()), groupInfos);
  result.groupInfos = groupInfos;
  validateClusterDAGParentSelfConsistency(result.clusters, result.clusterDAG, "pre_rewrite");

  for (uint32_t parent = 0; parent < result.groupDAG.size(); ++parent)
    for (uint32_t child : result.groupDAG[parent])
      assert(groupInfos[parent].lodLevel > groupInfos[child].lodLevel && "Parent must be coarser than child");
  return result;
}

void VirtualGeometryHierarchyBuilder::finalizePagesAndRewrites(
    VirtualGeometryBuildData &buildData,
    const VirtualGeometryBuildSettings &settings)
{
  buildData.buildSettings = settings;
  VirtualGeometryHierarchyRewrites::buildPagesAndRewrites(buildData, settings);
  validateClusterDAGParentSelfConsistency(buildData.clusters, buildData.clusterDAG, "post_rewrite");
}
