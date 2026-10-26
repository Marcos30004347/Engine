// Page install/uninstall rewrites over a deliberately huge octree.
//
// The rewrites are the only thing that makes a partially resident DAG
// renderable. They are pure CPU data, so they can be exercised far harder than a
// GPU test allows: this fixture is a single floor of tiny voxels wide enough
// that the brick octree runs seven levels deep and the page builder has to split
// the DAG across dozens of pages with real cross-page references.
//
// What is checked, after every single install and every single uninstall:
//
//   - the enabled set stays closed under DAG parents (the classic valid-cut
//     property), streaming marks stay a subset of it, and nothing on an
//     uninstalled page is ever enabled;
//   - the *renderable* cut is exactly one cluster per root-to-leaf path at a
//     sweep of error thresholds - no holes, no parent drawn over its children;
//   - cross-page rewrites only ever name nodes whose owning page is already
//     resident, so an update can never be applied against a page that is not
//     there;
//   - every cluster becomes available eventually, and once everything is
//     resident nothing is left on the residency border;
//   - install order does not change the final state, and uninstalling back down
//     reproduces the earlier states bit for bit.

#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <string>
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
// A huge octree: one floor of the finest voxels, as wide as we ask for
// ─────────────────────────────────────────────────────────────────────────────
//
// Each doubling of `side` adds an octree level. The floor is one voxel thick
// with a two-cell skirt so coarsening has something to lose, and every cell
// carries one of four materials in a pattern that does not merge - greedy
// meshing would otherwise collapse a flat plane to one quad per brick and the
// DAG would be far too small to stress the page builder.
uint32_t cellMaterial(const uint32_t x, const uint32_t z)
{
  return ((x ^ z) & 1u) | (((x >> 3u) ^ (z >> 3u)) & 1u) << 1u;
}

bool buildFloorOctree(
    const uint32_t side,
    svr::SparseVoxelBrickHierarchy &hierarchy,
    uint64_t &cellCount,
    std::string *error)
{
  std::vector<svr::SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<svr::SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> leaves;
  const uint64_t reserve = static_cast<uint64_t>(side) * side * 3u;
  centersLengths.reserve(reserve);
  materials.reserve(reserve);
  leaves.reserve(reserve);

  static constexpr std::array<uint32_t, 4> kAlbedo{
      0xff5f9e4eu, 0xff7a6a52u, 0xff9a9a9au, 0xffd8d2c0u};

  for (uint32_t x = 0u; x < side; ++x)
    for (uint32_t z = 0u; z < side; ++z)
    {
      // A shallow, deterministic relief: enough for coarsening to measure a
      // real error instead of falling back on the floor value everywhere.
      const uint32_t top = 3u + ((x / 16u + z / 16u) % 3u);
      for (uint32_t y = top >= 2u ? top - 2u : 0u; y < top; ++y)
      {
        svr::SparseVoxelCenterLengthGPU cell{};
        cell.center = {
            static_cast<float>(x) + 0.5f,
            static_cast<float>(y) + 0.5f,
            static_cast<float>(z) + 0.5f};
        cell.length = 1.0f;
        svr::SparseVoxelMaterialGPU material{};
        const uint32_t band = cellMaterial(x, z);
        material.albedoRGBA8 = kAlbedo[band];
        material.materialId = band;
        leaves.push_back(static_cast<uint32_t>(centersLengths.size()));
        centersLengths.push_back(cell);
        materials.push_back(material);
      }
    }

  cellCount = leaves.size();
  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.maximumBricks = 1u << 20u;
  return svr::buildSparseVoxelBrickHierarchy(
      centersLengths, materials, leaves, settings, hierarchy, error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Residency state driven only by the rewrites
// ─────────────────────────────────────────────────────────────────────────────

uint8_t fullMaskForCount(const uint32_t count)
{
  if (count == 0u)
    return 0u;
  return count >= 8u ? 0xffu : static_cast<uint8_t>((1u << count) - 1u);
}

// Mirrors culling_multipass.wgsl, which now applies the parent test to streaming
// leaves as well. Set true to model the older rule that skipped it.
bool streamingLeavesSkipParentTest = false;

struct Residency
{
  const vg::VirtualGeometryBuildData &build;
  std::vector<uint8_t> installed;
  std::vector<vg::VirtualGeometryHierarchy> hierarchy;
  // Cluster -> hierarchy node and bit index inside that node.
  std::vector<uint32_t> clusterNode;
  std::vector<uint8_t> clusterBit;
  std::vector<uint32_t> clusterPage;
  // Cluster -> its single coarser cluster, UINT32_MAX at a root.
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

  static void setMasks(
      vg::VirtualGeometryHierarchy &node,
      const uint8_t streamingMask,
      const uint8_t enabledMask)
  {
    node.flags &= ~(vg::HIERARCHY_STREAMING_MASK_BITS |
                    vg::HIERARCHY_ENABLED_MASK_BITS | vg::STREAMING_LEAF_FLAG);
    node.flags |= static_cast<uint32_t>(streamingMask) << vg::HIERARCHY_STREAMING_MASK_SHIFT;
    node.flags |= static_cast<uint32_t>(enabledMask) << vg::HIERARCHY_ENABLED_MASK_SHIFT;
    if (streamingMask != 0u)
      node.flags |= vg::STREAMING_LEAF_FLAG;
  }

  // Exactly what the runtime does: reset, apply the install updates of every
  // resident page in page order, then fully enable installed nodes no rewrite
  // ever mentions.
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
        setMasks(
            hierarchy[update.hierarchyNodeIndex],
            update.streamingLeafsBitset,
            update.enabledClustersBitset);
    }

    for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.size(); ++nodeIndex)
    {
      auto &node = hierarchy[nodeIndex];
      if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
        continue;
      if ((node.pageIndex & kPageNotInstalledBit) != 0u)
        continue;
      if (rewritten.count(nodeIndex) != 0u)
        continue;
      setMasks(node, 0u, fullMaskForCount(node.child_count));
    }
  }

  bool pageInstalled(const uint32_t page) const { return installed[page] != 0u; }

  bool canInstall(const uint32_t page) const
  {
    for (const uint32_t dependency : build.pages[page].dependencies)
      if (installed[dependency] == 0u)
        return false;
    return true;
  }

  void install(const uint32_t page)
  {
    installed[page] = 1u;
    applyRewrites();
  }

  void uninstall(const uint32_t page)
  {
    installed[page] = 0u;
    applyRewrites();
  }

  uint32_t installedPageCount() const
  {
    return static_cast<uint32_t>(std::count(installed.begin(), installed.end(), uint8_t{1u}));
  }

  bool clusterResident(const uint32_t cluster) const
  {
    const uint32_t nodeIndex = clusterNode[cluster];
    if (nodeIndex == UINT32_MAX)
      return false;
    return (hierarchy[nodeIndex].pageIndex & kPageNotInstalledBit) == 0u;
  }

  bool clusterEnabled(const uint32_t cluster) const
  {
    if (!clusterResident(cluster))
      return false;
    const auto &node = hierarchy[clusterNode[cluster]];
    const uint8_t mask = static_cast<uint8_t>(
        (node.flags & vg::HIERARCHY_ENABLED_MASK_BITS) >> vg::HIERARCHY_ENABLED_MASK_SHIFT);
    return ((mask >> clusterBit[cluster]) & 1u) != 0u;
  }

  bool clusterStreaming(const uint32_t cluster) const
  {
    if (!clusterResident(cluster))
      return false;
    const auto &node = hierarchy[clusterNode[cluster]];
    const uint8_t mask = static_cast<uint8_t>(
        (node.flags & vg::HIERARCHY_STREAMING_MASK_BITS) >> vg::HIERARCHY_STREAMING_MASK_SHIFT);
    return ((mask >> clusterBit[cluster]) & 1u) != 0u;
  }

  // The culler's rule, with the streaming-leaf override.
  bool clusterDrawn(const uint32_t cluster, const float threshold) const
  {
    if (!clusterEnabled(cluster))
      return false;
    const auto &data = build.clusters[cluster];
    // A streaming leaf cannot be refined past - its children are absent - so its
    // self error is forced to zero and it can never be rejected for being too
    // coarse. Coarsening is a separate question, and the parent test still
    // answers it: EXPERIMENT toggles whether that test is applied.
    const float selfError = clusterStreaming(cluster) ? 0.0f : data.self.error;
    if (selfError >= threshold)
      return false;
    if (streamingLeavesSkipParentTest && clusterStreaming(cluster))
      return true; // the older rule: a streaming leaf always drew
    return data.parent.error >= threshold;
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

struct StateReport
{
  uint32_t enabledOnUninstalledPage = 0u;
  uint32_t streamingNotEnabled = 0u;
  uint32_t enabledWithDisabledParent = 0u;
  // A path with nothing resident on it is not a hole - that geometry simply has
  // not been streamed yet. A hole is a path that has resident geometry and still
  // draws nothing.
  uint32_t pathsWithNoCluster = 0u;
  uint32_t pathsWithNothingResident = 0u;
  uint32_t pathsWithTwoClusters = 0u;
  uint32_t enabledClusters = 0u;
  uint32_t streamingClusters = 0u;
};

StateReport inspect(const Residency &state, const std::vector<float> &thresholds)
{
  StateReport report{};
  const uint32_t clusterCount = static_cast<uint32_t>(state.build.clusters.size());

  for (uint32_t cluster = 0u; cluster < clusterCount; ++cluster)
  {
    const bool enabled = state.clusterEnabled(cluster);
    const bool streaming = state.clusterStreaming(cluster);
    report.enabledClusters += enabled ? 1u : 0u;
    report.streamingClusters += streaming ? 1u : 0u;
    if (enabled && !state.pageInstalled(state.clusterPage[cluster]))
      ++report.enabledOnUninstalledPage;
    if (streaming && !enabled)
      ++report.streamingNotEnabled;
    // The valid-cut property: a fine cluster may only be enabled while every
    // coarser cluster that stands in for it is enabled too.
    if (enabled)
    {
      const uint32_t parent = state.clusterParent[cluster];
      if (parent != UINT32_MAX && !state.clusterEnabled(parent))
        ++report.enabledWithDisabledParent;
    }
  }

  for (const float threshold : thresholds)
    for (const uint32_t leaf : state.leafClusters)
    {
      uint32_t drawn = 0u;
      bool anyResident = false;
      for (uint32_t cursor = leaf; cursor != UINT32_MAX;
           cursor = state.clusterParent[cursor])
      {
        anyResident = anyResident || state.clusterEnabled(cursor);
        drawn += state.clusterDrawn(cursor, threshold) ? 1u : 0u;
      }
      if (drawn == 0u)
      {
        if (anyResident)
          ++report.pathsWithNoCluster;
        else
          ++report.pathsWithNothingResident;
      }
      else if (drawn > 1u)
        ++report.pathsWithTwoClusters;
    }
  return report;
}

std::vector<float> thresholdSweep(const vg::VirtualGeometryBuildData &build)
{
  float finest = 0.0f;
  float coarsest = 0.0f;
  for (uint32_t cluster = 0u; cluster < build.clusters.size(); ++cluster)
  {
    if (build.clusterDAG[cluster].empty())
      finest = std::max(finest, build.clusters[cluster].self.error);
    if (build.clusters[cluster].parent.error == std::numeric_limits<float>::max())
      coarsest = std::max(coarsest, build.clusters[cluster].self.error);
  }
  std::vector<float> thresholds;
  for (float value = std::max(finest, 1e-3f); value <= coarsest * 4.0f; value *= 2.0f)
    thresholds.push_back(value);
  thresholds.push_back(coarsest * 8.0f);
  return thresholds;
}

// ─────────────────────────────────────────────────────────────────────────────

struct Fixture
{
  svr::SparseVoxelBrickHierarchy octree;
  vg::VirtualGeometryBuildData build;
  svr::SparseVoxelOctreeVirtualGeometryStats stats;
  uint64_t cellCount = 0u;
  bool valid = false;
};

Fixture buildFixture(const uint32_t side, const uint32_t maxGroupsPerPage)
{
  Fixture fixture{};
  std::string error;
  const auto octreeStart = std::chrono::steady_clock::now();
  if (!buildFloorOctree(side, fixture.octree, fixture.cellCount, &error))
  {
    std::cout << "  floor octree build failed: " << error << '\n';
    return fixture;
  }
  const double octreeMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - octreeStart)
                              .count();

  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = maxGroupsPerPage;
  buildSettings.maxRootPageGroups = 1u;

  const auto buildStart = std::chrono::steady_clock::now();
  fixture.valid = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.octree,
      0u,
      static_cast<uint32_t>(fixture.octree.nodes.size()),
      octreeSettings,
      buildSettings,
      fixture.build,
      &fixture.stats,
      &error);
  const double buildMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - buildStart)
                             .count();
  if (!fixture.valid)
  {
    std::cout << "  octree virtual geometry build failed: " << error << '\n';
    return fixture;
  }

  std::cout << std::fixed << std::setprecision(1)
            << "  floor " << side << "x" << side << " cells=" << fixture.cellCount
            << " bricks=" << fixture.octree.nodes.size()
            << " levels=" << fixture.stats.levelCount
            << " clusters=" << fixture.build.clusters.size()
            << " groups=" << fixture.build.groupInfos.size()
            << " pages=" << fixture.build.pages.size()
            << " octree_ms=" << octreeMs << " vg_ms=" << buildMs << '\n';
  return fixture;
}

std::vector<uint32_t> topologicalPageOrder(
    const vg::VirtualGeometryBuildData &build,
    const bool reverse)
{
  const uint32_t pageCount = static_cast<uint32_t>(build.pages.size());
  std::vector<uint32_t> inDegree(pageCount, 0u);
  std::vector<std::vector<uint32_t>> dependants(pageCount);
  for (uint32_t page = 0u; page < pageCount; ++page)
  {
    inDegree[page] = static_cast<uint32_t>(build.pages[page].dependencies.size());
    for (const uint32_t dependency : build.pages[page].dependencies)
      dependants[dependency].push_back(page);
  }
  std::vector<uint32_t> ready;
  for (uint32_t page = 0u; page < pageCount; ++page)
    if (inDegree[page] == 0u)
      ready.push_back(page);

  std::vector<uint32_t> order;
  order.reserve(pageCount);
  while (!ready.empty())
  {
    const auto pick = reverse ? std::max_element(ready.begin(), ready.end())
                              : std::min_element(ready.begin(), ready.end());
    const uint32_t page = *pick;
    ready.erase(pick);
    order.push_back(page);
    for (const uint32_t dependant : dependants[page])
      if (--inDegree[dependant] == 0u)
        ready.push_back(dependant);
  }
  return order;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

void testHugeOctreeSplitsIntoManyPages()
{
  std::cout << "a huge floor octree splits into many pages with real dependencies\n";

  Fixture fixture = buildFixture(256u, 8u);
  check(fixture.valid, "the huge floor octree builds virtual geometry");
  if (!fixture.valid)
    return;

  check(fixture.stats.levelCount >= 6u, "the octree runs at least six levels deep");
  check(fixture.build.pages.size() > 20u, "the DAG needs many pages");
  check(
      fixture.build.pages[0].groups.size() == 1u,
      "the root page holds a single parent group");

  uint32_t pagesWithDependencies = 0u;
  uint32_t maximumDependencies = 0u;
  for (const auto &page : fixture.build.pages)
  {
    pagesWithDependencies += page.dependencies.empty() ? 0u : 1u;
    maximumDependencies =
        std::max(maximumDependencies, static_cast<uint32_t>(page.dependencies.size()));
  }
  check(pagesWithDependencies + 1u >= fixture.build.pages.size() - 1u,
        "every page but the root depends on something");
  check(maximumDependencies >= 1u, "the dependency graph is not trivial");

  // Cluster ranges must tile the cluster array exactly once.
  std::vector<uint32_t> owners(fixture.build.clusters.size(), 0u);
  for (const auto &page : fixture.build.pages)
    for (uint32_t local = 0u; local < page.clusterCount; ++local)
      ++owners[page.clusterOffset + local];
  const bool tiled = std::all_of(owners.begin(), owners.end(),
                                 [](const uint32_t count) { return count == 1u; });
  check(tiled, "every cluster is owned by exactly one page");
}

void testCrossPageRewritesOnlyNameResidentPages()
{
  std::cout << "cross-page rewrites only ever name pages that are already resident\n";

  Fixture fixture = buildFixture(256u, 8u);
  check(fixture.valid, "the huge floor octree builds virtual geometry");
  if (!fixture.valid)
    return;

  // Which page owns each hierarchy node.
  std::vector<uint32_t> nodePage(fixture.build.lodLevelHierarchy.size(), UINT32_MAX);
  for (uint32_t nodeIndex = 0u; nodeIndex < fixture.build.lodLevelHierarchy.size();
       ++nodeIndex)
  {
    const auto &node = fixture.build.lodLevelHierarchy[nodeIndex];
    if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
      continue;
    nodePage[nodeIndex] = node.pageIndex & ~kPageNotInstalledBit;
  }

  // Transitive dependency closure per page.
  const uint32_t pageCount = static_cast<uint32_t>(fixture.build.pages.size());
  std::vector<std::set<uint32_t>> closure(pageCount);
  for (const uint32_t page : topologicalPageOrder(fixture.build, false))
    for (const uint32_t dependency : fixture.build.pages[page].dependencies)
    {
      closure[page].insert(dependency);
      closure[page].insert(closure[dependency].begin(), closure[dependency].end());
    }

  uint32_t crossPageUpdates = 0u;
  uint32_t sameOwnerUpdates = 0u;
  uint32_t danglingNodeUpdates = 0u;
  uint32_t updatesNamingAbsentPage = 0u;
  uint32_t reEmittedGroups = 0u;
  std::set<std::pair<uint32_t, uint32_t>> seenUpdate;

  for (uint32_t page = 0u; page < pageCount; ++page)
    for (const auto &update : fixture.build.pages[page].installUpdates.hierarchyUpdates)
    {
      if (update.hierarchyNodeIndex >= nodePage.size() ||
          nodePage[update.hierarchyNodeIndex] == UINT32_MAX)
      {
        ++danglingNodeUpdates;
        continue;
      }
      const uint32_t owner = nodePage[update.hierarchyNodeIndex];
      if (owner == page)
        ++sameOwnerUpdates;
      else
      {
        ++crossPageUpdates;
        // The owning page has to be resident when this update is applied, which
        // for a dependency-ordered install means it must be in the closure.
        if (closure[page].count(owner) == 0u)
          ++updatesNamingAbsentPage;
      }
      if (!seenUpdate.insert({page, update.hierarchyNodeIndex}).second)
        ++reEmittedGroups;
    }

  check(danglingNodeUpdates == 0u, "no rewrite names a hierarchy node that owns no page");
  check(sameOwnerUpdates > 0u, "pages rewrite their own hierarchy nodes");
  check(crossPageUpdates > 0u, "pages rewrite hierarchy nodes owned by coarser pages");
  check(
      updatesNamingAbsentPage == 0u,
      "every cross-page rewrite names a page in this page's dependency closure: " +
          std::to_string(updatesNamingAbsentPage));
  check(reEmittedGroups == 0u, "no page emits two updates for the same node");

  std::cout << "  rewrites: sameOwner=" << sameOwnerUpdates
            << " crossPage=" << crossPageUpdates << '\n';
}

void testEveryInstallStepKeepsAValidCut()
{
  std::cout << "every install step keeps a valid, renderable cut\n";

  Fixture fixture = buildFixture(256u, 8u);
  check(fixture.valid, "the huge floor octree builds virtual geometry");
  if (!fixture.valid)
    return;

  const std::vector<float> thresholds = thresholdSweep(fixture.build);
  check(thresholds.size() > 3u, "the threshold sweep spans the error range");

  Residency state(fixture.build);
  const std::vector<uint32_t> order = topologicalPageOrder(fixture.build, false);
  check(
      order.size() == fixture.build.pages.size(),
      "the page dependency graph is acyclic and fully orderable");

  uint32_t stepsWithEnabledOnAbsentPage = 0u;
  uint32_t stepsWithStreamingNotEnabled = 0u;
  uint32_t stepsWithBrokenClosure = 0u;
  uint32_t stepsWithHoles = 0u;
  uint32_t stepsWithDoubleDraw = 0u;
  uint32_t enabledClustersAtFirstStep = 0u;
  uint32_t steps = 0u;

  for (const uint32_t page : order)
  {
    if (!state.canInstall(page))
    {
      check(false, "page " + std::to_string(page) + " installs only after its dependencies");
      return;
    }
    state.install(page);
    ++steps;

    const StateReport report = inspect(state, thresholds);
    if (steps == 1u)
      enabledClustersAtFirstStep = report.enabledClusters;
    stepsWithEnabledOnAbsentPage += report.enabledOnUninstalledPage != 0u ? 1u : 0u;
    stepsWithStreamingNotEnabled += report.streamingNotEnabled != 0u ? 1u : 0u;
    stepsWithBrokenClosure += report.enabledWithDisabledParent != 0u ? 1u : 0u;
    stepsWithHoles += report.pathsWithNoCluster != 0u ? 1u : 0u;
    stepsWithDoubleDraw += report.pathsWithTwoClusters != 0u ? 1u : 0u;
  }

  check(steps == fixture.build.pages.size(), "every page was installed");
  check(enabledClustersAtFirstStep > 0u, "the root page alone already enables clusters");
  check(
      stepsWithEnabledOnAbsentPage == 0u,
      "no step ever enables a cluster whose page is not resident");
  check(
      stepsWithStreamingNotEnabled == 0u,
      "no step marks a cluster streaming without enabling it");
  check(
      stepsWithBrokenClosure == 0u,
      "the enabled set stays closed under DAG parents at every step");
  check(stepsWithHoles == 0u, "no install step leaves a root-to-leaf path undrawn");
  check(
      stepsWithDoubleDraw == 0u,
      "no install step draws a parent and its children on the same path");

  // Everything available, nothing left on the border.
  const StateReport finalReport = inspect(state, thresholds);
  check(
      finalReport.enabledClusters == fixture.build.clusters.size(),
      "every cluster is available once every page is resident: " +
          std::to_string(finalReport.enabledClusters) + "/" +
          std::to_string(fixture.build.clusters.size()));
  check(
      finalReport.streamingClusters == 0u,
      "no cluster is left on the residency border once everything is resident");
}

void testInstallOrderAndUninstallSymmetry()
{
  std::cout << "install order is irrelevant and uninstall retraces the same states\n";

  Fixture fixture = buildFixture(128u, 6u);
  check(fixture.valid, "the floor octree builds virtual geometry");
  if (!fixture.valid)
    return;

  const std::vector<float> thresholds = thresholdSweep(fixture.build);

  const auto installAll = [&](const bool reverse)
  {
    Residency state(fixture.build);
    for (const uint32_t page : topologicalPageOrder(fixture.build, reverse))
      state.install(page);
    return state.flagSnapshot();
  };
  check(
      installAll(false) == installAll(true),
      "two different legal install orders reach the same hierarchy state");

  // Walk up recording every state, then walk back down and compare.
  Residency state(fixture.build);
  const std::vector<uint32_t> order = topologicalPageOrder(fixture.build, false);
  std::vector<std::vector<uint32_t>> snapshots;
  snapshots.push_back(state.flagSnapshot());
  for (const uint32_t page : order)
  {
    state.install(page);
    snapshots.push_back(state.flagSnapshot());
  }

  uint32_t mismatchedSnapshots = 0u;
  uint32_t uninstallStepsWithHoles = 0u;
  uint32_t uninstallStepsWithDoubleDraw = 0u;
  uint32_t uninstallStepsWithBrokenClosure = 0u;
  for (size_t index = order.size(); index-- > 0u;)
  {
    state.uninstall(order[index]);
    if (state.flagSnapshot() != snapshots[index])
      ++mismatchedSnapshots;
    const StateReport report = inspect(state, thresholds);
    uninstallStepsWithHoles += report.pathsWithNoCluster != 0u ? 1u : 0u;
    uninstallStepsWithDoubleDraw += report.pathsWithTwoClusters != 0u ? 1u : 0u;
    uninstallStepsWithBrokenClosure += report.enabledWithDisabledParent != 0u ? 1u : 0u;
  }

  check(
      mismatchedSnapshots == 0u,
      "uninstalling retraces every install state bit for bit: " +
          std::to_string(mismatchedSnapshots) + " mismatches");
  check(uninstallStepsWithHoles == 0u, "no uninstall step opens a hole");
  check(uninstallStepsWithDoubleDraw == 0u, "no uninstall step draws a path twice");
  check(
      uninstallStepsWithBrokenClosure == 0u,
      "the enabled set stays closed under DAG parents while evicting");
  check(state.installedPageCount() == 0u, "everything is evicted at the end");
}

void testPartialResidencyAlwaysRenders()
{
  std::cout << "any dependency-closed subset of pages still renders a complete cut\n";

  Fixture fixture = buildFixture(128u, 6u);
  check(fixture.valid, "the floor octree builds virtual geometry");
  if (!fixture.valid)
    return;

  const std::vector<float> thresholds = thresholdSweep(fixture.build);
  const std::vector<uint32_t> order = topologicalPageOrder(fixture.build, false);

  // A handful of deterministic prefixes of legal install orders, which is what
  // a streamer under a budget actually produces.
  uint32_t subsetsChecked = 0u;
  uint32_t subsetsWithHoles = 0u;
  uint32_t subsetsWithDoubleDraw = 0u;
  uint32_t subsetsWithUnusableCluster = 0u;
  // Prefixes of a legal topological order are exactly the dependency-closed
  // subsets a budgeted streamer produces; arbitrary skips are not closed and the
  // runtime would refuse them.
  for (uint32_t startOffset = 0u; startOffset < 3u; ++startOffset)
  {
    Residency state(fixture.build);
    for (size_t index = 0u; index < order.size(); ++index)
    {
      if (!state.canInstall(order[index]))
        continue;
      state.install(order[index]);
      if (index % (startOffset + 1u) != 0u)
        continue;

      ++subsetsChecked;
      const StateReport report = inspect(state, thresholds);
      subsetsWithHoles += report.pathsWithNoCluster != 0u ? 1u : 0u;
      subsetsWithDoubleDraw += report.pathsWithTwoClusters != 0u ? 1u : 0u;
      subsetsWithUnusableCluster += report.enabledOnUninstalledPage != 0u ? 1u : 0u;
    }
  }

  check(subsetsChecked > 20u, "many partial residency states were exercised");
  check(subsetsWithHoles == 0u, "no partial residency state opens a hole");
  check(subsetsWithDoubleDraw == 0u, "no partial residency state draws a path twice");
  check(
      subsetsWithUnusableCluster == 0u,
      "no partial residency state enables a cluster on an absent page");
}

void testStreamingLeafParentTestExperiment()
{
  std::cout << "the streaming-leaf selection rule\n";

  Fixture fixture = buildFixture(128u, 6u);
  check(fixture.valid, "the floor octree builds virtual geometry");
  if (!fixture.valid)
    return;

  const std::vector<float> thresholds = thresholdSweep(fixture.build);
  const std::vector<uint32_t> order = topologicalPageOrder(fixture.build, false);

  const auto sweep = [&](const bool skipParentTest)
  {
    streamingLeavesSkipParentTest = skipParentTest;
    Residency state(fixture.build);
    StateReport totals{};
    for (const uint32_t page : order)
    {
      state.install(page);
      const StateReport report = inspect(state, thresholds);
      totals.pathsWithNoCluster += report.pathsWithNoCluster;
      totals.pathsWithTwoClusters += report.pathsWithTwoClusters;
    }
    return totals;
  };

  const StateReport shipping = sweep(true);
  const StateReport proposed = sweep(false);
  streamingLeavesSkipParentTest = false;

  std::cout << "  legacy rule (streaming leaf skips the parent test): holes="
            << shipping.pathsWithNoCluster << " doubleDraws="
            << shipping.pathsWithTwoClusters << '\n'
            << "  shipping rule (streaming leaf keeps the parent test): holes="
            << proposed.pathsWithNoCluster << " doubleDraws="
            << proposed.pathsWithTwoClusters << '\n';

  check(
      proposed.pathsWithNoCluster == 0u,
      "the shipping rule opens no holes");
  check(
      proposed.pathsWithTwoClusters == 0u,
      "the shipping rule removes every double draw");
  check(
      shipping.pathsWithTwoClusters > 0u,
      "the legacy rule did draw a coarse cluster over a streaming leaf");
}

// How full are pages at the settings the runtime actually uses? The other tests
// force tiny pages (4-8 groups) to make many of them; this measures the default.
void testDefaultPackingFillsPages()
{
  std::cout << "default page packing fills pages across DAG levels\n";

  for (const uint32_t groupsPerPage : {8u, vg::MAX_GROUPS_PER_PAGE})
  {
    Fixture fixture = buildFixture(256u, groupsPerPage);
    if (!fixture.valid)
    {
      check(false, "the floor octree builds virtual geometry");
      continue;
    }

    const uint32_t pageCount = static_cast<uint32_t>(fixture.build.pages.size());
    uint32_t fullGroupPages = 0u;
    uint32_t multiLevelPages = 0u;
    uint32_t totalGroups = 0u;
    uint32_t totalClusters = 0u;
    uint32_t maximumClusters = 0u;
    for (uint32_t page = 0u; page < pageCount; ++page)
    {
      const auto &pageData = fixture.build.pages[page];
      totalGroups += static_cast<uint32_t>(pageData.groups.size());
      totalClusters += pageData.clusterCount;
      maximumClusters = std::max(maximumClusters, pageData.clusterCount);
      fullGroupPages += pageData.groups.size() >= groupsPerPage ? 1u : 0u;
      std::set<uint32_t> levels;
      for (uint32_t local = 0u; local < pageData.clusterCount; ++local)
        levels.insert(fixture.stats.clusterSources[pageData.clusterOffset + local].level);
      multiLevelPages += levels.size() > 1u ? 1u : 0u;
    }

    std::cout << "  groupsPerPage=" << groupsPerPage << " pages=" << pageCount
              << " meanGroups=" << std::setprecision(3)
              << static_cast<double>(totalGroups) / pageCount
              << " meanClusters=" << static_cast<double>(totalClusters) / pageCount
              << " maxClusters=" << maximumClusters
              << " fullPages=" << fullGroupPages << "/" << pageCount
              << " multiLevelPages=" << multiLevelPages << '\n';

    // Every page except the root-group page and the tail should be full.
    check(
        fullGroupPages + 2u >= pageCount,
        "pages are filled to the group budget at groupsPerPage=" +
            std::to_string(groupsPerPage) + " (" + std::to_string(fullGroupPages) +
            "/" + std::to_string(pageCount) + ")");
  }
}

// Geometry that straddles the origin gets a shorter LOD chain than the same
// geometry placed in positive coordinates.
//
// The brick octree coarsens by halving its integer brick keys. floorDiv(-1, 2)
// is -1, so a key of -1 stays -1 at every level and never merges with key 0:
// coarsening stalls with several permanent roots instead of converging on one.
// A page straddling the world origin therefore has fewer octree levels to
// refine through than an identical page that does not.
void testStraddlingTheOriginShortensTheLODChain()
{
  std::cout << "geometry straddling the origin gets a shorter LOD chain\n";

  const auto buildAt = [](const int32_t originShift, uint32_t &levels, uint32_t &clusters)
  {
    std::vector<svr::SparseVoxelCenterLengthGPU> centersLengths;
    std::vector<svr::SparseVoxelMaterialGPU> materials;
    std::vector<uint32_t> leaves;
    constexpr uint32_t kSide = 64u;
    for (uint32_t x = 0u; x < kSide; ++x)
      for (uint32_t z = 0u; z < kSide; ++z)
        for (uint32_t y = 0u; y < 2u; ++y)
        {
          svr::SparseVoxelCenterLengthGPU cell{};
          cell.center = {
              static_cast<float>(static_cast<int32_t>(x) + originShift) + 0.5f,
              static_cast<float>(y) + 0.5f,
              static_cast<float>(static_cast<int32_t>(z) + originShift) + 0.5f};
          cell.length = 1.0f;
          svr::SparseVoxelMaterialGPU material{};
          material.materialId = (x ^ z) & 1u;
          material.albedoRGBA8 = 0xff808080u + material.materialId;
          leaves.push_back(static_cast<uint32_t>(centersLengths.size()));
          centersLengths.push_back(cell);
          materials.push_back(material);
        }

    svr::SparseVoxelBrickHierarchy hierarchy{};
    svr::SparseVoxelBrickBuildSettings settings{};
    settings.voxelSize = 1.0f;
    settings.maximumBricks = 1u << 18u;
    std::string error;
    if (!svr::buildSparseVoxelBrickHierarchy(
            centersLengths, materials, leaves, settings, hierarchy, &error))
      return false;

    svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
    vg::VirtualGeometryBuildSettings buildSettings{};
    buildSettings.maxGroupsPerPage = 8u;
    vg::VirtualGeometryBuildData build{};
    svr::SparseVoxelOctreeVirtualGeometryStats stats{};
    if (!svr::buildSparseVoxelOctreeVirtualGeometry(
            hierarchy, 0u, static_cast<uint32_t>(hierarchy.nodes.size()),
            octreeSettings, buildSettings, build, &stats, &error))
      return false;
    levels = stats.levelCount;
    clusters = static_cast<uint32_t>(build.clusters.size());
    return true;
  };

  uint32_t positiveLevels = 0u;
  uint32_t positiveClusters = 0u;
  uint32_t straddlingLevels = 0u;
  uint32_t straddlingClusters = 0u;
  check(buildAt(0, positiveLevels, positiveClusters), "the positive-coordinate page builds");
  check(buildAt(-32, straddlingLevels, straddlingClusters), "the origin-straddling page builds");

  std::cout << "  positive origin: levels=" << positiveLevels
            << " clusters=" << positiveClusters
            << " | straddling origin: levels=" << straddlingLevels
            << " clusters=" << straddlingClusters << '\n';

  // KNOWN DEFECT, characterised here rather than asserted away.
  //
  // buildSparseVoxelBrickHierarchy coarsens by halving signed brick keys, and
  // floorDiv(-1, 2) is -1: key -1 stays -1 at every level and never merges with
  // key 0, so coarsening stalls with several permanent roots instead of
  // converging on one. Identical geometry therefore gets a shorter LOD chain on
  // the negative side of an axis, which is exactly the ground a world-space
  // terrain page sits on once it crosses the origin.
  //
  // If this is fixed, this check fails and should become an equality.
  check(
      straddlingLevels < positiveLevels,
      "KNOWN DEFECT: straddling the origin loses an octree level (" +
          std::to_string(straddlingLevels) + " vs " + std::to_string(positiveLevels) +
          ") - signed brick keys never coarsen past -1");
}

} // namespace

int main()
{
  std::cout << "Sparse voxel octree page rewrite tests\n";
  testHugeOctreeSplitsIntoManyPages();
  testCrossPageRewritesOnlyNameResidentPages();
  testEveryInstallStepKeepsAValidCut();
  testInstallOrderAndUninstallSymmetry();
  testPartialResidencyAlwaysRenders();
  testStreamingLeafParentTestExperiment();
  testDefaultPackingFillsPages();
  testStraddlingTheOriginShortensTheLODChain();

  if (failures != 0)
  {
    std::cout << failures << " sparse voxel octree rewrite checks failed\n";
    return 1;
  }
  std::cout << "all sparse voxel octree rewrite checks passed\n";
  return 0;
}
