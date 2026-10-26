// The virtual geometry payload contract, applied to both producers.
//
// The renderer does not know where a DAG came from. Whether the pages were
// built by VirtualGeometryBuilder (cluster partitioning and meshoptimizer
// simplification over a triangle mesh) or by the voxel octree builder (every
// octree level meshed, groups taken from the octree), the culler, the page
// installer and the rewrites rely on the same structure:
//
//   - a leaf cluster carries self error 0 and a root carries an infinite
//     parent error, so a cut always has something to draw;
//   - along every DAG edge the child's parent bound is the parent's self bound
//     and errors never decrease towards the root, so the two-test cut is
//     crack-free and picks exactly one cluster per root-to-leaf path;
//   - clusters, groups and pages sit inside the fixed GPU budgets;
//   - the encoded meshlets carry the cluster bounds unchanged, and the
//     install rewrites keep every residency prefix a valid, hole-free cut.
//
// This suite runs one oracle over three payloads - a triangle heightfield
// through the generic builder, a synthetic voxel chunk and a generated terrain
// chunk through the octree builder - and prints the structural statistics side
// by side. The triangle data itself is not compared: the octree emits quads
// from occupancy while the generic builder simplifies with meshoptimizer, and
// nothing downstream depends on which.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "TerrainGenerationPhase0Utils.hpp"
#include "os/Logger.hpp"
#include "editor/virtualgeometry/TerrainTriangleVirtualGeometryBridge.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "editor/virtualgeometry/VirtualGeometryHierarchyBuilder.hpp"
#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"

namespace vg = virtualgeometry;
namespace svr = voxel::svraster;

namespace
{

constexpr uint32_t kPageNotInstalledBit = 1u << 31u;

int failures = 0;

void check(const bool condition, const std::string &message)
{
  os::Logger::logf("  %s %s", condition ? "PASSED" : "FAILED", message.c_str());
  failures += condition ? 0 : 1;
}

// ── payloads ────────────────────────────────────────────────────────────────

struct Payload
{
  std::string name;
  vg::VirtualGeometryBuildData build;
  vg::VirtualGeometryEncodedData encoded;
  bool valid = false;
};

std::string heightfieldOBJ(const uint32_t quadsPerSide)
{
  std::ostringstream obj;
  const uint32_t vertsPerSide = quadsPerSide + 1u;
  for (uint32_t y = 0u; y < vertsPerSide; ++y)
    for (uint32_t x = 0u; x < vertsPerSide; ++x)
    {
      const float fx = static_cast<float>(x);
      const float fy = static_cast<float>(y);
      const float height = 3.0f * std::sin(fx * 0.35f) * std::cos(fy * 0.27f) +
          0.5f * std::sin(fx * 1.3f + fy * 0.9f);
      obj << "v " << fx << ' ' << height << ' ' << fy << '\n';
    }
  for (uint32_t y = 0u; y < vertsPerSide; ++y)
    for (uint32_t x = 0u; x < vertsPerSide; ++x)
      obj << "vt " << (static_cast<float>(x) / static_cast<float>(quadsPerSide)) << ' '
          << (static_cast<float>(y) / static_cast<float>(quadsPerSide)) << '\n';
  obj << "o heightfield\n";
  for (uint32_t y = 0u; y < quadsPerSide; ++y)
    for (uint32_t x = 0u; x < quadsPerSide; ++x)
    {
      const uint32_t i0 = y * vertsPerSide + x + 1u;
      const uint32_t i1 = i0 + 1u;
      const uint32_t i2 = i0 + vertsPerSide;
      const uint32_t i3 = i2 + 1u;
      obj << "f " << i0 << '/' << i0 << ' ' << i1 << '/' << i1 << ' ' << i3 << '/' << i3 << '\n';
      obj << "f " << i0 << '/' << i0 << ' ' << i3 << '/' << i3 << ' ' << i2 << '/' << i2 << '\n';
    }
  return obj.str();
}

Payload buildTrianglePayload(const vg::VirtualGeometryBuildSettings &settings)
{
  Payload payload{};
  payload.name = "triangle heightfield (VirtualGeometryBuilder)";
  std::vector<vg::Vertex> vertices;
  vg::Shape shape;
  vg::VirtualGeometryEncoder::loadOBJ(heightfieldOBJ(96u), false, vertices, shape);
  const bool previousLogging = vg::virtualGeometryBuildLoggingEnabled();
  vg::setVirtualGeometryBuildLogging(false);
  payload.build = vg::VirtualGeometryBuilder::build(
      vg::VirtualGeometryBuilder::buildLOD0Clusters(vertices, shape), settings);
  vg::setVirtualGeometryBuildLogging(previousLogging);
  payload.encoded = vg::VirtualGeometryCompressor::encode(payload.build, vg::QuantizationConfig{});
  payload.valid = !payload.build.clusters.empty() && !payload.encoded.pages.empty();
  return payload;
}

uint32_t columnHeight(const uint32_t x, const uint32_t z, const uint32_t side)
{
  uint32_t height = 4u;
  if (x >= side / 2u)
    height += 6u;
  if (z >= side * 3u / 4u)
    height += 3u;
  if (x == side / 4u && z == side / 4u)
    height += 9u;
  if (z > side / 3u && z < side / 3u + 4u)
    height += 5u + (x % 3u);
  return height;
}

Payload buildVoxelPayload(const vg::VirtualGeometryBuildSettings &settings)
{
  Payload payload{};
  payload.name = "synthetic voxel chunk (octree builder)";
  constexpr uint32_t side = 64u;
  std::vector<svr::SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<svr::SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> leaves;
  for (uint32_t x = 0u; x < side; ++x)
    for (uint32_t z = 0u; z < side; ++z)
    {
      const uint32_t top = columnHeight(x, z, side);
      for (uint32_t y = top > 3u ? top - 3u : 0u; y < top; ++y)
      {
        svr::SparseVoxelCenterLengthGPU cell{};
        cell.center = {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f};
        cell.length = 1.0f;
        svr::SparseVoxelMaterialGPU material{};
        material.albedoRGBA8 = 0xff7f9f5fu;
        material.materialId = (x / 8u + z / 8u) % 3u;
        leaves.push_back(static_cast<uint32_t>(centersLengths.size()));
        centersLengths.push_back(cell);
        materials.push_back(material);
      }
    }
  svr::SparseVoxelBrickHierarchy hierarchy{};
  svr::SparseVoxelBrickBuildSettings brickSettings{};
  brickSettings.voxelSize = 1.0f;
  brickSettings.maximumBricks = 1u << 16u;
  std::string error;
  if (!svr::buildSparseVoxelBrickHierarchy(centersLengths, materials, leaves, brickSettings, hierarchy, &error))
  {
    os::Logger::errorf("voxel chunk build failed: %s", error.c_str());
    return payload;
  }
  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  if (!svr::buildSparseVoxelOctreeVirtualGeometry(
          hierarchy, 0u, static_cast<uint32_t>(hierarchy.nodes.size()), octreeSettings, settings,
          payload.build, &stats, &error))
  {
    os::Logger::errorf("octree virtual geometry build failed: %s", error.c_str());
    return payload;
  }
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 100.0f;
  payload.encoded = vg::VirtualGeometryCompressor::encode(payload.build, quantization);
  payload.valid = !payload.build.clusters.empty() && !payload.encoded.pages.empty();
  return payload;
}

Payload buildTerrainPayload(const vg::VirtualGeometryBuildSettings &settings)
{
  Payload payload{};
  payload.name = "generated terrain chunk (octree builder)";
  std::string error;
  const auto bundle = terrain_phase0::makeFixture(terrain_phase0::FixtureKind::Mountain);
  const auto fixtureSettings = terrain_phase0::fixtureSettings(terrain_phase0::FixtureKind::Mountain, 0u);
  svr::PlanetVoxelChunkSet chunks;
  svr::WorldMapVoxelGenerationMetrics metrics{};
  if (!svr::buildWorldMapVoxelTerrain(bundle, fixtureSettings, chunks, &error, {}, nullptr, &metrics) ||
      chunks.chunks.empty())
  {
    os::Logger::errorf("terrain generation failed: %s", error.c_str());
    return payload;
  }
  const auto &record = chunks.chunks[0];
  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  std::array<double, 3> pageOrigin{};
  svr::SparseVoxelTerrainOctreeVirtualGeometryBuildStats bridgeStats{};
  if (!svr::buildSparseVoxelTerrainOctreeVirtualGeometryAsset(
          chunks, 0u, octreeSettings, settings, quantization, payload.encoded, &pageOrigin, &bridgeStats, &error))
  {
    os::Logger::errorf("terrain virtual geometry build failed: %s", error.c_str());
    return payload;
  }
  octreeSettings.originOffset = pageOrigin;
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  if (!svr::buildSparseVoxelOctreeVirtualGeometry(
          chunks.bricks, record.brickNodeOffset, record.brickNodeCapacity, octreeSettings, settings,
          payload.build, &stats, &error))
  {
    os::Logger::errorf("terrain source build failed: %s", error.c_str());
    return payload;
  }
  payload.valid = !payload.build.clusters.empty() && !payload.encoded.pages.empty();
  return payload;
}

// ── residency model: what the runtime does with the rewrites ────────────────

uint8_t fullMaskForCount(const uint32_t count)
{
  if (count == 0u)
    return 0u;
  return count >= 8u ? 0xffu : static_cast<uint8_t>((1u << count) - 1u);
}

struct Residency
{
  const vg::VirtualGeometryBuildData &build;
  std::vector<uint8_t> installed;
  std::vector<vg::VirtualGeometryHierarchy> hierarchy;
  std::vector<uint32_t> clusterNode;
  std::vector<uint8_t> clusterBit;
  std::vector<uint32_t> clusterPage;
  std::vector<uint32_t> clusterParent;
  std::vector<uint32_t> leafClusters;

  explicit Residency(const vg::VirtualGeometryBuildData &buildData)
      : build(buildData),
        installed(buildData.pages.size(), 0u),
        hierarchy(buildData.lodLevelHierarchy),
        clusterNode(buildData.clusters.size(), UINT32_MAX),
        clusterBit(buildData.clusters.size(), 0u),
        clusterPage(buildData.clusters.size(), UINT32_MAX),
        clusterParent(buildData.clusters.size(), UINT32_MAX)
  {
    for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.size(); ++nodeIndex)
    {
      const auto &node = build.lodLevelHierarchy[nodeIndex];
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
        continue;
      const uint32_t page = node.pageIndex & ~kPageNotInstalledBit;
      for (uint32_t local = 0u; local < node.child_count; ++local)
      {
        const uint32_t cluster = build.pages[page].clusterOffset + node.child_start + local;
        if (cluster >= clusterNode.size())
          continue;
        clusterNode[cluster] = nodeIndex;
        clusterBit[cluster] = static_cast<uint8_t>(local);
        clusterPage[cluster] = page;
      }
    }
    for (uint32_t parent = 0u; parent < build.clusterDAG.size(); ++parent)
      for (const uint32_t child : build.clusterDAG[parent])
        clusterParent[child] = parent;
    for (uint32_t cluster = 0u; cluster < build.clusters.size(); ++cluster)
      if (build.clusterDAG[cluster].empty())
        leafClusters.push_back(cluster);
    applyRewrites();
  }

  static void setMasks(vg::VirtualGeometryHierarchy &node, const uint8_t streamingMask, const uint8_t enabledMask)
  {
    node.flags &= ~(vg::HIERARCHY_STREAMING_MASK_BITS | vg::HIERARCHY_ENABLED_MASK_BITS | vg::STREAMING_LEAF_FLAG);
    node.flags |= static_cast<uint32_t>(streamingMask) << vg::HIERARCHY_STREAMING_MASK_SHIFT;
    node.flags |= static_cast<uint32_t>(enabledMask) << vg::HIERARCHY_ENABLED_MASK_SHIFT;
    if (streamingMask != 0u)
      node.flags |= vg::STREAMING_LEAF_FLAG;
  }

  void applyRewrites()
  {
    std::set<uint32_t> rewritten;
    for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.size(); ++nodeIndex)
    {
      auto &node = hierarchy[nodeIndex];
      node.flags = build.lodLevelHierarchy[nodeIndex].flags;
      node.pageIndex = build.lodLevelHierarchy[nodeIndex].pageIndex;
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
        continue;
      const uint32_t page = node.pageIndex & ~kPageNotInstalledBit;
      node.pageIndex = installed[page] ? page : (kPageNotInstalledBit | page);
      setMasks(node, 0u, 0u);
    }
    for (uint32_t page = 0u; page < build.pages.size(); ++page)
    {
      for (const auto &update : build.pages[page].installUpdates.hierarchyUpdates)
        rewritten.insert(update.hierarchyNodeIndex);
      if (installed[page] == 0u)
        continue;
      for (const auto &update : build.pages[page].installUpdates.hierarchyUpdates)
        setMasks(hierarchy[update.hierarchyNodeIndex], update.streamingLeafsBitset, update.enabledClustersBitset);
    }
    for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.size(); ++nodeIndex)
    {
      auto &node = hierarchy[nodeIndex];
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
        continue;
      if ((node.pageIndex & kPageNotInstalledBit) != 0u || rewritten.count(nodeIndex) != 0u)
        continue;
      setMasks(node, 0u, fullMaskForCount(node.child_count));
    }
  }

  bool clusterResident(const uint32_t cluster) const
  {
    const uint32_t nodeIndex = clusterNode[cluster];
    return nodeIndex != UINT32_MAX && (hierarchy[nodeIndex].pageIndex & kPageNotInstalledBit) == 0u;
  }
  bool clusterEnabled(const uint32_t cluster) const
  {
    if (!clusterResident(cluster))
      return false;
    const auto &node = hierarchy[clusterNode[cluster]];
    const uint8_t mask = static_cast<uint8_t>((node.flags & vg::HIERARCHY_ENABLED_MASK_BITS) >> vg::HIERARCHY_ENABLED_MASK_SHIFT);
    return ((mask >> clusterBit[cluster]) & 1u) != 0u;
  }
  bool clusterStreaming(const uint32_t cluster) const
  {
    if (!clusterResident(cluster))
      return false;
    const auto &node = hierarchy[clusterNode[cluster]];
    const uint8_t mask = static_cast<uint8_t>((node.flags & vg::HIERARCHY_STREAMING_MASK_BITS) >> vg::HIERARCHY_STREAMING_MASK_SHIFT);
    return ((mask >> clusterBit[cluster]) & 1u) != 0u;
  }
  // culling_multipass.wgsl: self test with the streaming override, then the
  // parent test for every cluster.
  bool clusterDrawn(const uint32_t cluster, const float threshold) const
  {
    if (!clusterEnabled(cluster))
      return false;
    const auto &data = build.clusters[cluster];
    const float selfError = clusterStreaming(cluster) ? 0.0f : data.self.error;
    if (selfError >= threshold)
      return false;
    return data.parent.error >= threshold;
  }
};

std::vector<float> thresholdSweep(const vg::VirtualGeometryBuildData &build)
{
  float finest = std::numeric_limits<float>::max();
  float coarsest = 0.0f;
  for (uint32_t cluster = 0u; cluster < build.clusters.size(); ++cluster)
  {
    const float error = build.clusters[cluster].self.error;
    if (error > 0.0f)
      finest = std::min(finest, error);
    if (build.clusters[cluster].parent.error == std::numeric_limits<float>::max())
      coarsest = std::max(coarsest, error);
  }
  if (finest == std::numeric_limits<float>::max())
    finest = 1e-3f;
  std::vector<float> thresholds{finest * 0.5f};
  for (float value = finest; value <= coarsest * 4.0f + 1e-6f; value *= 1.5f)
    thresholds.push_back(value);
  thresholds.push_back(coarsest * 8.0f + 1.0f);
  return thresholds;
}

// ── the contract ────────────────────────────────────────────────────────────

struct ContractReport
{
  // structure
  uint32_t clustersOverTriangleBudget = 0u;
  uint32_t clustersOverVertexBudget = 0u;
  uint32_t emptyClusters = 0u;
  uint32_t groupsOverBudget = 0u;
  uint32_t pagesOverGroupBudget = 0u;
  uint32_t pagesOverClusterBudget = 0u;
  uint32_t clustersNotCoveredByPages = 0u;
  uint32_t leavesMixingGroups = 0u;
  uint32_t leavesOutsidePage = 0u;
  // errors and bounds
  uint32_t leavesWithNonZeroSelfError = 0u;
  uint32_t rootsWithFiniteParentError = 0u;
  uint32_t edgesWithMismatchedParentBound = 0u;
  uint32_t edgesWithNonMonotoneError = 0u;
  uint32_t edgesWithChildOutsideParentSphere = 0u;
  uint32_t nodesWithInvertedError = 0u;
  uint32_t leafNodesWithWrongErrors = 0u;
  uint32_t interiorNodesWithWrongErrors = 0u;
  uint32_t nodesUnreachable = 0u;
  // encoded
  uint32_t pagesWithMeshletCountMismatch = 0u;
  uint32_t meshletsWithMismatchedBounds = 0u;
  uint32_t meshletsEmpty = 0u;
  uint32_t meshletsWithBadGroupIndex = 0u;
  uint32_t encodedUpdateMismatches = 0u;
  // residency
  uint32_t prefixEnabledOnUninstalled = 0u;
  uint32_t prefixStreamingNotEnabled = 0u;
  uint32_t prefixEnabledWithDisabledParent = 0u;
  uint32_t prefixHoles = 0u;
  uint32_t prefixDoubleDraws = 0u;
  uint32_t prefixStreamingWithResidentChildren = 0u;
  uint32_t finalDisabledClusters = 0u;
  uint32_t finalStreamingClusters = 0u;
  // stats
  uint32_t clusterCount = 0u;
  uint32_t groupCount = 0u;
  uint32_t pageCount = 0u;
  uint32_t leafCount = 0u;
  uint32_t rootCount = 0u;
  uint32_t levelCount = 0u;
  uint32_t hierarchyNodes = 0u;
  std::map<uint32_t, std::pair<float, float>> errorRangePerLevel;
  std::map<uint32_t, uint32_t> clustersPerLevel;
  uint32_t thresholdsTested = 0u;
  uint32_t prefixesTested = 0u;
  uint32_t maxVerticesPerCluster = 0u;
  uint32_t maxTrianglesPerCluster = 0u;
  uint32_t overVertexBudgetLevel = UINT32_MAX;

  uint32_t violations() const
  {
    return clustersOverTriangleBudget + clustersOverVertexBudget + emptyClusters + groupsOverBudget +
        pagesOverGroupBudget + pagesOverClusterBudget + clustersNotCoveredByPages + leavesMixingGroups +
        leavesOutsidePage + leavesWithNonZeroSelfError + rootsWithFiniteParentError +
        edgesWithMismatchedParentBound + edgesWithNonMonotoneError + edgesWithChildOutsideParentSphere +
        nodesWithInvertedError + leafNodesWithWrongErrors + interiorNodesWithWrongErrors + nodesUnreachable +
        pagesWithMeshletCountMismatch + meshletsWithMismatchedBounds + meshletsEmpty + meshletsWithBadGroupIndex +
        encodedUpdateMismatches + prefixEnabledOnUninstalled + prefixStreamingNotEnabled +
        prefixEnabledWithDisabledParent + prefixHoles + prefixDoubleDraws + prefixStreamingWithResidentChildren +
        finalDisabledClusters + finalStreamingClusters;
  }
};

bool nearly(const float a, const float b)
{
  const float scale = std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
  return std::fabs(a - b) <= scale * 1e-4f;
}

ContractReport inspectPayload(const Payload &payload)
{
  ContractReport report{};
  const auto &build = payload.build;
  const auto &encoded = payload.encoded;
  const uint32_t clusterCount = static_cast<uint32_t>(build.clusters.size());
  report.clusterCount = clusterCount;
  report.groupCount = static_cast<uint32_t>(build.groupInfos.size());
  report.pageCount = static_cast<uint32_t>(build.pages.size());
  report.hierarchyNodes = static_cast<uint32_t>(build.lodLevelHierarchy.size());

  // structure
  for (const auto &cluster : build.clusters)
  {
    const uint32_t triangles = static_cast<uint32_t>(cluster.indices.size() / 3u);
    report.clustersOverTriangleBudget += triangles > vg::ClusterSize ? 1u : 0u;
    report.clustersOverVertexBudget += cluster.vertices.size() > vg::MaxVertices ? 1u : 0u;
    report.emptyClusters += triangles == 0u ? 1u : 0u;
    report.maxVerticesPerCluster = std::max(report.maxVerticesPerCluster, static_cast<uint32_t>(cluster.vertices.size()));
    report.maxTrianglesPerCluster = std::max(report.maxTrianglesPerCluster, triangles);
  }
  for (const auto &group : build.groupInfos)
    report.groupsOverBudget += group.originalClusterIndices.size() > vg::MaxBVHChildren ? 1u : 0u;
  std::vector<uint32_t> coverage(clusterCount, 0u);
  for (const auto &page : build.pages)
  {
    report.pagesOverGroupBudget += page.groups.size() > vg::MAX_GROUPS_PER_PAGE ? 1u : 0u;
    report.pagesOverClusterBudget += page.clusterCount > vg::MAX_GROUPS_PER_PAGE * vg::GroupSize ? 1u : 0u;
    for (uint32_t local = 0u; local < page.clusterCount; ++local)
      if (page.clusterOffset + local < clusterCount)
        ++coverage[page.clusterOffset + local];
  }
  for (const uint32_t count : coverage)
    report.clustersNotCoveredByPages += count == 1u ? 0u : 1u;

  // DAG parents and levels
  std::vector<uint32_t> clusterParent(clusterCount, UINT32_MAX);
  for (uint32_t parent = 0u; parent < build.clusterDAG.size(); ++parent)
    for (const uint32_t child : build.clusterDAG[parent])
      clusterParent[child] = parent;
  std::vector<uint32_t> clusterLevel(clusterCount, 0u);
  for (const auto &group : build.groupInfos)
  {
    report.levelCount = std::max(report.levelCount, group.lodLevel + 1u);
    for (const int32_t cluster : group.originalClusterIndices)
      if (cluster >= 0 && static_cast<uint32_t>(cluster) < clusterCount)
        clusterLevel[static_cast<uint32_t>(cluster)] = group.lodLevel;
  }
  for (uint32_t cluster = 0u; cluster < clusterCount; ++cluster)
  {
    const auto &data = build.clusters[cluster];
    const bool leaf = build.clusterDAG[cluster].empty();
    const bool root = clusterParent[cluster] == UINT32_MAX;
    report.leafCount += leaf ? 1u : 0u;
    report.rootCount += root ? 1u : 0u;
    if (leaf && data.self.error != 0.0f)
      ++report.leavesWithNonZeroSelfError;
    if (root && data.parent.error != std::numeric_limits<float>::max())
      ++report.rootsWithFiniteParentError;
    ++report.clustersPerLevel[clusterLevel[cluster]];
    if (data.vertices.size() > vg::MaxVertices && report.overVertexBudgetLevel == UINT32_MAX)
      report.overVertexBudgetLevel = clusterLevel[cluster];
    auto &range = report.errorRangePerLevel[clusterLevel[cluster]];
    if (report.clustersPerLevel[clusterLevel[cluster]] == 1u)
      range = {data.self.error, data.self.error};
    range.first = std::min(range.first, data.self.error);
    range.second = std::max(range.second, data.self.error);
    for (const uint32_t child : build.clusterDAG[cluster])
    {
      const auto &childData = build.clusters[child];
      if (!nearly(childData.parent.center[0], data.self.center[0]) ||
          !nearly(childData.parent.center[1], data.self.center[1]) ||
          !nearly(childData.parent.center[2], data.self.center[2]) ||
          !nearly(childData.parent.radius, data.self.radius) ||
          !nearly(childData.parent.error, data.self.error))
        ++report.edgesWithMismatchedParentBound;
      if (childData.self.error > data.self.error)
        ++report.edgesWithNonMonotoneError;
      const float dx = childData.self.center[0] - data.self.center[0];
      const float dy = childData.self.center[1] - data.self.center[1];
      const float dz = childData.self.center[2] - data.self.center[2];
      const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (distance + childData.self.radius > data.self.radius * (1.0f + 1e-3f) + 1e-3f)
        ++report.edgesWithChildOutsideParentSphere;
    }
  }

  // hierarchy nodes
  std::vector<uint8_t> reachable(build.lodLevelHierarchy.size(), 0u);
  {
    std::vector<uint32_t> pending{0u};
    while (!pending.empty())
    {
      const uint32_t nodeIndex = pending.back();
      pending.pop_back();
      if (nodeIndex >= reachable.size() || reachable[nodeIndex])
        continue;
      reachable[nodeIndex] = 1u;
      const auto &node = build.lodLevelHierarchy[nodeIndex];
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) != 0u)
        continue;
      for (uint32_t child = 0u; child < node.child_count; ++child)
        pending.push_back(node.child_start + child);
    }
  }
  for (uint32_t nodeIndex = 0u; nodeIndex < build.lodLevelHierarchy.size(); ++nodeIndex)
  {
    const auto &node = build.lodLevelHierarchy[nodeIndex];
    report.nodesUnreachable += reachable[nodeIndex] ? 0u : 1u;
    if (node.max_parent_lod_error < node.min_lod_error)
      ++report.nodesWithInvertedError;
    if ((node.flags & vg::HIERARCHY_LEAF_FLAG) != 0u)
    {
      if (node.pageIndex == UINT32_MAX)
        continue;
      const uint32_t page = node.pageIndex & ~kPageNotInstalledBit;
      if (page >= build.pages.size() || node.child_start + node.child_count > build.pages[page].clusterCount)
      {
        ++report.leavesOutsidePage;
        continue;
      }
      float minSelf = std::numeric_limits<float>::max();
      float maxParent = std::numeric_limits<float>::lowest();
      uint32_t groupId = UINT32_MAX;
      for (uint32_t local = 0u; local < node.child_count; ++local)
      {
        const uint32_t cluster = build.pages[page].clusterOffset + node.child_start + local;
        const auto &data = build.clusters[cluster];
        minSelf = std::min(minSelf, data.self.error);
        maxParent = std::max(maxParent, data.parent.error);
        if (groupId == UINT32_MAX)
          groupId = data.groupId;
        else if (groupId != data.groupId)
          ++report.leavesMixingGroups;
      }
      if (!nearly(minSelf, node.min_lod_error) || !nearly(maxParent, node.max_parent_lod_error))
        ++report.leafNodesWithWrongErrors;
    }
    else
    {
      float minSelf = std::numeric_limits<float>::max();
      float maxParent = std::numeric_limits<float>::lowest();
      for (uint32_t child = 0u; child < node.child_count; ++child)
      {
        const uint32_t childIndex = node.child_start + child;
        if (childIndex >= build.lodLevelHierarchy.size())
          continue;
        minSelf = std::min(minSelf, build.lodLevelHierarchy[childIndex].min_lod_error);
        maxParent = std::max(maxParent, build.lodLevelHierarchy[childIndex].max_parent_lod_error);
      }
      if (node.min_lod_error > minSelf * (1.0f + 1e-4f) + 1e-6f || node.max_parent_lod_error < maxParent)
        ++report.interiorNodesWithWrongErrors;
    }
  }

  // encoded pages
  if (encoded.pages.size() != build.pages.size())
    ++report.pagesWithMeshletCountMismatch;
  for (uint32_t pageIndex = 0u; pageIndex < std::min(encoded.pages.size(), build.pages.size()); ++pageIndex)
  {
    const auto &page = encoded.pages[pageIndex];
    const auto &buildPage = build.pages[pageIndex];
    if (page.meshlets.size() != buildPage.clusterCount)
    {
      ++report.pagesWithMeshletCountMismatch;
      continue;
    }
    if (page.installUpdates.hierarchyUpdates.size() != buildPage.installUpdates.hierarchyUpdates.size())
      ++report.encodedUpdateMismatches;
    for (uint32_t local = 0u; local < page.meshlets.size(); ++local)
    {
      const auto &meshlet = page.meshlets[local];
      const auto &cluster = build.clusters[buildPage.clusterOffset + local];
      report.meshletsEmpty += (meshlet.triangle_count == 0u || meshlet.vertex_count == 0u) ? 1u : 0u;
      if (!nearly(meshlet.self.error, cluster.self.error) || !nearly(meshlet.parent.error, cluster.parent.error) ||
          !nearly(meshlet.self.radius, cluster.self.radius) || !nearly(meshlet.parent.radius, cluster.parent.radius) ||
          !nearly(meshlet.self.center[0], cluster.self.center[0]) || !nearly(meshlet.parent.center[0], cluster.parent.center[0]))
        ++report.meshletsWithMismatchedBounds;
      if (meshlet.pageLocalGroupIndex >= buildPage.groups.size())
      {
        ++report.meshletsWithBadGroupIndex;
        continue;
      }
      const auto &span = buildPage.groups[meshlet.pageLocalGroupIndex];
      if (local < span.clusterOffset || local >= span.clusterOffset + span.clusterCount ||
          meshlet.clusterIndexInGroup != local - span.clusterOffset)
        ++report.meshletsWithBadGroupIndex;
    }
  }

  // residency prefixes: every dependency-closed prefix in page order is a
  // valid, hole-free cut at every threshold.
  Residency residency(build);
  const std::vector<float> thresholds = thresholdSweep(build);
  report.thresholdsTested = static_cast<uint32_t>(thresholds.size());
  for (uint32_t page = 0u; page < build.pages.size(); ++page)
  {
    residency.installed[page] = 1u;
    residency.applyRewrites();
    ++report.prefixesTested;
    for (uint32_t cluster = 0u; cluster < clusterCount; ++cluster)
    {
      const bool enabled = residency.clusterEnabled(cluster);
      const bool streaming = residency.clusterStreaming(cluster);
      if (enabled && residency.installed[residency.clusterPage[cluster]] == 0u)
        ++report.prefixEnabledOnUninstalled;
      if (streaming && !enabled)
        ++report.prefixStreamingNotEnabled;
      if (enabled && clusterParent[cluster] != UINT32_MAX && !residency.clusterEnabled(clusterParent[cluster]))
        ++report.prefixEnabledWithDisabledParent;
      if (streaming)
      {
        bool anyChildAbsent = build.clusterDAG[cluster].empty();
        for (const uint32_t child : build.clusterDAG[cluster])
          anyChildAbsent = anyChildAbsent || !residency.clusterEnabled(child);
        if (!anyChildAbsent)
          ++report.prefixStreamingWithResidentChildren;
      }
    }
    for (const float threshold : thresholds)
      for (const uint32_t leaf : residency.leafClusters)
      {
        uint32_t drawn = 0u;
        bool anyResident = false;
        for (uint32_t cursor = leaf; cursor != UINT32_MAX; cursor = clusterParent[cursor])
        {
          anyResident = anyResident || residency.clusterEnabled(cursor);
          drawn += residency.clusterDrawn(cursor, threshold) ? 1u : 0u;
        }
        if (drawn == 0u && anyResident)
          ++report.prefixHoles;
        else if (drawn > 1u)
          ++report.prefixDoubleDraws;
      }
  }
  for (uint32_t cluster = 0u; cluster < clusterCount; ++cluster)
  {
    report.finalDisabledClusters += residency.clusterEnabled(cluster) ? 0u : 1u;
    report.finalStreamingClusters += residency.clusterStreaming(cluster) ? 1u : 0u;
  }
  return report;
}

void logReport(const Payload &payload, const ContractReport &report)
{
  std::ostringstream levels;
  for (const auto &[level, count] : report.clustersPerLevel)
  {
    const auto range = report.errorRangePerLevel.at(level);
    levels << " L" << level << "=" << count << "[" << range.first << ".." << range.second << "]";
  }
  os::Logger::logf(
      "  %s: clusters=%u groups=%u pages=%u hierarchy=%u leaves=%u roots=%u levels=%u prefixes=%u thresholds=%u",
      payload.name.c_str(), report.clusterCount, report.groupCount, report.pageCount, report.hierarchyNodes,
      report.leafCount, report.rootCount, report.levelCount, report.prefixesTested, report.thresholdsTested);
  os::Logger::logf("    clusters per level (self error range):%s", levels.str().c_str());
  os::Logger::logf(
      "    structure detail: overTriangles=%u overVertices=%u empty=%u groupsOver=%u pagesOverGroups=%u "
      "pagesOverClusters=%u uncovered=%u leavesMixing=%u leavesOutside=%u",
      report.clustersOverTriangleBudget, report.clustersOverVertexBudget, report.emptyClusters,
      report.groupsOverBudget, report.pagesOverGroupBudget, report.pagesOverClusterBudget,
      report.clustersNotCoveredByPages, report.leavesMixingGroups, report.leavesOutsidePage);
  os::Logger::logf(
      "    cluster budgets: maxTriangles=%u/%u maxVertices=%u/%u firstOverVertexLevel=%d",
      report.maxTrianglesPerCluster, vg::ClusterSize, report.maxVerticesPerCluster,
      static_cast<uint32_t>(vg::MaxVertices),
      report.overVertexBudgetLevel == UINT32_MAX ? -1 : static_cast<int>(report.overVertexBudgetLevel));
  os::Logger::logf(
      "    violations: structure=%u errors/bounds=%u hierarchy=%u encoded=%u residency=%u",
      report.clustersOverTriangleBudget + report.clustersOverVertexBudget + report.emptyClusters +
          report.groupsOverBudget + report.pagesOverGroupBudget + report.pagesOverClusterBudget +
          report.clustersNotCoveredByPages + report.leavesMixingGroups + report.leavesOutsidePage,
      report.leavesWithNonZeroSelfError + report.rootsWithFiniteParentError + report.edgesWithMismatchedParentBound +
          report.edgesWithNonMonotoneError + report.edgesWithChildOutsideParentSphere,
      report.nodesWithInvertedError + report.leafNodesWithWrongErrors + report.interiorNodesWithWrongErrors +
          report.nodesUnreachable,
      report.pagesWithMeshletCountMismatch + report.meshletsWithMismatchedBounds + report.meshletsEmpty +
          report.meshletsWithBadGroupIndex + report.encodedUpdateMismatches,
      report.prefixEnabledOnUninstalled + report.prefixStreamingNotEnabled + report.prefixEnabledWithDisabledParent +
          report.prefixHoles + report.prefixDoubleDraws + report.prefixStreamingWithResidentChildren +
          report.finalDisabledClusters + report.finalStreamingClusters);
}

void checkContract(const Payload &payload, const ContractReport &report, const bool genericBuilder)
{
  const std::string prefix = payload.name + ": ";
  check(report.clustersOverTriangleBudget == 0u && report.emptyClusters == 0u,
        prefix + "every cluster is non-empty and inside the triangle budget");
  if (genericBuilder && report.clustersOverVertexBudget != 0u)
  {
    // Known defect of the generic simplification stage, outside the voxel
    // path: a simplified cluster can exceed MaxVertices (199 measured here),
    // which the page decoder's fixed per-cluster vertex slots cannot hold.
    // Reported rather than asserted so the octree contract stays the gate.
    os::Logger::logf(
        "  KNOWN  %sVirtualGeometryBuilder emitted %u cluster(s) over the vertex budget (max %u/%u at level %d)",
        prefix.c_str(), report.clustersOverVertexBudget, report.maxVerticesPerCluster,
        static_cast<uint32_t>(vg::MaxVertices),
        report.overVertexBudgetLevel == UINT32_MAX ? -1 : static_cast<int>(report.overVertexBudgetLevel));
  }
  else
    check(report.clustersOverVertexBudget == 0u, prefix + "every cluster is inside the vertex budget");
  check(report.groupsOverBudget == 0u, prefix + "no group holds more than eight clusters");
  check(report.pagesOverGroupBudget == 0u && report.pagesOverClusterBudget == 0u,
        prefix + "no page exceeds the group or cluster capacity");
  check(report.clustersNotCoveredByPages == 0u, prefix + "the pages partition the cluster list exactly");
  check(report.leavesMixingGroups == 0u && report.leavesOutsidePage == 0u,
        prefix + "every hierarchy leaf names one group inside its page");
  check(report.leavesWithNonZeroSelfError == 0u,
        prefix + "every DAG leaf carries self error 0 (" + std::to_string(report.leavesWithNonZeroSelfError) + " do not)");
  check(report.rootsWithFiniteParentError == 0u,
        prefix + "every DAG root carries an infinite parent error");
  check(report.edgesWithMismatchedParentBound == 0u,
        prefix + "along every DAG edge the child's parent bound is the parent's self bound (" +
            std::to_string(report.edgesWithMismatchedParentBound) + " mismatches)");
  check(report.edgesWithNonMonotoneError == 0u,
        prefix + "errors never decrease towards the root (" + std::to_string(report.edgesWithNonMonotoneError) + " edges do)");
  check(report.edgesWithChildOutsideParentSphere == 0u,
        prefix + "every parent sphere contains its children's spheres (" +
            std::to_string(report.edgesWithChildOutsideParentSphere) + " escape)");
  check(report.nodesWithInvertedError == 0u && report.leafNodesWithWrongErrors == 0u &&
            report.interiorNodesWithWrongErrors == 0u,
        prefix + "hierarchy node errors summarise their clusters and children");
  check(report.nodesUnreachable == 0u, prefix + "every hierarchy node is reachable from the root");
  check(report.pagesWithMeshletCountMismatch == 0u && report.meshletsEmpty == 0u,
        prefix + "every encoded page carries one non-empty meshlet per cluster");
  check(report.meshletsWithMismatchedBounds == 0u,
        prefix + "encoded meshlets carry the cluster self and parent bounds unchanged");
  check(report.meshletsWithBadGroupIndex == 0u, prefix + "encoded meshlets name their group and slot consistently");
  check(report.encodedUpdateMismatches == 0u, prefix + "encoded pages carry the install rewrites");
  check(report.prefixEnabledOnUninstalled == 0u && report.prefixStreamingNotEnabled == 0u,
        prefix + "no residency prefix enables an absent cluster or marks a disabled one streaming");
  check(report.prefixEnabledWithDisabledParent == 0u,
        prefix + "the enabled set is closed under DAG parents at every residency prefix");
  check(report.prefixStreamingWithResidentChildren == 0u,
        prefix + "a streaming mark only ever sits on a cluster with an absent child (" +
            std::to_string(report.prefixStreamingWithResidentChildren) + " wrong)");
  check(report.prefixHoles == 0u,
        prefix + "every residency prefix draws exactly one cluster on every path with resident geometry (" +
            std::to_string(report.prefixHoles) + " holes)");
  check(report.prefixDoubleDraws == 0u,
        prefix + "no residency prefix draws two clusters on one path (" +
            std::to_string(report.prefixDoubleDraws) + " double draws)");
  check(report.finalDisabledClusters == 0u && report.finalStreamingClusters == 0u,
        prefix + "with every page resident every cluster is enabled and nothing is on the border");
}

} // namespace

int main()
{
  os::Logger::start();
  os::Logger::log("Virtual geometry payload contract: generic builder vs voxel octree builder");
  os::Logger::log("=========================================================================");

  vg::VirtualGeometryBuildSettings settings{};
  // Small pages so the residency prefixes have real cross-page rewrites.
  settings.maxGroupsPerPage = 6u;
  settings.maxRootPageGroups = 1u;

  std::vector<Payload> payloads;
  payloads.push_back(buildTrianglePayload(settings));
  payloads.push_back(buildVoxelPayload(settings));
  payloads.push_back(buildTerrainPayload(settings));

  std::vector<ContractReport> reports;
  for (const Payload &payload : payloads)
  {
    check(payload.valid, payload.name + " builds");
    if (!payload.valid)
    {
      reports.push_back({});
      continue;
    }
    reports.push_back(inspectPayload(payload));
    logReport(payload, reports.back());
  }
  for (size_t index = 0u; index < payloads.size(); ++index)
    if (payloads[index].valid)
      checkContract(payloads[index], reports[index], index == 0u);

  // Voxel-specific: the octree doubles its cell per level, so the group error
  // ladder is monotone per level with a ratio near two, and the chain is more
  // than one level deep - the property the old triangle bridge lost.
  for (size_t index = 1u; index < payloads.size(); ++index)
  {
    if (!payloads[index].valid)
      continue;
    const ContractReport &report = reports[index];
    check(report.levelCount >= 3u, payloads[index].name + ": the octree DAG has at least three LOD levels");
    bool ladderMonotone = true;
    float previousMax = -1.0f;
    for (const auto &[level, range] : report.errorRangePerLevel)
    {
      if (level == 0u)
        continue;
      if (range.first < previousMax)
        ladderMonotone = false;
      previousMax = range.second;
    }
    check(ladderMonotone, payloads[index].name + ": each octree level's error range sits above the level below");
  }
  // The generic builder's contract is the same one; a multi-level chain is
  // expected from a connected heightfield too.
  if (payloads[0].valid)
    check(reports[0].levelCount >= 2u, payloads[0].name + ": the generic builder produced more than one LOD level");

  if (failures != 0)
  {
    os::Logger::errorf("%d payload contract checks failed", failures);
    os::Logger::shutdown();
    return 1;
  }
  os::Logger::log("all payload contract checks passed");
  os::Logger::shutdown();
  return 0;
}
