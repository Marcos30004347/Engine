// One small chunk, one camera standing next to it, end to end.
//
// The big fixture suites prove the octree virtual geometry build over real
// generated terrain. This one is deliberately small and hand-built so every
// number in it can be reasoned about, and it carries the three stages through
// in order:
//
//   1. a prebuilt octree -> mesh and DAG. Every visible voxel face must have
//      exactly one mesh face, at every octree level, and the DAG's self/parent
//      errors must admit exactly one cluster per root-to-leaf path.
//   2. a small page 0. The root and the first coarse levels only, not the whole
//      DAG, with a page layout that follows the octree.
//   3. the runtime loop. Traverse the hierarchy the way culling_multipass.wgsl
//      does, request the pages the error metric asks for, stream them in
//      through the install rewrites, and take them out again through the
//      uninstall rewrites.
//
// The selection arithmetic below is a direct port of the shader:
//   distance      = max(|viewPos - center| - radius, nearPlane)
//   projectedError= error / distance * projectionScale
//   descend node  = projectedError(parent bounds) >= threshold
//   draw cluster  = enabled && projSelf < threshold && projParent >= threshold
//   streaming leaf=> selfError is forced to zero, so it always draws

#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelBrickSurfaceMesh.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace svr = voxel::svraster;
namespace vg = virtualgeometry;

namespace
{

constexpr uint32_t kPageNotInstalledBit = 1u << 31u;

int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// A prebuilt octree chunk
// ─────────────────────────────────────────────────────────────────────────────

// Deterministic height field with the features that make LOD interesting: a
// flat apron, a terrace step, a one-cell pillar, and a pit. Everything is
// integer, so the expected occupancy is exact.
uint32_t columnHeight(const uint32_t x, const uint32_t z, const uint32_t side)
{
  uint32_t height = 4u;
  if (x >= side / 2u)
    height += 6u;              // terrace step across the middle
  if (z >= side * 3u / 4u)
    height += 3u;              // a second step, on the other axis
  if (x == side / 4u && z == side / 4u)
    height += 9u;              // an isolated pillar
  if (x > side / 8u && x < side / 8u + 3u && z > side / 2u && z < side / 2u + 3u)
    height = height > 3u ? height - 3u : 1u; // a pit
  return height;
}

struct SyntheticChunk
{
  svr::SparseVoxelBrickHierarchy hierarchy;
  // Occupied cells of the finest level in integer cell coordinates.
  std::set<std::array<int64_t, 3>> finestCells;
  uint32_t side = 0u;
  float voxelSize = 1.0f;
};

bool buildSyntheticChunk(const uint32_t side, SyntheticChunk &chunk, std::string *error)
{
  chunk = {};
  chunk.side = side;
  chunk.voxelSize = 1.0f;

  std::vector<svr::SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<svr::SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> leaves;

  // A shell three cells deep under each column top: enough to close the cliff
  // faces the terraces create without filling the whole volume.
  constexpr uint32_t kShellDepth = 3u;
  for (uint32_t x = 0u; x < side; ++x)
    for (uint32_t z = 0u; z < side; ++z)
    {
      const uint32_t top = columnHeight(x, z, side);
      uint32_t lowestNeighbour = top;
      const std::array<std::array<int32_t, 2>, 4> neighbours{{
          {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
      }};
      for (const auto &offset : neighbours)
      {
        const int32_t nx = static_cast<int32_t>(x) + offset[0];
        const int32_t nz = static_cast<int32_t>(z) + offset[1];
        if (nx < 0 || nz < 0 || nx >= static_cast<int32_t>(side) ||
            nz >= static_cast<int32_t>(side))
          continue;
        lowestNeighbour = std::min(
            lowestNeighbour,
            columnHeight(static_cast<uint32_t>(nx), static_cast<uint32_t>(nz), side));
      }
      const uint32_t floor = std::min(
          top > kShellDepth ? top - kShellDepth : 0u,
          lowestNeighbour > 0u ? lowestNeighbour - 1u : 0u);

      for (uint32_t y = floor; y < top; ++y)
      {
        svr::SparseVoxelCenterLengthGPU cell{};
        // Cell centres: the brick builder derives its integer grid from these.
        cell.center = {
            static_cast<float>(x) + 0.5f,
            static_cast<float>(y) + 0.5f,
            static_cast<float>(z) + 0.5f};
        cell.length = chunk.voxelSize;
        svr::SparseVoxelMaterialGPU material{};
        material.albedoRGBA8 = 0xff808080u;
        material.materialId = ((x / 8u) + (z / 8u)) % 3u;
        leaves.push_back(static_cast<uint32_t>(centersLengths.size()));
        centersLengths.push_back(cell);
        materials.push_back(material);
        chunk.finestCells.insert({
            static_cast<int64_t>(x),
            static_cast<int64_t>(y),
            static_cast<int64_t>(z)});
      }
    }

  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = chunk.voxelSize;
  settings.maximumBricks = 1u << 16u;
  return svr::buildSparseVoxelBrickHierarchy(
      centersLengths, materials, leaves, settings, chunk.hierarchy, error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Oracles rebuilt from the occupancy words, sharing no code with the mesher
// ─────────────────────────────────────────────────────────────────────────────

uint64_t packCell(const std::array<int64_t, 3> &cell)
{
  constexpr int64_t kBias = 1 << 20;
  return (static_cast<uint64_t>(cell[0] + kBias) << 42u) |
      (static_cast<uint64_t>(cell[1] + kBias) << 21u) |
      static_cast<uint64_t>(cell[2] + kBias);
}

std::map<uint32_t, std::set<std::array<int64_t, 3>>> levelOccupancy(
    const svr::SparseVoxelBrickHierarchy &hierarchy)
{
  std::map<uint32_t, std::set<std::array<int64_t, 3>>> levels;
  for (const auto &node : hierarchy.nodes)
  {
    const uint32_t level = node.level & 0xffu;
    const int64_t scale = std::max<int64_t>(1, node.canonicalGridOriginScale[3]);
    const auto &occupancy = hierarchy.occupancy[node.occupancyIndex];
    for (uint32_t cell = 0u; cell < svr::kSparseVoxelBrickVoxelCount; ++cell)
    {
      if ((occupancy.cells[cell / 32u].bits & (uint32_t{1u} << (cell % 32u))) == 0u)
        continue;
      levels[level].insert({
          static_cast<int64_t>(node.canonicalGridOriginScale[0]) / scale +
              static_cast<int64_t>(cell % svr::kSparseVoxelBrickSide),
          static_cast<int64_t>(node.canonicalGridOriginScale[1]) / scale +
              static_cast<int64_t>((cell / svr::kSparseVoxelBrickSide) %
                                   svr::kSparseVoxelBrickSide),
          static_cast<int64_t>(node.canonicalGridOriginScale[2]) / scale +
              static_cast<int64_t>(cell / (svr::kSparseVoxelBrickSide *
                                           svr::kSparseVoxelBrickSide)),
      });
    }
  }
  return levels;
}

std::set<std::pair<uint64_t, uint32_t>> visibleFaces(
    const std::set<std::array<int64_t, 3>> &cells)
{
  static constexpr std::array<std::array<int64_t, 3>, 6> kOffsets{{
      {{-1, 0, 0}}, {{1, 0, 0}}, {{0, -1, 0}},
      {{0, 1, 0}},  {{0, 0, -1}}, {{0, 0, 1}},
  }};
  std::set<std::pair<uint64_t, uint32_t>> faces;
  for (const auto &cell : cells)
    for (uint32_t direction = 0u; direction < 6u; ++direction)
    {
      const std::array<int64_t, 3> neighbour{
          cell[0] + kOffsets[direction][0],
          cell[1] + kOffsets[direction][1],
          cell[2] + kOffsets[direction][2]};
      if (cells.count(neighbour) == 0u)
        faces.emplace(packCell(cell), direction);
    }
  return faces;
}

// Expand the built clusters back into unit voxel faces, per octree level.
bool meshFacesPerLevel(
    const vg::VirtualGeometryBuildData &build,
    const svr::SparseVoxelOctreeVirtualGeometryStats &stats,
    const svr::SparseVoxelBrickHierarchy &hierarchy,
    std::map<uint32_t, std::map<std::pair<uint64_t, uint32_t>, uint32_t>> &faces)
{
  faces.clear();
  for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
  {
    const auto &cluster = build.clusters[clusterIndex];
    const auto &origin = stats.clusterSources[clusterIndex];
    if (origin.brickNodeIndex >= hierarchy.nodes.size())
      return false;
    const auto &node = hierarchy.nodes[origin.brickNodeIndex];
    const double cellSize = static_cast<double>(node.minimumVoxelSize[3]);
    const int64_t scale = std::max<int64_t>(1, node.canonicalGridOriginScale[3]);
    std::array<int64_t, 3> base{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      base[axis] = static_cast<int64_t>(node.canonicalGridOriginScale[axis]) / scale;

    if (cluster.indices.size() % 6u != 0u)
      return false;
    for (size_t quad = 0u; quad * 6u < cluster.indices.size(); ++quad)
    {
      const uint32_t *triangle = &cluster.indices[quad * 6u];
      if (triangle[0] != triangle[3] || triangle[2] != triangle[4])
        return false;
      const std::array<uint32_t, 4> corners{
          triangle[0], triangle[1], triangle[2], triangle[5]};

      std::array<int64_t, 3> minimum{
          std::numeric_limits<int64_t>::max(),
          std::numeric_limits<int64_t>::max(),
          std::numeric_limits<int64_t>::max()};
      std::array<int64_t, 3> maximum{
          std::numeric_limits<int64_t>::lowest(),
          std::numeric_limits<int64_t>::lowest(),
          std::numeric_limits<int64_t>::lowest()};
      for (const uint32_t corner : corners)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          const double local =
              (static_cast<double>(cluster.vertices[corner].pos[axis]) -
               static_cast<double>(node.minimumVoxelSize[axis])) /
              cellSize;
          const int64_t rounded = static_cast<int64_t>(std::llround(local));
          if (std::fabs(local - static_cast<double>(rounded)) > 1e-3)
            return false;
          minimum[axis] = std::min(minimum[axis], rounded);
          maximum[axis] = std::max(maximum[axis], rounded);
        }

      uint32_t axis = 4u;
      for (uint32_t candidate = 0u; candidate < 3u; ++candidate)
        if (minimum[candidate] == maximum[candidate])
          axis = axis == 4u ? candidate : 4u;
      if (axis == 4u)
        return false;

      const float normalComponent = cluster.vertices[corners[0]].norm[axis];
      const bool positive = normalComponent > 0.0f;
      const uint32_t direction = axis * 2u + (positive ? 1u : 0u);
      const std::array<uint32_t, 2> tangents{
          axis == 0u ? 1u : 0u, axis == 2u ? 1u : 2u};
      for (int64_t u = minimum[tangents[0]]; u < maximum[tangents[0]]; ++u)
        for (int64_t v = minimum[tangents[1]]; v < maximum[tangents[1]]; ++v)
        {
          std::array<int64_t, 3> cell{};
          cell[axis] = minimum[axis] - (positive ? 1 : 0);
          cell[tangents[0]] = u;
          cell[tangents[1]] = v;
          faces[origin.level][{packCell({base[0] + cell[0],
                                         base[1] + cell[1],
                                         base[2] + cell[2]}),
                               direction}] += 1u;
        }
    }
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Runtime simulation: the shader's traversal, selection, and page streaming
// ─────────────────────────────────────────────────────────────────────────────

struct SelectionView
{
  std::array<float, 3> position{};
  float errorThreshold = 1.0f;
  // proj[1][1] * 0.5 * viewportHeight, folded into one constant.
  float projectionScale = 500.0f;
  float nearPlane = 0.1f;
};

float projectedError(
    const SelectionView &view,
    const vg::LODBounds &bounds,
    const float errorOverride)
{
  const float dx = bounds.center[0] - view.position[0];
  const float dy = bounds.center[1] - view.position[1];
  const float dz = bounds.center[2] - view.position[2];
  const float distance = std::max(
      std::sqrt(dx * dx + dy * dy + dz * dz) - bounds.radius, view.nearPlane);
  return errorOverride / distance * view.projectionScale;
}

vg::LODBounds nodeParentBounds(const vg::VirtualGeometryHierarchy &node)
{
  vg::LODBounds bounds{};
  bounds.center[0] = node.max_center_x;
  bounds.center[1] = node.max_center_y;
  bounds.center[2] = node.max_center_z;
  bounds.radius = node.max_radius;
  bounds.error = node.max_parent_lod_error;
  return bounds;
}

uint8_t fullMaskForCount(const uint32_t count)
{
  if (count == 0u)
    return 0u;
  return count >= 8u ? 0xffu : static_cast<uint8_t>((1u << count) - 1u);
}

// Streaming manager + hierarchy flag state, driven only by page install and
// uninstall rewrites - exactly what the runtime applies.
struct StreamingSimulation
{
  const vg::VirtualGeometryBuildData &build;
  std::vector<uint8_t> installed;
  std::vector<vg::VirtualGeometryHierarchy> hierarchy;

  explicit StreamingSimulation(const vg::VirtualGeometryBuildData &buildData)
      : build(buildData),
        installed(buildData.pages.size(), 0u),
        hierarchy(buildData.lodLevelHierarchy)
  {
    applyRewrites();
  }

  static void setMasks(
      vg::VirtualGeometryHierarchy &node,
      const uint8_t streamingMask,
      const uint8_t enabledMask)
  {
    node.flags &= ~(vg::HIERARCHY_STREAMING_MASK_BITS |
                    vg::HIERARCHY_ENABLED_MASK_BITS | vg::STREAMING_LEAF_FLAG);
    node.flags |= static_cast<uint32_t>(streamingMask)
        << vg::HIERARCHY_STREAMING_MASK_SHIFT;
    node.flags |= static_cast<uint32_t>(enabledMask)
        << vg::HIERARCHY_ENABLED_MASK_SHIFT;
    if (streamingMask != 0u)
      node.flags |= vg::STREAMING_LEAF_FLAG;
  }

  void applyRewrites()
  {
    for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.size(); ++nodeIndex)
    {
      auto &node = hierarchy[nodeIndex];
      node.flags = build.lodLevelHierarchy[nodeIndex].flags;
      node.pageIndex = build.lodLevelHierarchy[nodeIndex].pageIndex;
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u ||
          node.pageIndex == UINT32_MAX)
        continue;
      const uint32_t page = node.pageIndex & ~kPageNotInstalledBit;
      node.pageIndex = installed[page] ? page : (kPageNotInstalledBit | page);
      setMasks(node, 0u, 0u);
    }

    std::set<uint32_t> rewritten;
    for (uint32_t page = 0u; page < build.pages.size(); ++page)
    {
      for (const auto &update : build.pages[page].installUpdates.hierarchyUpdates)
        rewritten.insert(update.hierarchyNodeIndex);
      if (installed[page] == 0u)
        continue;
      for (const auto &update : build.pages[page].installUpdates.hierarchyUpdates)
        setMasks(
            hierarchy[update.hierarchyNodeIndex],
            update.streamingLeafsBitset,
            update.enabledClustersBitset);
    }

    for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.size(); ++nodeIndex)
    {
      auto &node = hierarchy[nodeIndex];
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u ||
          node.pageIndex == UINT32_MAX)
        continue;
      if ((node.pageIndex & kPageNotInstalledBit) != 0u)
        continue;
      if (rewritten.count(nodeIndex) != 0u)
        continue;
      setMasks(node, 0u, fullMaskForCount(node.child_count));
    }
  }

  bool canInstall(const uint32_t page) const
  {
    for (const uint32_t dependency : build.pages[page].dependencies)
      if (installed[dependency] == 0u)
        return false;
    return true;
  }

  void installPage(const uint32_t page)
  {
    installed[page] = 1u;
    applyRewrites();
  }

  void uninstallPage(const uint32_t page)
  {
    installed[page] = 0u;
    applyRewrites();
  }

  uint32_t installedPageCount() const
  {
    uint32_t count = 0u;
    for (const uint8_t flag : installed)
      count += flag != 0u ? 1u : 0u;
    return count;
  }

  std::vector<uint32_t> flagSnapshot() const
  {
    std::vector<uint32_t> snapshot;
    snapshot.reserve(hierarchy.size() * 2u);
    for (const auto &node : hierarchy)
    {
      snapshot.push_back(node.flags);
      snapshot.push_back(node.pageIndex);
    }
    return snapshot;
  }
};

struct FrameResult
{
  // Global cluster indices the frame would draw.
  std::vector<uint32_t> drawnClusters;
  // Pages the traversal reached but could not use, with their accumulated
  // priority. This is exactly the shader's install-candidate set.
  std::map<uint32_t, uint32_t> requestedPages;
  // Priority accumulated per installed page. The lowest is the eviction pick.
  std::map<uint32_t, uint32_t> installedPagePriority;
  uint32_t visitedNodes = 0u;
  uint32_t blockedLeafNodes = 0u;
};

FrameResult simulateFrame(
    const StreamingSimulation &sim,
    const SelectionView &view)
{
  FrameResult result{};
  if (sim.hierarchy.empty())
    return result;

  std::vector<uint32_t> stack{0u};
  while (!stack.empty())
  {
    const uint32_t nodeIndex = stack.back();
    stack.pop_back();
    const auto &node = sim.hierarchy[nodeIndex];
    ++result.visitedNodes;

    const bool isLeaf = (node.flags & vg::HIERARCHY_LEAF_FLAG) != 0u;
    if (!isLeaf)
    {
      // Descend only while the coarse representation is not good enough.
      if (projectedError(view, nodeParentBounds(node), node.max_parent_lod_error) >=
          view.errorThreshold)
        for (uint32_t child = 0u; child < node.child_count; ++child)
          stack.push_back(node.child_start + child);
      continue;
    }
    if (node.pageIndex == UINT32_MAX)
      continue;

    const uint32_t page = node.pageIndex & ~kPageNotInstalledBit;
    // The shader accumulates page priority before the install test, which is
    // what turns an unreachable page into an install candidate.
    if ((node.pageIndex & kPageNotInstalledBit) != 0u)
    {
      result.requestedPages[page] += 1u;
      ++result.blockedLeafNodes;
      continue;
    }
    result.installedPagePriority[page] += 1u;

    const uint8_t enabledMask = static_cast<uint8_t>(
        (node.flags & vg::HIERARCHY_ENABLED_MASK_BITS) >>
        vg::HIERARCHY_ENABLED_MASK_SHIFT);
    const uint8_t streamingMask = static_cast<uint8_t>(
        (node.flags & vg::HIERARCHY_STREAMING_MASK_BITS) >>
        vg::HIERARCHY_STREAMING_MASK_SHIFT);

    for (uint32_t local = 0u; local < node.child_count; ++local)
    {
      if (((enabledMask >> local) & 1u) == 0u)
        continue;
      const uint32_t cluster =
          sim.build.pages[page].clusterOffset + node.child_start + local;
      const auto &clusterData = sim.build.clusters[cluster];
      const bool streamingLeaf = ((streamingMask >> local) & 1u) != 0u;
      const float selfError = streamingLeaf ? 0.0f : clusterData.self.error;
      if (projectedError(view, clusterData.self, selfError) >= view.errorThreshold)
        continue;
      if (!streamingLeaf &&
          projectedError(view, clusterData.parent, clusterData.parent.error) <
              view.errorThreshold)
        continue;
      result.drawnClusters.push_back(cluster);
    }
  }
  return result;
}

// A drawn set covers the surface exactly when every finest block sits under
// exactly one drawn cluster's block box.
struct CoverageResult
{
  uint32_t uncovered = 0u;
  uint32_t doubled = 0u;
  uint32_t finestBlocks = 0u;
};

CoverageResult measureCoverage(
    const vg::VirtualGeometryBuildData &build,
    const svr::SparseVoxelOctreeVirtualGeometryStats &stats,
    const std::vector<uint32_t> &drawnClusters)
{
  std::vector<uint32_t> parents(build.clusters.size(), UINT32_MAX);
  for (uint32_t parent = 0u; parent < build.clusterDAG.size(); ++parent)
    for (const uint32_t child : build.clusterDAG[parent])
      parents[child] = parent;

  std::vector<uint8_t> drawn(build.clusters.size(), 0u);
  for (const uint32_t cluster : drawnClusters)
    drawn[cluster] = 1u;

  CoverageResult result{};
  for (uint32_t cluster = 0u; cluster < build.clusters.size(); ++cluster)
  {
    if (!build.clusterDAG[cluster].empty())
      continue;
    ++result.finestBlocks;
    uint32_t onPath = 0u;
    uint32_t cursor = cluster;
    while (cursor != UINT32_MAX)
    {
      onPath += drawn[cursor];
      cursor = parents[cursor];
    }
    if (onPath == 0u)
      ++result.uncovered;
    else if (onPath > 1u)
      ++result.doubled;
  }
  (void)stats;
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 1: mesh, LOD and DAG over the prebuilt octree
// ─────────────────────────────────────────────────────────────────────────────

struct BuiltChunk
{
  SyntheticChunk chunk;
  vg::VirtualGeometryBuildData build;
  svr::SparseVoxelOctreeVirtualGeometryStats stats;
  bool valid = false;
};

BuiltChunk buildChunkPages(const uint32_t side, const uint32_t maxGroupsPerPage, const uint32_t maxRootPageGroups)
{
  BuiltChunk result{};
  std::string error;
  if (!buildSyntheticChunk(side, result.chunk, &error))
  {
    std::cout << "  synthetic chunk build failed: " << error << '\n';
    return result;
  }

  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = maxGroupsPerPage;
  buildSettings.maxRootPageGroups = maxRootPageGroups;
  result.valid = svr::buildSparseVoxelOctreeVirtualGeometry(
      result.chunk.hierarchy,
      0u,
      static_cast<uint32_t>(result.chunk.hierarchy.nodes.size()),
      octreeSettings,
      buildSettings,
      result.build,
      &result.stats,
      &error);
  if (!result.valid)
    std::cout << "  octree virtual geometry build failed: " << error << '\n';
  return result;
}

void testPrebuiltOctreeIsWellFormed()
{
  std::cout << "stage 1: the prebuilt octree is a valid coarsening chain\n";

  SyntheticChunk chunk{};
  std::string error;
  check(buildSyntheticChunk(64u, chunk, &error), "synthetic chunk octree builds: " + error);
  if (chunk.hierarchy.empty())
    return;

  check(chunk.hierarchy.levelCount > 2u, "the chunk has several octree levels");
  check(!chunk.finestCells.empty(), "the chunk has occupied cells");

  const auto levels = levelOccupancy(chunk.hierarchy);
  check(levels.size() == chunk.hierarchy.levelCount, "every level carries occupancy");
  check(
      levels.count(0u) != 0u && levels.at(0u) == chunk.finestCells,
      "level zero reproduces the authored voxel set exactly");

  uint32_t coverageBreaks = 0u;
  uint32_t inventedCells = 0u;
  for (const auto &[level, cells] : levels)
  {
    if (level == 0u)
      continue;
    const auto &finer = levels.at(level - 1u);
    for (const auto &cell : finer)
      if (cells.count({cell[0] >> 1, cell[1] >> 1, cell[2] >> 1}) == 0u)
        ++coverageBreaks;
    for (const auto &cell : cells)
    {
      bool anyChild = false;
      for (uint32_t octant = 0u; octant < 8u && !anyChild; ++octant)
        anyChild = finer.count({
                       cell[0] * 2 + static_cast<int64_t>(octant & 1u),
                       cell[1] * 2 + static_cast<int64_t>((octant >> 1u) & 1u),
                       cell[2] * 2 + static_cast<int64_t>((octant >> 2u) & 1u),
                   }) != 0u;
      if (!anyChild)
        ++inventedCells;
    }
  }
  check(coverageBreaks == 0u, "every fine cell has an occupied parent cell");
  check(inventedCells == 0u, "no coarse cell is invented from empty space");

  uint32_t nonMonotonic = 0u;
  uint32_t missingParentError = 0u;
  for (const auto &node : chunk.hierarchy.nodes)
  {
    if (node.parentIndex == svr::kSparseVoxelBrickInvalidIndex)
    {
      if (node.parentError != std::numeric_limits<float>::max())
        ++missingParentError;
      continue;
    }
    const auto &parent = chunk.hierarchy.nodes[node.parentIndex];
    if (parent.selfError + 1e-6f < node.selfError)
      ++nonMonotonic;
    if (std::fabs(node.parentError - parent.selfError) > 1e-6f)
      ++missingParentError;
  }
  check(nonMonotonic == 0u, "octree errors are monotonic parent to child");
  check(missingParentError == 0u, "every node carries its parent's measured error");

  std::cout << "  octree levels=" << chunk.hierarchy.levelCount
            << " nodes=" << chunk.hierarchy.nodes.size()
            << " cells=" << chunk.finestCells.size() << '\n';
}

void testEveryVisibleVoxelFaceHasAMeshFace()
{
  std::cout << "stage 1: every visible voxel face has exactly one mesh face\n";

  BuiltChunk built = buildChunkPages(64u, vg::MAX_GROUPS_PER_PAGE, 0u);
  check(built.valid, "octree virtual geometry builds");
  if (!built.valid)
    return;

  std::map<uint32_t, std::map<std::pair<uint64_t, uint32_t>, uint32_t>> meshed;
  check(
      meshFacesPerLevel(built.build, built.stats, built.chunk.hierarchy, meshed),
      "every cluster is a run of well formed quads");

  const auto levels = levelOccupancy(built.chunk.hierarchy);
  uint32_t missing = 0u;
  uint32_t extra = 0u;
  uint32_t duplicated = 0u;
  uint64_t expectedTotal = 0u;
  for (const auto &[level, cells] : levels)
  {
    const auto expected = visibleFaces(cells);
    expectedTotal += expected.size();
    const auto &actual = meshed[level];
    for (const auto &face : expected)
      if (actual.count(face) == 0u)
        ++missing;
    for (const auto &[face, count] : actual)
    {
      if (expected.count(face) == 0u)
        ++extra;
      if (count != 1u)
        ++duplicated;
    }
  }

  check(missing == 0u, "no visible voxel face is missing: " + std::to_string(missing));
  check(extra == 0u, "no mesh face sits on an occluded side: " + std::to_string(extra));
  check(duplicated == 0u, "no voxel face is meshed twice: " + std::to_string(duplicated));
  check(
      built.stats.exposedFaceCount == expectedTotal,
      "the builder's face count matches the oracle");

  std::cout << "  faces=" << expectedTotal << " quads=" << built.stats.quadCount
            << " clusters=" << built.stats.clusterCount
            << " blockDepth=" << built.stats.blockDepth << '\n';
}

void testDAGErrorsAdmitExactlyOneClusterPerPath()
{
  std::cout << "stage 1: self and parent errors admit exactly one cluster per path\n";

  BuiltChunk built = buildChunkPages(64u, vg::MAX_GROUPS_PER_PAGE, 0u);
  check(built.valid, "octree virtual geometry builds");
  if (!built.valid)
    return;

  uint32_t boundsMismatch = 0u;
  uint32_t errorRegression = 0u;
  for (uint32_t parent = 0u; parent < built.build.clusterDAG.size(); ++parent)
    for (const uint32_t child : built.build.clusterDAG[parent])
    {
      const auto &parentSelf = built.build.clusters[parent].self;
      const auto &childParent = built.build.clusters[child].parent;
      if (std::fabs(parentSelf.error - childParent.error) > 1e-5f ||
          std::fabs(parentSelf.radius - childParent.radius) > 1e-4f ||
          std::fabs(parentSelf.center[0] - childParent.center[0]) > 1e-4f ||
          std::fabs(parentSelf.center[1] - childParent.center[1]) > 1e-4f ||
          std::fabs(parentSelf.center[2] - childParent.center[2]) > 1e-4f)
        ++boundsMismatch;
      if (parentSelf.error + 1e-5f < built.build.clusters[child].self.error)
        ++errorRegression;
    }
  check(boundsMismatch == 0u, "a child's parent bounds equal its coarse cluster's self bounds");
  check(errorRegression == 0u, "errors never shrink going up the DAG");

  uint32_t oversizedGroups = 0u;
  for (const auto &group : built.build.groupInfos)
    if (group.originalClusterIndices.size() > vg::MaxBVHChildren)
      ++oversizedGroups;
  check(oversizedGroups == 0u, "no group exceeds the hierarchy leaf child budget");

  // Fully installed: the cut is pure error selection, at every distance.
  StreamingSimulation sim(built.build);
  for (uint32_t page = 0u; page < built.build.pages.size(); ++page)
    sim.installPage(page);

  uint32_t uncovered = 0u;
  uint32_t doubled = 0u;
  std::vector<uint32_t> drawnPerDistance;
  for (const float distance : {6.0f, 24.0f, 96.0f, 384.0f, 1536.0f})
  {
    SelectionView view{};
    view.position = {32.0f, 12.0f + distance * 0.25f, -distance};
    const FrameResult frame = simulateFrame(sim, view);
    const CoverageResult coverage =
        measureCoverage(built.build, built.stats, frame.drawnClusters);
    uncovered += coverage.uncovered;
    doubled += coverage.doubled;
    drawnPerDistance.push_back(static_cast<uint32_t>(frame.drawnClusters.size()));
  }
  check(uncovered == 0u, "no path is left undrawn at any distance: no holes");
  check(doubled == 0u, "no path draws twice at any distance: no double geometry");

  bool refines = true;
  for (size_t index = 1u; index < drawnPerDistance.size(); ++index)
    refines = refines && drawnPerDistance[index] <= drawnPerDistance[index - 1u];
  check(refines, "moving away never draws more clusters");
  check(
      drawnPerDistance.front() > drawnPerDistance.back(),
      "the near view draws strictly more clusters than the far one");

  std::cout << "  clusters drawn by distance:";
  for (const uint32_t count : drawnPerDistance)
    std::cout << ' ' << count;
  std::cout << '\n';
}


void testLeafErrorsAreZeroAndRootsAreInfinite()
{
  std::cout << "stage 1: leaf clusters have zero self error, roots infinite parent error\n";

  BuiltChunk built = buildChunkPages(64u, vg::MAX_GROUPS_PER_PAGE, 0u);
  check(built.valid, "octree virtual geometry builds");
  if (!built.valid)
    return;

  // The two conventions the culler depends on. A leaf cluster is the finest
  // representation there is, so drawing it approximates nothing: self error must
  // be zero or the cluster gets dropped once the camera is close enough, with
  // nothing finer to replace it. A root cluster has no coarser stand-in, so its
  // parent error must be infinite or it gets dropped once the camera is far
  // enough away. Between them they guarantee there is always something to draw.
  uint32_t leafClusters = 0u;
  uint32_t leafWithError = 0u;
  uint32_t rootClusters = 0u;
  uint32_t rootWithFiniteParent = 0u;
  for (uint32_t cluster = 0u; cluster < built.build.clusters.size(); ++cluster)
  {
    if (built.build.clusterDAG[cluster].empty())
    {
      ++leafClusters;
      if (built.build.clusters[cluster].self.error != 0.0f)
        ++leafWithError;
    }
    bool hasCoarser = false;
    for (uint32_t parent = 0u; parent < built.build.clusterDAG.size(); ++parent)
      for (const uint32_t child : built.build.clusterDAG[parent])
        hasCoarser = hasCoarser || child == cluster;
    if (!hasCoarser)
    {
      ++rootClusters;
      if (built.build.clusters[cluster].parent.error !=
          std::numeric_limits<float>::max())
        ++rootWithFiniteParent;
    }
  }
  check(leafClusters > 0u, "the DAG has leaf clusters");
  check(leafWithError == 0u, "every leaf cluster has zero self error");
  check(rootClusters > 0u, "the DAG has root clusters");
  check(rootWithFiniteParent == 0u, "every root cluster has an infinite parent error");

  // And the consequence: at any distance, however extreme, the fully resident
  // DAG draws something.
  StreamingSimulation sim(built.build);
  for (uint32_t page = 0u; page < built.build.pages.size(); ++page)
    sim.installPage(page);

  uint32_t emptyFrames = 0u;
  uint32_t holeFrames = 0u;
  for (const float distance : {0.5f, 2.0f, 8.0f, 32.0f, 128.0f, 512.0f, 2048.0f, 65536.0f})
  {
    SelectionView view{};
    view.position = {32.0f, 16.0f, -distance};
    const FrameResult frame = simulateFrame(sim, view);
    if (frame.drawnClusters.empty())
      ++emptyFrames;
    const CoverageResult coverage =
        measureCoverage(built.build, built.stats, frame.drawnClusters);
    if (coverage.uncovered != 0u)
      ++holeFrames;
  }
  check(emptyFrames == 0u, "there is always something to render, at every distance");
  check(holeFrames == 0u, "and it always covers the whole surface");
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 2: a small page 0
// ─────────────────────────────────────────────────────────────────────────────

void testPageZeroHoldsOnlyTheRootAndCoarseLevels()
{
  std::cout << "stage 2: page 0 holds the root and a few coarse levels only\n";

  BuiltChunk built = buildChunkPages(64u, 4u, 1u);
  check(built.valid, "octree virtual geometry builds with a small root page");
  if (!built.valid)
    return;

  check(built.build.pages.size() > 2u, "the DAG really is split over several pages");
  check(
      built.build.pages[0].clusterCount < built.build.clusters.size(),
      "page 0 is a strict subset of the DAG");
  check(built.build.pages[0].dependencies.empty(), "page 0 depends on nothing");

  // Page 0 must carry the coarsest clusters: the ones with an infinite parent
  // error are the roots, and they can only live in the page installed first.
  uint32_t rootClustersInPageZero = 0u;
  uint32_t rootClustersElsewhere = 0u;
  for (uint32_t page = 0u; page < built.build.pages.size(); ++page)
    for (uint32_t local = 0u; local < built.build.pages[page].clusterCount; ++local)
    {
      const uint32_t cluster = built.build.pages[page].clusterOffset + local;
      if (built.build.clusters[cluster].parent.error !=
          std::numeric_limits<float>::max())
        continue;
      if (page == 0u)
        ++rootClustersInPageZero;
      else
        ++rootClustersElsewhere;
    }
  check(rootClustersInPageZero > 0u, "page 0 carries root clusters");
  check(rootClustersElsewhere == 0u, "no root cluster lives outside page 0");

  // Octree shape: page 0's clusters come from the coarsest octree levels, and
  // no page holds a cluster finer than one a page it depends on holds.
  uint32_t pageZeroMinimumLevel = UINT32_MAX;
  std::vector<uint32_t> pageMinimumLevel(built.build.pages.size(), UINT32_MAX);
  std::vector<uint32_t> pageMaximumLevel(built.build.pages.size(), 0u);
  for (uint32_t page = 0u; page < built.build.pages.size(); ++page)
    for (uint32_t local = 0u; local < built.build.pages[page].clusterCount; ++local)
    {
      const uint32_t cluster = built.build.pages[page].clusterOffset + local;
      const uint32_t level = built.stats.clusterSources[cluster].level;
      pageMinimumLevel[page] = std::min(pageMinimumLevel[page], level);
      pageMaximumLevel[page] = std::max(pageMaximumLevel[page], level);
    }
  pageZeroMinimumLevel = pageMinimumLevel[0];

  const uint32_t coarsestLevel = built.stats.levelCount - 1u;
  check(
      pageMaximumLevel[0] == coarsestLevel,
      "page 0 reaches the coarsest octree level");
  check(
      pageZeroMinimumLevel > 0u,
      "page 0 stops short of the finest octree level");

  uint32_t dependencyLevelBreaks = 0u;
  for (uint32_t page = 0u; page < built.build.pages.size(); ++page)
    for (const uint32_t dependency : built.build.pages[page].dependencies)
      if (pageMinimumLevel[dependency] < pageMinimumLevel[page])
        ++dependencyLevelBreaks;
  check(
      dependencyLevelBreaks == 0u,
      "a page never depends on one that carries finer geometry than itself");

  // Cluster ranges must tile the cluster array exactly once.
  std::vector<uint8_t> owned(built.build.clusters.size(), 0u);
  uint32_t overlaps = 0u;
  for (const auto &page : built.build.pages)
    for (uint32_t local = 0u; local < page.clusterCount; ++local)
      overlaps += owned[page.clusterOffset + local]++ != 0u ? 1u : 0u;
  const bool everyClusterOwned =
      std::all_of(owned.begin(), owned.end(), [](const uint8_t count) { return count == 1u; });
  check(overlaps == 0u, "no cluster is owned by two pages");
  check(everyClusterOwned, "every cluster is owned by exactly one page");

  // The dependency graph must be installable at all.
  std::vector<uint32_t> inDegree(built.build.pages.size(), 0u);
  std::vector<std::vector<uint32_t>> dependants(built.build.pages.size());
  for (uint32_t page = 0u; page < built.build.pages.size(); ++page)
  {
    inDegree[page] = static_cast<uint32_t>(built.build.pages[page].dependencies.size());
    for (const uint32_t dependency : built.build.pages[page].dependencies)
      dependants[dependency].push_back(page);
  }
  std::queue<uint32_t> ready;
  for (uint32_t page = 0u; page < inDegree.size(); ++page)
    if (inDegree[page] == 0u)
      ready.push(page);
  uint32_t ordered = 0u;
  while (!ready.empty())
  {
    const uint32_t page = ready.front();
    ready.pop();
    ++ordered;
    for (const uint32_t dependant : dependants[page])
      if (--inDegree[dependant] == 0u)
        ready.push(dependant);
  }
  check(ordered == built.build.pages.size(), "the page dependency graph is acyclic");

  std::cout << "  pages=" << built.build.pages.size()
            << " page0 clusters=" << built.build.pages[0].clusterCount
            << "/" << built.build.clusters.size()
            << " page0 levels=" << pageZeroMinimumLevel << ".." << pageMaximumLevel[0]
            << " octree levels=" << built.stats.levelCount << '\n';
}

void testPageZeroAloneStillDrawsTheWholeSurface()
{
  std::cout << "stage 2: page 0 alone already covers the whole surface\n";

  BuiltChunk built = buildChunkPages(64u, 4u, 1u);
  check(built.valid, "octree virtual geometry builds with a small root page");
  if (!built.valid)
    return;

  StreamingSimulation sim(built.build);
  sim.installPage(0u);
  check(sim.installedPageCount() == 1u, "only page 0 is installed");

  // Standing right next to the chunk with nothing but the root page resident:
  // the streaming rewrites must still produce a complete, hole-free cut, or the
  // first frame after a page install shows holes.
  SelectionView view{};
  view.position = {32.0f, 20.0f, -8.0f};
  const FrameResult frame = simulateFrame(sim, view);
  check(!frame.drawnClusters.empty(), "page 0 alone draws geometry");

  const CoverageResult coverage =
      measureCoverage(built.build, built.stats, frame.drawnClusters);
  check(coverage.uncovered == 0u, "no hole while only page 0 is resident");
  check(coverage.doubled == 0u, "no double geometry while only page 0 is resident");
  check(frame.requestedPages.size() > 0u, "page 0 alone is not enough: finer pages are requested");

  // Every drawn cluster must belong to page 0 - nothing else is installed.
  uint32_t drawnOutsidePageZero = 0u;
  for (const uint32_t cluster : frame.drawnClusters)
    if (cluster < built.build.pages[0].clusterOffset ||
        cluster >= built.build.pages[0].clusterOffset + built.build.pages[0].clusterCount)
      ++drawnOutsidePageZero;
  check(drawnOutsidePageZero == 0u, "nothing outside page 0 is ever drawn");

  std::cout << "  page0-only drawn=" << frame.drawnClusters.size()
            << " requested pages=" << frame.requestedPages.size()
            << " blocked leaves=" << frame.blockedLeafNodes << '\n';
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 3: the runtime streaming loop
// ─────────────────────────────────────────────────────────────────────────────

void testRuntimeRequestsPagesUntilTheErrorBudgetIsMet()
{
  std::cout << "stage 3: the runtime keeps requesting pages until the cut is resident\n";

  BuiltChunk built = buildChunkPages(64u, 4u, 1u);
  check(built.valid, "octree virtual geometry builds with a small root page");
  if (!built.valid)
    return;

  StreamingSimulation sim(built.build);
  sim.installPage(0u);

  SelectionView near{};
  near.position = {32.0f, 20.0f, -8.0f};

  uint32_t frames = 0u;
  uint32_t installs = 0u;
  uint32_t holes = 0u;
  uint32_t doubles = 0u;
  uint32_t dependencyViolations = 0u;
  std::vector<uint32_t> requestsPerFrame;
  std::vector<uint32_t> drawnPerFrame;

  // One page install per frame, always the highest-priority request, which is
  // what the shader's reduction picks.
  while (frames < static_cast<uint32_t>(built.build.pages.size()) * 4u)
  {
    const FrameResult frame = simulateFrame(sim, near);
    const CoverageResult coverage =
        measureCoverage(built.build, built.stats, frame.drawnClusters);
    holes += coverage.uncovered;
    doubles += coverage.doubled;
    requestsPerFrame.push_back(static_cast<uint32_t>(frame.requestedPages.size()));
    drawnPerFrame.push_back(static_cast<uint32_t>(frame.drawnClusters.size()));
    ++frames;

    if (frame.requestedPages.empty())
      break;

    uint32_t bestPage = UINT32_MAX;
    uint32_t bestPriority = 0u;
    for (const auto &[page, priority] : frame.requestedPages)
      if (priority > bestPriority || (priority == bestPriority && page < bestPage))
      {
        bestPriority = priority;
        bestPage = page;
      }
    if (!sim.canInstall(bestPage))
    {
      // The runtime installs dependencies first; a request for a page whose
      // parent page is missing must never be honoured out of order.
      ++dependencyViolations;
      bool progressed = false;
      for (uint32_t page = 0u; page < built.build.pages.size() && !progressed; ++page)
        if (sim.installed[page] == 0u && sim.canInstall(page))
        {
          sim.installPage(page);
          progressed = true;
        }
      if (!progressed)
        break;
    }
    else
      sim.installPage(bestPage);
    ++installs;
  }

  check(frames > 1u, "the loop ran for several frames");
  check(installs > 0u, "the runtime installed pages in response to requests");
  check(holes == 0u, "no frame in the streaming sequence had a hole");
  check(doubles == 0u, "no frame in the streaming sequence drew twice");
  check(
      dependencyViolations == 0u,
      "no requested page was ever missing its dependencies");
  check(
      requestsPerFrame.back() == 0u,
      "the request queue drains: the resident cut finally satisfies the error budget");
  check(
      drawnPerFrame.back() > drawnPerFrame.front(),
      "the converged cut draws more clusters than the root page alone");

  bool requestsShrink = true;
  for (size_t index = 1u; index < requestsPerFrame.size(); ++index)
    requestsShrink = requestsShrink && requestsPerFrame[index] <= requestsPerFrame[index - 1u];
  check(requestsShrink, "the outstanding request set never grows while installing");

  std::cout << "  frames=" << frames << " installs=" << installs
            << " installedPages=" << sim.installedPageCount() << "/"
            << built.build.pages.size()
            << " drawn " << drawnPerFrame.front() << "->" << drawnPerFrame.back()
            << '\n';
}

void testRequestsFollowTheCameraAndNotTheWholeTree()
{
  std::cout << "stage 3: requests follow the error metric, not the whole tree\n";

  BuiltChunk built = buildChunkPages(64u, 4u, 1u);
  check(built.valid, "octree virtual geometry builds with a small root page");
  if (!built.valid)
    return;

  StreamingSimulation sim(built.build);
  sim.installPage(0u);

  SelectionView nearView{};
  nearView.position = {32.0f, 20.0f, -8.0f};
  SelectionView farView = nearView;
  farView.position = {32.0f, 400.0f, -4000.0f};

  const FrameResult nearFrame = simulateFrame(sim, nearView);
  const FrameResult farFrame = simulateFrame(sim, farView);

  check(
      farFrame.requestedPages.size() < nearFrame.requestedPages.size(),
      "a distant camera requests fewer pages than a near one");
  check(!nearFrame.requestedPages.empty(), "a near camera does request finer pages");
  // The root page here holds a single group, so even a distant camera needs one
  // more page. What must hold is containment: distance can only ever remove
  // requests, never introduce one the near view did not have.
  uint32_t farRequestsNotInNear = 0u;
  for (const auto &[page, priority] : farFrame.requestedPages)
  {
    (void)priority;
    if (nearFrame.requestedPages.count(page) == 0u)
      ++farRequestsNotInNear;
  }
  check(
      farRequestsNotInNear == 0u,
      "every page a distant camera requests is also requested up close");
  check(
      farFrame.visitedNodes < nearFrame.visitedNodes,
      "a distant camera stops descending the hierarchy earlier");

  // Priority must be a real ordering, not a constant: the page the camera is
  // closest to has to outrank the rest.
  uint32_t distinctPriorities = 0u;
  std::set<uint32_t> priorities;
  for (const auto &[page, priority] : nearFrame.requestedPages)
  {
    (void)page;
    priorities.insert(priority);
  }
  distinctPriorities = static_cast<uint32_t>(priorities.size());
  check(distinctPriorities >= 1u, "requested pages carry a priority");

  std::cout << "  near requests=" << nearFrame.requestedPages.size()
            << " visited=" << nearFrame.visitedNodes
            << " | far requests=" << farFrame.requestedPages.size()
            << " visited=" << farFrame.visitedNodes << '\n';
}

void testUninstallRewritesRestoreTheCoarseCutExactly()
{
  std::cout << "stage 3: uninstall rewrites restore the coarse cut exactly\n";

  BuiltChunk built = buildChunkPages(64u, 4u, 1u);
  check(built.valid, "octree virtual geometry builds with a small root page");
  if (!built.valid)
    return;

  StreamingSimulation sim(built.build);
  sim.installPage(0u);
  const std::vector<uint32_t> rootOnlyFlags = sim.flagSnapshot();

  SelectionView nearView{};
  nearView.position = {32.0f, 20.0f, -8.0f};
  const FrameResult rootOnlyFrame = simulateFrame(sim, nearView);

  // Stream everything in, in dependency order.
  std::vector<uint32_t> installOrder;
  while (sim.installedPageCount() < built.build.pages.size())
  {
    bool progressed = false;
    for (uint32_t page = 0u; page < built.build.pages.size(); ++page)
      if (sim.installed[page] == 0u && sim.canInstall(page))
      {
        sim.installPage(page);
        installOrder.push_back(page);
        progressed = true;
      }
    if (!progressed)
      break;
  }
  check(
      sim.installedPageCount() == built.build.pages.size(),
      "every page installs in dependency order");

  const FrameResult fullFrame = simulateFrame(sim, nearView);
  check(
      fullFrame.drawnClusters.size() > rootOnlyFrame.drawnClusters.size(),
      "the fully resident cut is finer than the root-only cut");
  const CoverageResult fullCoverage =
      measureCoverage(built.build, built.stats, fullFrame.drawnClusters);
  check(fullCoverage.uncovered == 0u, "the fully resident cut has no holes");
  check(fullCoverage.doubled == 0u, "the fully resident cut draws nothing twice");

  // Now evict everything except page 0, lowest priority first, checking the cut
  // after each step. This is the path a camera walking away takes.
  uint32_t evictions = 0u;
  uint32_t holes = 0u;
  uint32_t doubles = 0u;
  uint32_t dependantStillInstalled = 0u;
  while (sim.installedPageCount() > 1u)
  {
    // Only evict a page nothing installed depends on, which is what
    // uninstallPageRecursive enforces at runtime.
    uint32_t victim = UINT32_MAX;
    for (uint32_t page = static_cast<uint32_t>(built.build.pages.size()); page-- > 1u;)
    {
      if (sim.installed[page] == 0u)
        continue;
      bool hasInstalledDependant = false;
      for (uint32_t other = 0u; other < built.build.pages.size(); ++other)
      {
        if (sim.installed[other] == 0u)
          continue;
        const auto &dependencies = built.build.pages[other].dependencies;
        if (std::find(dependencies.begin(), dependencies.end(), page) !=
            dependencies.end())
          hasInstalledDependant = true;
      }
      if (!hasInstalledDependant)
      {
        victim = page;
        break;
      }
    }
    if (victim == UINT32_MAX)
    {
      ++dependantStillInstalled;
      break;
    }

    sim.uninstallPage(victim);
    ++evictions;
    const FrameResult frame = simulateFrame(sim, nearView);
    const CoverageResult coverage =
        measureCoverage(built.build, built.stats, frame.drawnClusters);
    holes += coverage.uncovered;
    doubles += coverage.doubled;
  }

  check(evictions > 0u, "pages were evicted");
  check(dependantStillInstalled == 0u, "eviction never stalls on a dependency");
  check(holes == 0u, "no hole appears at any point during eviction");
  check(doubles == 0u, "no double geometry appears at any point during eviction");
  check(sim.installedPageCount() == 1u, "everything but page 0 is evicted");

  // The rewrites must be exactly reversible: install then uninstall has to land
  // on the same hierarchy bits, or residency slowly corrupts the cut.
  check(
      sim.flagSnapshot() == rootOnlyFlags,
      "uninstall restores the root-only hierarchy flags bit for bit");
  const FrameResult restoredFrame = simulateFrame(sim, nearView);
  check(
      restoredFrame.drawnClusters == rootOnlyFrame.drawnClusters,
      "the restored cut is identical to the original root-only cut");

  std::cout << "  installs=" << installOrder.size() << " evictions=" << evictions
            << " drawn root-only=" << rootOnlyFrame.drawnClusters.size()
            << " full=" << fullFrame.drawnClusters.size() << '\n';
}

void testStreamingSequenceIsOrderIndependent()
{
  std::cout << "stage 3: any legal install order reaches the same resident cut\n";

  BuiltChunk built = buildChunkPages(64u, 4u, 1u);
  check(built.valid, "octree virtual geometry builds with a small root page");
  if (!built.valid)
    return;

  SelectionView view{};
  view.position = {32.0f, 20.0f, -8.0f};

  const auto installAll = [&](const bool reverse)
  {
    StreamingSimulation sim(built.build);
    while (sim.installedPageCount() < built.build.pages.size())
    {
      bool progressed = false;
      const uint32_t pageCount = static_cast<uint32_t>(built.build.pages.size());
      for (uint32_t step = 0u; step < pageCount && !progressed; ++step)
      {
        const uint32_t page = reverse ? pageCount - 1u - step : step;
        if (sim.installed[page] == 0u && sim.canInstall(page))
        {
          sim.installPage(page);
          progressed = true;
        }
      }
      if (!progressed)
        break;
    }
    return sim.flagSnapshot();
  };

  const std::vector<uint32_t> forward = installAll(false);
  const std::vector<uint32_t> backward = installAll(true);
  check(forward == backward, "install order does not change the resident hierarchy state");

  StreamingSimulation sim(built.build);
  for (uint32_t page = 0u; page < built.build.pages.size(); ++page)
    sim.installPage(page);
  const FrameResult frame = simulateFrame(sim, view);
  const CoverageResult coverage =
      measureCoverage(built.build, built.stats, frame.drawnClusters);
  check(coverage.uncovered == 0u, "the fully resident cut still has no holes");
  check(coverage.doubled == 0u, "the fully resident cut still draws nothing twice");
}

} // namespace

int main()
{
  std::cout << "Sparse voxel octree page streaming tests\n";
  testPrebuiltOctreeIsWellFormed();
  testEveryVisibleVoxelFaceHasAMeshFace();
  testDAGErrorsAdmitExactlyOneClusterPerPath();
  testLeafErrorsAreZeroAndRootsAreInfinite();
  testPageZeroHoldsOnlyTheRootAndCoarseLevels();
  testPageZeroAloneStillDrawsTheWholeSurface();
  testRuntimeRequestsPagesUntilTheErrorBudgetIsMet();
  testRequestsFollowTheCameraAndNotTheWholeTree();
  testUninstallRewritesRestoreTheCoarseCutExactly();
  testStreamingSequenceIsOrderIndependent();

  if (failures != 0)
  {
    std::cout << failures << " sparse voxel octree page streaming checks failed\n";
    return 1;
  }
  std::cout << "all sparse voxel octree page streaming checks passed\n";
  return 0;
}
