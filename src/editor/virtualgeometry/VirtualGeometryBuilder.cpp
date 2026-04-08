#include "VirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryBoneSelection.hpp"
#include "VirtualGeometryHierarchyBuilder.hpp"
#include "VirtualGeometryPartitioner.hpp"
#include "VirtualGeometrySimplifier.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "meshoptimizer.h"

#include <atomic>
#include <functional>
#include <thread>

namespace virtualgeometry
{

namespace
{

static constexpr uint32_t kPageNotInstalledBit = (1u << 31);

static void addCandidateBoneAndAncestors(
    std::vector<uint32_t> &candidateBones,
    const rendering::animation::Skeleton *skeleton,
    uint32_t boneIndex)
{
  if (boneIndex == UINT32_MAX)
    return;

  candidateBones.push_back(boneIndex);
  if (!detail::isValidBoneIndex(skeleton, boneIndex))
    return;

  int32_t parentIndex = skeleton->getBone(boneIndex).parentIndex;
  while (parentIndex >= 0)
  {
    const uint32_t parentBoneIndex = static_cast<uint32_t>(parentIndex);
    candidateBones.push_back(parentBoneIndex);
    if (!detail::isValidBoneIndex(skeleton, parentBoneIndex))
      break;
    parentIndex = skeleton->getBone(parentBoneIndex).parentIndex;
  }
}

static void finalizeCandidateBones(std::vector<uint32_t> &candidateBones)
{
  std::sort(candidateBones.begin(), candidateBones.end());
  candidateBones.erase(std::unique(candidateBones.begin(), candidateBones.end()), candidateBones.end());
}

static uint32_t chooseDominantBoneFromVertices(
    const std::vector<Vertex> &vertices,
    const std::vector<uint32_t> &indices,
    const rendering::animation::Skeleton *skeleton,
    const std::vector<uint32_t> *candidateBones = nullptr,
    uint32_t selectionLevel = 0u)
{
  if (vertices.empty())
    return UINT32_MAX;

  const detail::BoneSelectionContext selectionContext(skeleton, selectionLevel);
  std::unordered_map<uint32_t, float> scores;
  std::vector<uint8_t> visited(vertices.size(), 0u);

  for (uint32_t index : indices)
  {
    if (index >= vertices.size() || visited[index] != 0u)
      continue;

    visited[index] = 1u;
    for (const BoneWeight &boneWeight : vertices[index].boneWeights)
      detail::accumulateBoneScore(scores, selectionContext, candidateBones, boneWeight.boneIndex, boneWeight.weight);
  }

  if (scores.empty())
    return UINT32_MAX;

  return detail::pickHighestScoringBone(scores);
}

static std::vector<uint32_t> collectCandidateBonesFromClusterGroup(
    const std::vector<Vertex> &vertices,
    const std::vector<VirtualGeometryCluster> &clusters,
    const std::vector<int32_t> &group,
    const rendering::animation::Skeleton *skeleton)
{
  std::vector<uint32_t> candidateBones;
  std::unordered_set<uint32_t> visitedVertices;
  visitedVertices.reserve(group.size() * ClusterSize);

  for (int32_t clusterIndex : group)
  {
    if (clusterIndex < 0 || static_cast<size_t>(clusterIndex) >= clusters.size())
      continue;

    for (uint32_t vertexIndex : clusters[clusterIndex].indices)
    {
      if (!visitedVertices.insert(vertexIndex).second || vertexIndex >= vertices.size())
        continue;

      for (const BoneWeight &boneWeight : vertices[vertexIndex].boneWeights)
        addCandidateBoneAndAncestors(candidateBones, skeleton, boneWeight.boneIndex);
    }
  }

  finalizeCandidateBones(candidateBones);
  return candidateBones;
}

static std::vector<MeshPartInfo> buildMeshPartsForSkeleton(const rendering::animation::Skeleton *skeleton)
{
  if (skeleton == nullptr || skeleton->empty())
    return {MeshPartInfo{UINT32_MAX}};

  std::vector<MeshPartInfo> meshParts(skeleton->getBoneCount());
  for (uint32_t boneIndex = 0u; boneIndex < skeleton->getBoneCount(); ++boneIndex)
    meshParts[boneIndex].dominantBoneIndex = boneIndex;
  return meshParts;
}

} // namespace


// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

static int32_t followParent(std::vector<int32_t> &parents, int32_t index)
{
  while (index != parents[index])
  {
    int32_t parent = parents[index];
    parents[index] = parents[parent];
    index = parent;
  }
  return index;
}

static int32_t getConnectedComponents(std::vector<int32_t> &parents, const std::vector<uint32_t> &indices, const std::vector<uint32_t> &remap)
{
  assert(parents.size() == remap.size());
  for (uint64_t i = 0; i < indices.size(); ++i)
  {
    uint32_t v = remap[indices[i]];
    parents[v] = v;
  }
  for (uint64_t i = 0; i < indices.size(); i += 3)
    for (int e = 0; e < 3; ++e)
    {
      int32_t v0 = followParent(parents, remap[indices[i + e]]);
      int32_t v1 = followParent(parents, remap[indices[i + (e == 2 ? 0 : e + 1)]]);
      if (v0 != v1)
        parents[v0] = v1;
    }
  for (uint64_t i = 0; i < indices.size(); ++i)
  {
    uint32_t v = remap[indices[i]];
    parents[v] = followParent(parents, v);
  }
  int32_t roots = 0;
  for (uint64_t i = 0; i < indices.size(); ++i)
  {
    uint32_t v = remap[indices[i]];
    if (parents[v] == static_cast<int32_t>(v))
      roots++;
    parents[v] = -1;
  }
  return roots;
}

static int32_t getUniqueVertices(std::vector<int32_t> &used, const std::vector<uint32_t> &indices, const std::vector<uint8_t> *locks = nullptr)
{
  for (uint64_t i = 0; i < indices.size(); ++i)
    used[indices[i]] = 1;
  uint64_t verts = 0;
  for (uint64_t i = 0; i < indices.size(); ++i)
  {
    uint32_t v = indices[i];
    verts += used[v] && (!locks || !(*locks)[v]);
    used[v] = 0;
  }
  return static_cast<int32_t>(verts);
}

static LODBounds computeBounds(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices, float error)
{
  meshopt_Bounds b = meshopt_computeClusterBounds(indices.data(), indices.size(), &vertices[0].pos[0], vertices.size(), sizeof(Vertex));
  LODBounds r;
  r.center[0] = b.center[0];
  r.center[1] = b.center[1];
  r.center[2] = b.center[2];
  r.radius = b.radius;
  r.error = error;
  return r;
}

static ClusterCone computeClusterCone(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices)
{
  ClusterCone cone{};
  if (vertices.empty() || indices.empty())
    return cone;

  const meshopt_Bounds bounds = meshopt_computeClusterBounds(indices.data(), indices.size(), &vertices[0].pos[0], vertices.size(), sizeof(Vertex));
  cone.axis[0] = bounds.cone_axis[0];
  cone.axis[1] = bounds.cone_axis[1];
  cone.axis[2] = bounds.cone_axis[2];
  cone.cutoff = bounds.cone_cutoff;
  return cone;
}

static LODBounds mergeClusterGroupBounds(const std::vector<VirtualGeometryCluster> &clusters, const std::vector<int32_t> &group)
{
  std::vector<LODBounds> bounds;
  bounds.reserve(group.size());
  for (int idx : group)
    bounds.push_back(clusters[idx].self);

  meshopt_Bounds m = meshopt_computeSphereBounds(&bounds[0].center[0], bounds.size(), sizeof(LODBounds), &bounds[0].radius, sizeof(LODBounds));
  LODBounds r = {};
  r.center[0] = m.center[0];
  r.center[1] = m.center[1];
  r.center[2] = m.center[2];
  r.radius = m.radius;
  for (int idx : group)
    r.error = std::max(r.error, clusters[idx].self.error);
  return r;
}

static void rebuildPositionRemap(const std::vector<Vertex> &globalVertices, const std::vector<VirtualGeometryCluster> &clusters, std::vector<uint32_t> &remap)
{
  remap.resize(globalVertices.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(remap.size()); ++i)
    remap[i] = i;

  if (globalVertices.empty())
    return;

  std::vector<uint32_t> allIndices;
  for (const auto &c : clusters)
    allIndices.insert(allIndices.end(), c.indices.begin(), c.indices.end());

  if (allIndices.empty())
    return;

  meshopt_Stream posStream = {&globalVertices[0].pos[0], sizeof(float) * 3, sizeof(Vertex)};
  meshopt_generateVertexRemapMulti(remap.data(), allIndices.data(), allIndices.size(), globalVertices.size(), &posStream, 1);

  const uint32_t kInvalid = std::numeric_limits<uint32_t>::max();
  for (uint32_t i = 0; i < static_cast<uint32_t>(remap.size()); ++i)
    if (remap[i] == kInvalid)
      remap[i] = i;
}

static std::vector<std::vector<int32_t>> splitGroupsByLimit(const std::vector<std::vector<int32_t>> &groups, uint32_t maxGroupSize)
{
  std::vector<std::vector<int32_t>> split;
  split.reserve(groups.size());

  for (const auto &g : groups)
  {
    if (g.empty())
      continue;

    if (g.size() <= maxGroupSize)
    {
      split.push_back(g);
      continue;
    }

    for (size_t start = 0; start < g.size(); start += maxGroupSize)
    {
      const size_t end = std::min(start + static_cast<size_t>(maxGroupSize), g.size());
      split.emplace_back(g.begin() + static_cast<std::ptrdiff_t>(start), g.begin() + static_cast<std::ptrdiff_t>(end));
    }
  }

  return split;
}

static void printLODMetrics(
    int level,
    const std::vector<VirtualGeometryCluster> &clusters,
    const std::vector<std::vector<int32_t>> &groups,
    const std::vector<uint32_t> &remap,
    const std::vector<uint8_t> &locks,
    const std::vector<int32_t> &retry)
{
  std::vector<int32_t> parents(remap.size());
  int clusterCount = 0, triangles = 0, fullClusters = 0, components = 0, transformedVerts = 0, boundaryVerts = 0;
  for (const auto &g : groups)
    for (int32_t ci : g)
    {
      const VirtualGeometryCluster &c = clusters[ci];
      clusterCount++;
      triangles += static_cast<int>(c.indices.size() / 3);
      fullClusters += (c.indices.size() == ClusterSize * 3) ? 1 : 0;
      components += getConnectedComponents(parents, c.indices, remap);
      transformedVerts += getUniqueVertices(parents, c.indices);
      boundaryVerts += getUniqueVertices(parents, c.indices, &locks);
    }

  int stuckC = 0, stuckT = 0;
  for (int32_t ci : retry)
  {
    stuckC++;
    stuckT += static_cast<int>(clusters[ci].indices.size() / 3);
  }

  double avgGroup = static_cast<double>(clusterCount) / static_cast<double>(groups.size());
  double invC = 1.0 / static_cast<double>(clusterCount);

  printf(
      "LOD %d: %d clusters, %zu groups (%.1f%% full, %.1f tri/cl, %.1f vtx/cl, %.2f connected, %.1f boundary, %.1f partition), %d triangles",
      level,
      clusterCount,
      groups.size(),
      static_cast<double>(fullClusters) * invC * 100,
      static_cast<double>(triangles) * invC,
      static_cast<double>(transformedVerts) * invC,
      static_cast<double>(components) * invC,
      static_cast<double>(boundaryVerts) * invC,
      avgGroup,
      triangles);
  if (stuckC > 0)
    printf("; stuck %d clusters (%d triangles)", stuckC, stuckT);
  printf("\n");
}

static VirtualGeometryBuildData buildSingleShape(
    std::vector<VirtualGeometryCluster> inputClusters,
    const rendering::animation::Skeleton *skeleton)
{
  VirtualGeometryBuildData emptyResult;
  if (inputClusters.empty())
    return emptyResult;

  std::vector<Vertex> globalVertices;
  std::vector<uint32_t> allGlobalIndices;

  {
    uint32_t vertexOffset = 0;
    for (const auto &ic : inputClusters)
    {
      globalVertices.insert(globalVertices.end(), ic.vertices.begin(), ic.vertices.end());
      for (uint32_t li : ic.indices)
        allGlobalIndices.push_back(vertexOffset + li);
      vertexOffset += static_cast<uint32_t>(ic.vertices.size());
    }
  }

  std::vector<uint32_t> remap(globalVertices.size());
  if (!globalVertices.empty())
  {
    meshopt_Stream posStream = {&globalVertices[0].pos[0], sizeof(float) * 3, sizeof(Vertex)};
    meshopt_generateVertexRemapMulti(remap.data(), allGlobalIndices.data(), allGlobalIndices.size(), globalVertices.size(), &posStream, 1);
  }

  std::vector<VirtualGeometryCluster> clusters;
  clusters.reserve(inputClusters.size());
  {
    uint32_t vertexOffset = 0;
    for (auto &ic : inputClusters)
    {
      VirtualGeometryCluster c;
      c.groupId = ic.groupId;
      c.meshPartIndex = ic.meshPartIndex;
      c.self = ic.self;
      c.parent = ic.parent;
      c.cone = ic.cone;
      if (!ic.vertices.empty() && !ic.indices.empty())
        c.cone = computeClusterCone(ic.vertices, ic.indices);
      c.indices.reserve(ic.indices.size());
      for (uint32_t li : ic.indices)
        c.indices.push_back(vertexOffset + li);
      clusters.push_back(std::move(c));
      vertexOffset += static_cast<uint32_t>(ic.vertices.size());
    }
  }

  printf("Ideal LOD chain: %.1f levels\n", log2(static_cast<double>(allGlobalIndices.size() / 3) / static_cast<double>(ClusterSize)));

  std::vector<uint8_t> locks(globalVertices.size());
  std::vector<int32_t> pending(clusters.size());
  for (uint32_t i = 0; i < clusters.size(); ++i)
    pending[i] = static_cast<int32_t>(i);

  std::vector<ClusterGroupInfo> groupInfos;
  std::vector<std::vector<std::vector<int32_t>>> groupsPerLevel;

  int depth = 0;

  while (pending.size() > 1)
  {
    std::vector<std::vector<int32_t>> groups = VirtualGeometryPartitioner::partition(clusters, pending, remap);
    groups = splitGroupsByLimit(groups, MaxBVHChildren);

    VirtualGeometryPartitioner::lockBoundaryVertices(locks, groups, clusters, remap);

    pending.clear();
    std::vector<int32_t> retry;
    std::vector<std::vector<int32_t>> finalizedGroups;
    uint64_t triangles = 0, stuckTriangles = 0;

    for (uint64_t i = 0; i < groups.size(); ++i)
    {
      if (groups[i].empty())
        continue;

      if (groups[i].size() == 1)
      {
        stuckTriangles += clusters[groups[i][0]].indices.size() / 3;
        retry.push_back(groups[i][0]);
        continue;
      }

      std::vector<uint32_t> merged;
      for (int32_t ci : groups[i])
        merged.insert(merged.end(), clusters[ci].indices.begin(), clusters[ci].indices.end());

      uint64_t targetSize = (merged.size() / 3) / 2 * 3;
      float error = 0.f;
      std::vector<Vertex> localVerts;
      std::vector<uint32_t> simplified = VirtualGeometrySimplifier::simplify(globalVertices, localVerts, merged, &locks, targetSize, &error);

      if (simplified.size() > merged.size() * SimplifyThreshold || simplified.size() / (ClusterSize * 3) >= merged.size() / (ClusterSize * 3))
      {
        float sloppyError = 0.f;
        std::vector<Vertex> sloppyVerts;
        std::vector<uint32_t> sloppyIndices = VirtualGeometrySimplifier::simplifySloppy(globalVertices, sloppyVerts, merged, &locks, targetSize, &sloppyError);

        if (sloppyIndices.size() / (ClusterSize * 3) < merged.size() / (ClusterSize * 3))
        {
          simplified = std::move(sloppyIndices);
          localVerts = std::move(sloppyVerts);
          error = sloppyError;
        }
        else
        {
          stuckTriangles += merged.size() / 3;
          for (int32_t ci : groups[i])
            retry.push_back(ci);
          continue;
        }
      }

      LODBounds groupBounds = mergeClusterGroupBounds(clusters, groups[i]);
      groupBounds.error += error;

      for (int32_t ci : groups[i])
      {
        assert(clusters[ci].parent.error == std::numeric_limits<float>::max());
        clusters[ci].parent = groupBounds;
        clusters[ci].groupId = static_cast<uint32_t>(groupInfos.size());
      }

      groupInfos.emplace_back();
      groupInfos.back().lodLevel = depth;
      groupInfos.back().originalClusterIndices = groups[i];

      const std::vector<uint32_t> candidateBones = collectCandidateBonesFromClusterGroup(globalVertices, clusters, groups[i], skeleton);
      std::vector<VirtualGeometryCluster> newClusters = VirtualGeometryPartitioner::clusterize(localVerts, simplified);
      for (auto &nc : newClusters)
        nc.cone = computeClusterCone(localVerts, nc.indices);
      for (auto &nc : newClusters)
        nc.meshPartIndex = chooseDominantBoneFromVertices(localVerts, nc.indices, skeleton, &candidateBones, static_cast<uint32_t>(depth + 1));

      {
        uint32_t offset = static_cast<uint32_t>(globalVertices.size());
        globalVertices.insert(globalVertices.end(), localVerts.begin(), localVerts.end());
        locks.resize(globalVertices.size(), 0);
        for (auto &nc : newClusters)
          for (uint32_t &idx : nc.indices)
            idx += offset;
      }

      std::vector<int32_t> simplifiedClusters;
      for (auto &nc : newClusters)
      {
        nc.self = groupBounds;
        nc.groupId = static_cast<uint32_t>(-1);

        const uint32_t clusterId = static_cast<uint32_t>(clusters.size());
        simplifiedClusters.push_back(static_cast<int32_t>(clusterId));

        clusters.push_back(nc);
        pending.push_back(static_cast<int32_t>(clusterId));
        triangles += nc.indices.size() / 3;
      }

      rebuildPositionRemap(globalVertices, clusters, remap);

      groupInfos.back().simplifiedClusterIndices = simplifiedClusters;
      finalizedGroups.push_back(groups[i]);
    }

    if (!finalizedGroups.empty())
      groupsPerLevel.push_back(finalizedGroups);

    printLODMetrics(depth, clusters, finalizedGroups.empty() ? groups : finalizedGroups, remap, locks, retry);
    ++depth;

    pending.insert(pending.end(), retry.begin(), retry.end());

    if (triangles < stuckTriangles / 3)
      break;
  }

  if (pending.size() > 1)
  {
    std::vector<int32_t> rootClusters(pending.begin(), pending.end());

    LODBounds rootBounds = mergeClusterGroupBounds(clusters, rootClusters);
    rootBounds.error = std::numeric_limits<float>::max();

    std::vector<std::vector<int32_t>> rootGroup = splitGroupsByLimit({rootClusters}, MaxBVHChildren);
    for (const auto &rg : rootGroup)
    {
      const uint32_t groupId = static_cast<uint32_t>(groupInfos.size());
      for (int32_t ci : rg)
      {
        assert(clusters[ci].groupId == static_cast<uint32_t>(-1));
        clusters[ci].parent = rootBounds;
        clusters[ci].groupId = groupId;
      }

      groupInfos.emplace_back();
      groupInfos.back().lodLevel = depth;
      groupInfos.back().originalClusterIndices = rg;
    }

    groupsPerLevel.push_back(rootGroup);
    printLODMetrics(depth, clusters, rootGroup, remap, locks, {});
    ++depth;

    pending.clear();
  }

  while (!pending.empty())
  {
    const auto clusterId = pending.back();
    auto &cluster = clusters[clusterId];
    pending.pop_back();

    assert(cluster.groupId == static_cast<uint32_t>(-1));

    std::vector<std::vector<int32_t>> singleGroup = {{clusterId}};

    cluster.parent = cluster.self;
    cluster.parent.error = std::numeric_limits<float>::max();
    cluster.groupId = static_cast<uint32_t>(groupInfos.size());

    groupInfos.emplace_back();
    groupInfos.back().lodLevel = depth;
    groupInfos.back().originalClusterIndices = {clusterId};

    groupsPerLevel.push_back(singleGroup);

    printLODMetrics(depth, clusters, singleGroup, remap, locks, {});
  }

  return VirtualGeometryHierarchyBuilder::buildHierarchy(globalVertices, clusters, groupInfos, groupsPerLevel, skeleton);
}

struct HierarchyRef
{
  enum class Kind : uint8_t
  {
    Synthetic,
    PartNode,
  };

  Kind kind = Kind::Synthetic;
  uint32_t index = 0u;
  uint32_t localNodeIndex = 0u;

  static HierarchyRef synthetic(uint32_t syntheticIndex)
  {
    HierarchyRef ref;
    ref.kind = Kind::Synthetic;
    ref.index = syntheticIndex;
    return ref;
  }

  static HierarchyRef partNode(uint32_t partIndex, uint32_t nodeIndex)
  {
    HierarchyRef ref;
    ref.kind = Kind::PartNode;
    ref.index = partIndex;
    ref.localNodeIndex = nodeIndex;
    return ref;
  }
};

struct SyntheticHierarchyNode
{
  std::vector<HierarchyRef> children;
};

static VirtualGeometryHierarchy makeSyntheticHierarchyNode()
{
  VirtualGeometryHierarchy node{};
  const float inf = std::numeric_limits<float>::infinity();
  node.max_x = inf;
  node.max_y = inf;
  node.max_z = inf;
  node.min_x = -inf;
  node.min_y = -inf;
  node.min_z = -inf;
  node.max_center_x = 0.0f;
  node.max_center_y = 0.0f;
  node.max_center_z = 0.0f;
  node.max_radius = inf;
  node.min_lod_error = 0.0f;
  node.max_parent_lod_error = inf;
  node.child_start = UINT32_MAX;
  node.child_count = 0u;
  node.pageIndex = UINT32_MAX;
  node.meshPartIndex = UINT32_MAX;
  node.flags = HIERARCHY_FORCE_TRAVERSAL_FLAG;
  return node;
}

static std::vector<ShapeBuildInput> wrapShapesAsBuildInputs(const std::vector<Shape> &shapes)
{
  std::vector<ShapeBuildInput> wrapped;
  wrapped.reserve(shapes.size());
  for (const Shape &shape : shapes)
  {
    ShapeBuildInput input;
    input.meshParts.push_back(MeshPart{shape, UINT32_MAX});
    wrapped.push_back(std::move(input));
  }
  return wrapped;
}

static VirtualGeometryBuildData mergeMeshPartsIntoSingleShape(
    std::vector<VirtualGeometryBuildData> &&partBuilds,
    const ShapeBuildInput &shapeInput)
{
  VirtualGeometryBuildData merged;
  merged.shapes.resize(1u);
  merged.shapes[0].materialIndex = shapeInput.materialIndex;

  if (partBuilds.empty())
    return merged;

  if (partBuilds.size() == 1u)
    return std::move(partBuilds.front());

  uint32_t clusterOffset = 0u;
  uint32_t groupOffset = 0u;
  uint32_t buildToLinearizedOffset = 0u;

  std::vector<uint32_t> partClusterOffsets;
  partClusterOffsets.reserve(partBuilds.size());

  std::vector<SyntheticHierarchyNode> syntheticNodes;
  std::vector<HierarchyRef> currentLevel;
  currentLevel.reserve(partBuilds.size());

  for (uint32_t partIndex = 0u; partIndex < partBuilds.size(); ++partIndex)
    currentLevel.push_back(HierarchyRef::partNode(partIndex, 0u));

  while (currentLevel.size() > 1u)
  {
    std::vector<HierarchyRef> nextLevel;
    for (size_t start = 0; start < currentLevel.size(); start += MaxBVHChildren)
    {
      SyntheticHierarchyNode node;
      const size_t end = std::min(start + static_cast<size_t>(MaxBVHChildren), currentLevel.size());
      node.children.assign(currentLevel.begin() + static_cast<std::ptrdiff_t>(start), currentLevel.begin() + static_cast<std::ptrdiff_t>(end));
      const uint32_t syntheticIndex = static_cast<uint32_t>(syntheticNodes.size());
      syntheticNodes.push_back(std::move(node));
      nextLevel.push_back(HierarchyRef::synthetic(syntheticIndex));
    }
    currentLevel = std::move(nextLevel);
  }

  if (syntheticNodes.empty())
  {
    SyntheticHierarchyNode syntheticRoot;
    syntheticRoot.children.push_back(HierarchyRef::partNode(0u, 0u));
    syntheticNodes.push_back(std::move(syntheticRoot));
    currentLevel = {HierarchyRef::synthetic(0u)};
  }

  const HierarchyRef rootRef = currentLevel.front();

  for (size_t partIndex = 0; partIndex < partBuilds.size(); ++partIndex)
  {
    VirtualGeometryBuildData &partBuild = partBuilds[partIndex];
    partClusterOffsets.push_back(clusterOffset);
    merged.meshParts.push_back(MeshPartInfo{shapeInput.meshParts[partIndex].dominantBoneIndex});

    for (auto cluster : partBuild.clusters)
    {
      if (cluster.groupId != static_cast<uint32_t>(-1))
        cluster.groupId += groupOffset;
      merged.clusters.push_back(std::move(cluster));
    }

    for (auto info : partBuild.groupInfos)
    {
      for (int32_t &idx : info.originalClusterIndices)
        idx += static_cast<int32_t>(clusterOffset);
      for (int32_t &idx : info.simplifiedClusterIndices)
        idx += static_cast<int32_t>(clusterOffset);
      merged.groupInfos.push_back(std::move(info));
    }

    const size_t mergedGroupBase = merged.groupDAG.size();
    merged.groupDAG.resize(mergedGroupBase + partBuild.groupDAG.size());
    for (size_t i = 0; i < partBuild.groupDAG.size(); ++i)
      for (uint32_t child : partBuild.groupDAG[i])
        merged.groupDAG[mergedGroupBase + i].push_back(groupOffset + child);

    const size_t mergedClusterBase = merged.clusterDAG.size();
    merged.clusterDAG.resize(mergedClusterBase + partBuild.clusterDAG.size());
    for (size_t i = 0; i < partBuild.clusterDAG.size(); ++i)
      for (uint32_t child : partBuild.clusterDAG[i])
        merged.clusterDAG[mergedClusterBase + i].push_back(clusterOffset + child);

    for (uint32_t idx : partBuild.buildToLinearizedClusterIndex)
      merged.buildToLinearizedClusterIndex.push_back(idx == UINT32_MAX ? UINT32_MAX : buildToLinearizedOffset + idx);

    clusterOffset += static_cast<uint32_t>(partBuild.clusters.size());
    groupOffset += static_cast<uint32_t>(partBuild.groupInfos.size());
    buildToLinearizedOffset += static_cast<uint32_t>(partBuild.clusters.size());
  }

  std::queue<HierarchyRef> pending;
  pending.push(rootRef);

  while (!pending.empty())
  {
    const HierarchyRef ref = pending.front();
    pending.pop();

    const uint32_t nodeIndex = static_cast<uint32_t>(merged.lodLevelHierarchy.size());
    merged.lodLevelHierarchy.push_back(VirtualGeometryHierarchy{});
    VirtualGeometryHierarchy &dst = merged.lodLevelHierarchy[nodeIndex];

    std::vector<HierarchyRef> children;
    if (ref.kind == HierarchyRef::Kind::Synthetic)
    {
      dst = makeSyntheticHierarchyNode();
      children = syntheticNodes[ref.index].children;
    }
    else
    {
      const VirtualGeometryBuildData &partBuild = partBuilds[ref.index];
      const VirtualGeometryHierarchy &src = partBuild.lodLevelHierarchy[ref.localNodeIndex];
      dst = src;

      if ((dst.flags & HIERARCHY_LEAF_FLAG) != 0u)
      {
        if (dst.child_start != UINT32_MAX)
          dst.child_start += partClusterOffsets[ref.index];
      }
      else
      {
        children.reserve(dst.child_count);
        for (uint32_t childIndex = 0u; childIndex < dst.child_count; ++childIndex)
          children.push_back(HierarchyRef::partNode(ref.index, src.child_start + childIndex));
      }
    }

    if (!children.empty())
    {
      dst.child_start = static_cast<uint32_t>(merged.lodLevelHierarchy.size() + pending.size());
      dst.child_count = static_cast<uint32_t>(children.size());
      for (const HierarchyRef &childRef : children)
        pending.push(childRef);
    }
  }

  merged.shapes[0].root_node_index = 0u;
  merged.shapes[0].hierarchy_node_count = static_cast<uint32_t>(merged.lodLevelHierarchy.size());
  return merged;
}

static VirtualGeometryBuildData mergeIndependentShapes(
    std::vector<VirtualGeometryBuildData> &&shapeBuilds,
    const VirtualGeometryBuildSettings &settings,
    bool remapMeshPartIndices)
{
  VirtualGeometryBuildData merged;
  merged.buildSettings = settings;

  uint32_t clusterOffset = 0u;
  uint32_t hierarchyOffset = 0u;
  uint32_t groupOffset = 0u;
  uint32_t pageOffset = 0u;
  uint32_t buildToLinearizedOffset = 0u;
  uint32_t meshPartOffset = 0u;

  for (auto &shapeBuild : shapeBuilds)
  {
    for (auto shapeInfo : shapeBuild.shapes)
    {
      shapeInfo.root_node_index += hierarchyOffset;
      if (shapeInfo.root_page_index != UINT32_MAX)
        shapeInfo.root_page_index += pageOffset;
      merged.shapes.push_back(shapeInfo);
    }

    merged.meshParts.insert(merged.meshParts.end(), shapeBuild.meshParts.begin(), shapeBuild.meshParts.end());
    if (merged.skeleton.empty() && !shapeBuild.skeleton.empty())
      merged.skeleton = shapeBuild.skeleton;

    for (auto cluster : shapeBuild.clusters)
    {
      if (cluster.groupId != static_cast<uint32_t>(-1))
        cluster.groupId += groupOffset;
      if (cluster.meshPartIndex != UINT32_MAX && remapMeshPartIndices)
        cluster.meshPartIndex += meshPartOffset;
      merged.clusters.push_back(std::move(cluster));
    }

    for (auto node : shapeBuild.lodLevelHierarchy)
    {
      if (node.meshPartIndex != UINT32_MAX && remapMeshPartIndices)
        node.meshPartIndex += meshPartOffset;
      if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
      {
        if (node.child_start != UINT32_MAX)
          node.child_start += hierarchyOffset;
      }
      else
      {
        if (node.pageIndex == UINT32_MAX)
        {
          if (node.child_start != UINT32_MAX)
            node.child_start += clusterOffset;
        }
        else
        {
          node.pageIndex = (node.pageIndex & kPageNotInstalledBit) | ((node.pageIndex & ~kPageNotInstalledBit) + pageOffset);
        }
      }
      merged.lodLevelHierarchy.push_back(node);
    }

    for (auto info : shapeBuild.groupInfos)
    {
      for (auto &idx : info.originalClusterIndices)
        idx += static_cast<int32_t>(clusterOffset);
      for (auto &idx : info.simplifiedClusterIndices)
        idx += static_cast<int32_t>(clusterOffset);
      merged.groupInfos.push_back(std::move(info));
    }

    const size_t mergedGroupBase = merged.groupDAG.size();
    merged.groupDAG.resize(mergedGroupBase + shapeBuild.groupDAG.size());
    for (size_t i = 0; i < shapeBuild.groupDAG.size(); ++i)
      for (uint32_t child : shapeBuild.groupDAG[i])
        merged.groupDAG[mergedGroupBase + i].push_back(groupOffset + child);

    const size_t mergedClusterBase = merged.clusterDAG.size();
    merged.clusterDAG.resize(mergedClusterBase + shapeBuild.clusterDAG.size());
    for (size_t i = 0; i < shapeBuild.clusterDAG.size(); ++i)
      for (uint32_t child : shapeBuild.clusterDAG[i])
        merged.clusterDAG[mergedClusterBase + i].push_back(clusterOffset + child);

    for (uint32_t idx : shapeBuild.buildToLinearizedClusterIndex)
      merged.buildToLinearizedClusterIndex.push_back(idx == UINT32_MAX ? UINT32_MAX : buildToLinearizedOffset + idx);

    for (auto page : shapeBuild.pages)
    {
      page.clusterOffset += clusterOffset;
      page.hierarchyOffset += hierarchyOffset;
      for (VirtualGeometryBuildPage::GroupSpan &groupSpan : page.groups)
        if (groupSpan.globalGroupId != UINT32_MAX)
          groupSpan.globalGroupId += groupOffset;
      for (uint32_t &dependency : page.dependencies)
        dependency += pageOffset;
      for (HierarchyClusterFlagsUpdate &update : page.installUpdates.hierarchyUpdates)
        update.hierarchyNodeIndex += hierarchyOffset;
      for (HierarchyClusterFlagsUpdate &update : page.uninstallUpdates.hierarchyUpdates)
        update.hierarchyNodeIndex += hierarchyOffset;
      merged.pages.push_back(std::move(page));
    }

    clusterOffset += static_cast<uint32_t>(shapeBuild.clusters.size());
    hierarchyOffset += static_cast<uint32_t>(shapeBuild.lodLevelHierarchy.size());
    groupOffset += static_cast<uint32_t>(shapeBuild.groupInfos.size());
    pageOffset += static_cast<uint32_t>(shapeBuild.pages.size());
    buildToLinearizedOffset += static_cast<uint32_t>(shapeBuild.clusters.size());
    meshPartOffset += static_cast<uint32_t>(shapeBuild.meshParts.size());
  }

  return merged;
}

std::vector<VirtualGeometryCluster> VirtualGeometryBuilder::buildLOD0Clusters(
    std::vector<Vertex> &vertices,
    const Shape &shape,
    const rendering::animation::Skeleton *skeleton)
{
  std::vector<VirtualGeometryCluster> clusters = VirtualGeometryPartitioner::clusterize(vertices, shape.indices);

  for (auto &c : clusters)
    c.groupId = UINT32_MAX;

  for (auto &c : clusters)
    c.self = computeBounds(vertices, c.indices, 0.f);

  for (auto &c : clusters)
    c.cone = computeClusterCone(vertices, c.indices);

  for (auto &c : clusters)
  {
    std::map<uint32_t, uint32_t> vertexMap;
    std::vector<Vertex> localVerts;

    for (uint32_t gi : c.indices)
      if (vertexMap.find(gi) == vertexMap.end())
      {
        vertexMap[gi] = static_cast<uint32_t>(localVerts.size());
        localVerts.push_back(vertices[gi]);
      }

    std::vector<uint32_t> localIndices;
    localIndices.reserve(c.indices.size());
    for (uint32_t gi : c.indices)
      localIndices.push_back(vertexMap[gi]);

    c.indices = std::move(localIndices);
    c.vertices = std::move(localVerts);
    c.meshPartIndex = chooseDominantBoneFromVertices(c.vertices, c.indices, skeleton, nullptr, 0u);
  }

  return clusters;
}

VirtualGeometryBuildData VirtualGeometryBuilder::build(std::vector<VirtualGeometryCluster> inputClusters, const VirtualGeometryBuildSettings &settings)
{
  VirtualGeometryBuildData result = buildSingleShape(std::move(inputClusters), nullptr);
  VirtualGeometryShapeInfo shapeInfo{};
  shapeInfo.root_node_index = 0u;
  shapeInfo.root_page_index = 0u;
  shapeInfo.hierarchy_node_count = static_cast<uint32_t>(result.lodLevelHierarchy.size());
  result.shapes.push_back(shapeInfo);
  result.meshParts.push_back(MeshPartInfo{UINT32_MAX});
  VirtualGeometryHierarchyBuilder::finalizePagesAndRewrites(result, settings);
  return result;
}

VirtualGeometryBuildData VirtualGeometryBuilder::build(
    std::vector<Vertex> &vertices,
    const std::vector<Shape> &shapes,
    const VirtualGeometryBuildSettings &settings)
{
  return build(vertices, wrapShapesAsBuildInputs(shapes), settings);
}

VirtualGeometryBuildData VirtualGeometryBuilder::build(
    std::vector<Vertex> &vertices,
    const std::vector<ShapeBuildInput> &shapes,
    const VirtualGeometryBuildSettings &settings)
{
  if (shapes.empty())
    return {};

  std::vector<VirtualGeometryBuildData> shapeBuilds;
  shapeBuilds.reserve(shapes.size());
  const rendering::animation::Skeleton *sharedSkeleton = nullptr;

  for (const ShapeBuildInput &shapeInput : shapes)
  {
    if (sharedSkeleton == nullptr && shapeInput.skeleton != nullptr && !shapeInput.skeleton->empty())
      sharedSkeleton = shapeInput.skeleton;

    std::vector<VirtualGeometryBuildData> partBuilds;
    partBuilds.reserve(shapeInput.meshParts.size());

    for (const MeshPart &meshPart : shapeInput.meshParts)
    {
      std::vector<VirtualGeometryCluster> clusters = buildLOD0Clusters(vertices, meshPart.shape, shapeInput.skeleton);
      if (clusters.empty())
        continue;

      VirtualGeometryBuildData partBuild = buildSingleShape(std::move(clusters), shapeInput.skeleton);
      partBuild.shapes.push_back(VirtualGeometryShapeInfo{0u, 0u, static_cast<uint32_t>(partBuild.lodLevelHierarchy.size()), shapeInput.materialIndex});
      partBuilds.push_back(std::move(partBuild));
    }

    if (partBuilds.empty())
      continue;

    VirtualGeometryBuildData shapeBuild = mergeMeshPartsIntoSingleShape(std::move(partBuilds), shapeInput);
    shapeBuilds.push_back(std::move(shapeBuild));
  }

  VirtualGeometryBuildData merged = mergeIndependentShapes(
      std::move(shapeBuilds),
      settings,
      sharedSkeleton == nullptr || sharedSkeleton->empty());
  if (sharedSkeleton != nullptr && !sharedSkeleton->empty())
  {
    merged.skeleton = *sharedSkeleton;
    merged.meshParts = buildMeshPartsForSkeleton(sharedSkeleton);
  }
  else if (merged.meshParts.empty())
  {
    merged.meshParts.push_back(MeshPartInfo{UINT32_MAX});
  }

  VirtualGeometryHierarchyBuilder::finalizePagesAndRewrites(merged, settings);
  return merged;
}

} // namespace virtualgeometry
