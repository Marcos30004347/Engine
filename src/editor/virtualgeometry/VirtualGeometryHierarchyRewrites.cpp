#include "VirtualGeometryHierarchyRewrites.hpp"
#include "VirtualGeometryHierarchyBuilder.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define VG_BUILD_LOG(...)                                                      \
  do                                                                           \
  {                                                                            \
    if (virtualgeometry::virtualGeometryBuildLoggingEnabled())                 \
      std::printf(__VA_ARGS__);                                                \
  } while (false)

namespace virtualgeometry
{

namespace
{

static constexpr uint32_t PAGE_NOT_INSTALLED_BIT = (1u << 31);

// Estimated encoded size of one cluster, for the page byte budget.
//
// The packer runs before the encoder, so this cannot be the real figure. It
// only has to be monotone in the geometry and deterministic, which it is: a
// cluster's index list is the one field every route fills (the voxel builder
// leaves `vertices` empty and indexes a shared array), and the encoder's output
// tracks it closely because positions, normals and uvs are all per referenced
// vertex. Calibrated against terrain: 217 clusters per page for 166 KiB, so
// about 784 bytes per cluster at 384 indices - two bytes an index.
static uint64_t estimateClusterBytes(const VirtualGeometryCluster &cluster)
{
  const uint64_t indices = static_cast<uint64_t>(cluster.indices.size());
  const uint64_t vertices = static_cast<uint64_t>(cluster.vertices.size());
  // 12 bytes a vertex where the cluster owns its vertices, otherwise the
  // index-derived estimate; both land within a few percent of the measured
  // bytes per cluster.
  return vertices != 0u ? vertices * 12ull + indices : indices * 2ull;
}

// One group's clusters on one page. `firstClusterInGroup` is zero for a group
// placed whole; a split group contributes one span per page it reaches, in
// page order, and the spans partition its cluster list.
struct PageGroupSpan
{
  uint32_t groupId = UINT32_MAX;
  uint32_t firstClusterInGroup = 0u;
  uint32_t clusterCount = 0u;
};

// Where a group's clusters ended up. Appended as the packer places them, so a
// prefix of this list is exactly "the spans placed on pages up to now".
struct GroupPlacement
{
  uint32_t page = UINT32_MAX;
  uint32_t firstClusterInGroup = 0u;
  uint32_t clusterCount = 0u;
};

// A rewrite recorded before the hierarchy knows its final node indices.
//
// A split group's leaf is not divided until after page assignment, so an
// update emitted while packing cannot name its node yet. It names the group
// and the span instead, and the export pass resolves both once the split has
// run. The bitsets are group-local here and re-based to the span there.
struct PendingHierarchyUpdate
{
  uint32_t groupId = UINT32_MAX;
  uint32_t firstClusterInGroup = 0u;
  uint32_t clusterCount = 0u;
  uint8_t streamingLeafsBitset = 0u;
  uint8_t enabledClustersBitset = 0u;
};

struct PageBuildState
{
  std::vector<PageGroupSpan> groups;
  std::vector<PendingHierarchyUpdate> updates;
  uint32_t clusterCount = 0u;
  uint64_t estimatedBytes = 0u;
  std::unordered_set<uint32_t> dependencies;
  PageUpdateList uninstallUpdates;
};

static uint8_t fullMaskForCount(uint32_t count)
{
  if (count == 0)
    return 0u;
  if (count >= 8u)
    return 0xFFu;
  return static_cast<uint8_t>((1u << count) - 1u);
}

static uint8_t bitsetForGroup(const std::vector<uint32_t> &groupClusters, const std::vector<uint8_t> &membership)
{
  uint8_t bits = 0u;
  for (uint32_t i = 0; i < groupClusters.size() && i < 8u; ++i)
  {
    const uint32_t c = groupClusters[i];
    if (c < membership.size() && membership[c] != 0u)
      bits = static_cast<uint8_t>(bits | (1u << i));
  }
  return bits;
}

// Clusters on the residency border of a page: resident once this page and its
// dependencies are installed, but standing in for finer clusters that are not.
//
// The culler forces a streaming leaf's self error to zero so it always draws,
// which is what keeps a partially streamed DAG free of holes. Deriving this
// from the *global* cut is wrong: page construction expands the cut until it
// has enough groups to fill a page, so the cut at emit time routinely already
// contains clusters that were pushed to the next page. A cluster whose children
// live in a page that is not resident yet would then be marked interior, and
// nothing on that root-to-leaf path draws at all.
//
// A group whose bitset comes back non-zero stays in `pendingInstalls`, so a
// later page re-emits it once its children become resident and the mark clears.
// Keyed by *cluster* rather than by group: with `splitGroupsAcrossPages` a
// group's clusters can land on different pages, so "is this cluster resident
// by the time `pageIndex` is installed" is a per-cluster question. For an
// unsplit group every cluster carries its group's page and the answer is
// unchanged.
static uint8_t streamingBitsetForGroup(
    const std::vector<uint32_t> &groupClusters,
    const std::vector<std::vector<uint32_t>> &clusterDAG,
    const std::vector<uint32_t> &clusterPage,
    uint32_t pageIndex)
{
  const auto resident = [&](uint32_t cluster)
  {
    return cluster < clusterPage.size() && clusterPage[cluster] != UINT32_MAX &&
        clusterPage[cluster] <= pageIndex;
  };

  uint8_t bits = 0u;
  for (uint32_t i = 0; i < groupClusters.size() && i < 8u; ++i)
  {
    const uint32_t cluster = groupClusters[i];
    if (!resident(cluster) || cluster >= clusterDAG.size())
      continue;
    for (uint32_t child : clusterDAG[cluster])
      if (!resident(child))
      {
        bits = static_cast<uint8_t>(bits | (1u << i));
        break;
      }
  }
  return bits;
}

// Clusters with nothing finer in the DAG. With
// `finestClustersAreStreamingLeaves` these carry the streaming mark too, but
// unlike a mark from an unresident child it never clears inside this build -
// residency clears it at runtime - so it must not hold a group pending.
static uint8_t finestBitsetForGroup(
    const std::vector<uint32_t> &groupClusters,
    const std::vector<std::vector<uint32_t>> &clusterDAG)
{
  uint8_t bits = 0u;
  for (uint32_t i = 0; i < groupClusters.size() && i < 8u; ++i)
  {
    const uint32_t cluster = groupClusters[i];
    if (cluster < clusterDAG.size() && clusterDAG[cluster].empty())
      bits = static_cast<uint8_t>(bits | (1u << i));
  }
  return bits;
}

// One minimal topological expansion layer:
// add all nodes not in cut whose incoming dependencies are all already in cut.
static uint32_t expandCut(std::vector<uint8_t> &cut, const std::vector<std::vector<uint32_t>> &reverseClusterDAG)
{
  std::vector<uint32_t> ready;
  ready.reserve(cut.size());

  for (uint32_t c = 0; c < cut.size(); ++c)
  {
    if (cut[c] != 0u)
      continue;

    bool allParentsInCut = true;
    for (uint32_t p : reverseClusterDAG[c])
    {
      if (cut[p] == 0u)
      {
        allParentsInCut = false;
        break;
      }
    }

    if (allParentsInCut)
      ready.push_back(c);
  }

  for (uint32_t c : ready)
    cut[c] = 1u;

  return static_cast<uint32_t>(ready.size());
}

// Border nodes of the cut:
// node is in cut and has at least one DAG child outside the cut.
static std::vector<uint8_t> leafsOfCut(const std::vector<uint8_t> &cut, const std::vector<std::vector<uint32_t>> &clusterDAG)
{
  std::vector<uint8_t> leafs(cut.size(), 0u);
  for (uint32_t c = 0; c < cut.size(); ++c)
  {
    if (cut[c] == 0u)
      continue;

    bool border = false;
    for (uint32_t child : clusterDAG[c])
    {
      if (cut[child] == 0u)
      {
        border = true;
        break;
      }
    }
    leafs[c] = border ? 1u : 0u;
  }
  return leafs;
}

static std::unordered_map<uint32_t, uint32_t> buildGroupToHierarchyMap(const std::vector<VirtualGeometryHierarchy> &hierarchy, const std::vector<VirtualGeometryCluster> &clusters)
{
  std::unordered_map<uint32_t, uint32_t> groupToHierarchy;

  for (uint32_t nodeIdx = 0; nodeIdx < hierarchy.size(); ++nodeIdx)
  {
    const VirtualGeometryHierarchy &node = hierarchy[nodeIdx];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
      continue;

    const uint32_t firstCluster = node.child_start;
    assert(firstCluster < clusters.size());
    const uint32_t groupId = clusters[firstCluster].groupId;

    for (uint32_t i = 0; i < node.child_count; ++i)
    {
      const uint32_t ci = firstCluster + i;
      assert(ci < clusters.size());
      assert(clusters[ci].groupId == groupId && "Leaf node must contain clusters from one group");
    }

    assert(groupToHierarchy.find(groupId) == groupToHierarchy.end());
    groupToHierarchy[groupId] = nodeIdx;
  }

  return groupToHierarchy;
}

static void dedupeAndSortUpdates(PageUpdateList &list)
{
  std::unordered_map<uint32_t, HierarchyClusterFlagsUpdate> lastPerNode;
  lastPerNode.reserve(list.hierarchyUpdates.size());

  for (const HierarchyClusterFlagsUpdate &u : list.hierarchyUpdates)
    lastPerNode[u.hierarchyNodeIndex] = u;

  list.hierarchyUpdates.clear();
  list.hierarchyUpdates.reserve(lastPerNode.size());
  for (const auto &kv : lastPerNode)
    list.hierarchyUpdates.push_back(kv.second);

  std::sort(
      list.hierarchyUpdates.begin(),
      list.hierarchyUpdates.end(),
      [](const HierarchyClusterFlagsUpdate &a, const HierarchyClusterFlagsUpdate &b)
      {
        return a.hierarchyNodeIndex < b.hierarchyNodeIndex;
      });
}

static void printClusterCutState(uint32_t iteration, const std::vector<uint8_t> &cut, const std::vector<uint8_t> &leafs)
{
  uint32_t cutCount = 0u;
  uint32_t leafCount = 0u;
  for (uint8_t v : cut)
    cutCount += static_cast<uint32_t>(v != 0u);
  for (uint8_t v : leafs)
    leafCount += static_cast<uint32_t>(v != 0u);

  VG_BUILD_LOG("    Iteration %u: cut=%u clusters, borderLeafs=%u clusters\n", iteration, cutCount, leafCount);
}

static uint32_t findShapeRootPage(const std::vector<VirtualGeometryHierarchy> &hierarchy,
                                 uint32_t rootNodeIndex, std::vector<VirtualGeometryBuildPage> &pages)
{
  if (rootNodeIndex >= hierarchy.size())
    return UINT32_MAX;

  uint32_t bestPageIndex = UINT32_MAX;
  float bestError = -std::numeric_limits<float>::infinity();
  uint32_t lastRootPageIndex = UINT32_MAX;
  std::vector<uint32_t> rootPages;

  std::vector<uint32_t> pending = {rootNodeIndex};
  while (!pending.empty())
  {
    const uint32_t nodeIndex = pending.back();
    pending.pop_back();
    if (nodeIndex >= hierarchy.size())
      continue;

    const VirtualGeometryHierarchy &node = hierarchy[nodeIndex];
    if ((node.flags & HIERARCHY_LEAF_FLAG) != 0u)
    {
      if (node.pageIndex == UINT32_MAX)
        continue;

      const uint32_t pageIndex = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
      // A shape can be a forest of independent DAG roots, with different
      // self errors and pages. Its initial resident prefix must include all
      // of them. Choosing only the largest-error root's page left holes when
      // a streamed shape replaced its spatial fallback.
      if (node.max_parent_lod_error >= std::numeric_limits<float>::max())
      {
        lastRootPageIndex = lastRootPageIndex == UINT32_MAX
            ? pageIndex : std::max(lastRootPageIndex, pageIndex);
        rootPages.push_back(pageIndex);
      }
      if (bestPageIndex == UINT32_MAX || node.min_lod_error > bestError)
      {
        bestPageIndex = pageIndex;
        bestError = node.min_lod_error;
      }
      continue;
    }

    for (uint32_t childIndex = 0u; childIndex < node.child_count; ++childIndex)
      pending.push_back(node.child_start + childIndex);
  }

  if (lastRootPageIndex < pages.size())
  {
    // The file streamer starts from one root page, not a contiguous prefix.
    // Make the entire independent root cut reachable through its ordinary
    // dependency installer. All added edges point to earlier pages.
    auto &dependencies = pages[lastRootPageIndex].dependencies;
    for (const uint32_t page : rootPages)
      if (page < lastRootPageIndex)
        dependencies.push_back(page);
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
  }
  return lastRootPageIndex != UINT32_MAX ? lastRootPageIndex : bestPageIndex;
}

} // namespace

void VirtualGeometryHierarchyRewrites::buildPagesAndRewrites(VirtualGeometryBuildData &data, const VirtualGeometryBuildSettings &settings)
{
  auto &hierarchy = data.lodLevelHierarchy;
  auto &clusters = data.clusters;
  auto &groupInfos = data.groupInfos;
  auto &clusterDAG = data.clusterDAG;
  const uint32_t maxGroupsPerPage = std::min(std::max(1u, settings.maxGroupsPerPage), MAX_GROUPS_PER_PAGE);
  const uint32_t maxClustersPerPage = MAX_GROUPS_PER_PAGE * GroupSize;

  const uint32_t clusterCount = static_cast<uint32_t>(clusters.size());
  const uint32_t groupCount = static_cast<uint32_t>(groupInfos.size());
  assert(clusterDAG.size() == clusterCount && "clusterDAG must contain one adjacency list per cluster");

  VG_BUILD_LOG("Building pages and rewrites from cluster DAG...\n");
  VG_BUILD_LOG("  clusters=%u groups=%u maxGroupsPerPage=%u maxClustersPerPage=%u maxRootPageGroups=%u\n", clusterCount, groupCount, maxGroupsPerPage, maxClustersPerPage, settings.maxRootPageGroups);

  // Build reverse DAG once for cut expansion.
  std::vector<std::vector<uint32_t>> reverseClusterDAG(clusterCount);
  for (uint32_t parent = 0; parent < clusterCount; ++parent)
    for (uint32_t child : clusterDAG[parent])
    {
      assert(child < clusterCount);
      reverseClusterDAG[child].push_back(parent);
    }

  std::vector<uint32_t> clusterToGroup(clusterCount, UINT32_MAX);
  for (uint32_t c = 0; c < clusterCount; ++c)
  {
    assert(clusters[c].groupId < groupCount);
    clusterToGroup[c] = clusters[c].groupId;
  }

  std::vector<std::vector<uint32_t>> groupClusters(groupCount);
  groupClusters.reserve(groupCount);
  for (uint32_t g = 0; g < groupCount; ++g)
  {
    for (int32_t ci : groupInfos[g].originalClusterIndices)
    {
      assert(ci >= 0);
      const uint32_t c = static_cast<uint32_t>(ci);
      assert(c < clusterCount);
      groupClusters[g].push_back(c);
    }
    assert(groupClusters[g].size() <= 8u && "Each group must have at most 8 clusters");
  }

  std::unordered_map<uint32_t, uint32_t> groupToHierarchy = buildGroupToHierarchyMap(hierarchy, clusters);
  VG_BUILD_LOG("  mapped %zu group(s) to hierarchy leaf node(s)\n", groupToHierarchy.size());

  // ---------------------------------------------------------------------------
  // Page generation + rewrite generation (cut-driven)
  // ---------------------------------------------------------------------------
  std::vector<PageBuildState> pages;
  std::vector<uint8_t> cut(clusterCount, 0u);
  std::vector<uint8_t> leafs(clusterCount, 0u);
  std::vector<uint8_t> groupAssigned(groupCount, 0u);
  // Clusters of each group already placed, so a group can be resumed on the
  // next page. Equal to the group's size exactly when `groupAssigned` is set.
  std::vector<uint32_t> groupPlacedClusters(groupCount, 0u);
  std::vector<std::vector<GroupPlacement>> groupPlacements(groupCount);
  // Per cluster, the page holding it. This is what residency is asked about
  // now that a group can straddle pages.
  std::vector<uint32_t> clusterPage(clusterCount, UINT32_MAX);
  const bool splitGroups = settings.splitGroupsAcrossPages;
  std::unordered_set<uint32_t> pendingInstalls;

  uint32_t cutCount = 0u;
  uint32_t iteration = 0u;

  auto collectVisibleUnassignedGroups = [&]() -> std::vector<uint32_t>
  {
    std::unordered_set<uint32_t> pendingGroupsSet;
    for (uint32_t c = 0; c < clusterCount; ++c)
    {
      if (cut[c] == 0u)
        continue;

      const uint32_t g = clusterToGroup[c];
      if (groupAssigned[g] == 0u)
        pendingGroupsSet.insert(g);
    }

    std::vector<uint32_t> pendingGroups(pendingGroupsSet.begin(), pendingGroupsSet.end());
    std::sort(pendingGroups.begin(), pendingGroups.end());
    return pendingGroups;
  };

  auto emitUpdate = [&](PageBuildState &pageState, uint32_t groupId, uint32_t emitPageIndex) -> std::pair<uint8_t, uint8_t>
  {
    const auto it = groupToHierarchy.find(groupId);
    if (it == groupToHierarchy.end())
      return {0u, 0u};

    // The returned streaming bits are only those an unresident child made:
    // they are what keeps a group pending until a later page clears them.
    const uint8_t pendingBits = streamingBitsetForGroup(
        groupClusters[groupId], clusterDAG, clusterPage, emitPageIndex);
    const uint8_t finestBits = settings.finestClustersAreStreamingLeaves
        ? finestBitsetForGroup(groupClusters[groupId], clusterDAG)
        : 0u;
    const uint8_t streamingBits = static_cast<uint8_t>(pendingBits | finestBits);
    // A half-resident group must not draw.
    //
    // A group is the substitution for its DAG children as a *unit*: its
    // clusters share one parent bound, and the coarse cluster they replace
    // stays a streaming leaf - self error forced to zero, always drawn - until
    // every one of them is resident. So enabling the clusters of a group whose
    // remaining spans are still on later pages would draw them *over* that
    // coarse cluster: half a substitution taken, and the overlap
    // `testPagesHoldWholeGroups` was written to prevent.
    //
    // The enabled mask expresses it exactly: a group is enabled only once its
    // last span has landed. Until then its spans carry a zero mask, which also
    // keeps the group in `pendingInstalls`, so the page that completes it
    // re-emits every span with the real mask. With splitting off a group is
    // always complete at its own page and this is the mask it always had.
    const bool groupComplete =
        groupPlacedClusters[groupId] == groupClusters[groupId].size() &&
        !groupPlacements[groupId].empty() &&
        groupPlacements[groupId].back().page <= emitPageIndex;
    const uint8_t enabledBits =
        groupComplete ? bitsetForGroup(groupClusters[groupId], cut) : 0u;
    // One update per span the group has placed so far. A span on a later page
    // does not exist yet - its clusters are not resident at this point in the
    // install order - and the page that places it emits its own update.
    for (const GroupPlacement &placement : groupPlacements[groupId])
    {
      if (placement.page > emitPageIndex)
        continue;
      PendingHierarchyUpdate update;
      update.groupId = groupId;
      update.firstClusterInGroup = placement.firstClusterInGroup;
      update.clusterCount = placement.clusterCount;
      update.streamingLeafsBitset = streamingBits;
      update.enabledClustersBitset = enabledBits;
      pageState.updates.push_back(update);
    }
    return {pendingBits, enabledBits};
  };

  while (true)
  {
    if (cutCount >= clusterCount)
    {
      const std::vector<uint32_t> remainingGroups = collectVisibleUnassignedGroups();
      if (remainingGroups.empty() && pendingInstalls.empty())
        break;
    }

    PageBuildState pageState;
    const uint32_t pageIndex = static_cast<uint32_t>(pages.size());
    const bool buildingRootPage = (pageIndex == 0u);
    uint32_t pageGroupLimit = maxGroupsPerPage;
    if (buildingRootPage && settings.maxRootPageGroups != 0u)
      pageGroupLimit = std::min(std::max(1u, settings.maxRootPageGroups), maxGroupsPerPage);

    while (pageState.groups.size() < pageGroupLimit &&
           pageState.clusterCount < maxClustersPerPage)
    {
      std::vector<uint32_t> pendingGroups = collectVisibleUnassignedGroups();
      if (pendingGroups.empty())
      {
        if (cutCount >= clusterCount)
          break;

        ++iteration;
        const uint32_t added = expandCut(cut, reverseClusterDAG);
        assert(added > 0u && "cluster DAG must be acyclic and reachable from roots");
        cutCount += added;

        leafs = leafsOfCut(cut, clusterDAG);
        printClusterCutState(iteration, cut, leafs);

        pendingGroups = collectVisibleUnassignedGroups();
      }

      if (pendingGroups.empty())
        break;

      uint32_t groupsAdded = 0u;
      for (uint32_t pendingIndex = 0u;
           pendingIndex < static_cast<uint32_t>(pendingGroups.size()) &&
           pageState.groups.size() < pageGroupLimit;
           ++pendingIndex)
      {
        const uint32_t groupId = pendingGroups[pendingIndex];
        const uint32_t groupClusterCount =
            static_cast<uint32_t>(groupClusters[groupId].size());
        assert(groupClusterCount != 0u);
        assert(groupClusterCount <= 8u);
        assert(groupClusterCount <= maxClustersPerPage);
        // Where a previous page took a prefix of this group, only the
        // remainder is still to place.
        const uint32_t placed = groupPlacedClusters[groupId];
        assert(placed < groupClusterCount);
        const uint32_t remaining = groupClusterCount - placed;

        // How many of the remaining clusters this page can still hold.
        uint32_t fits = std::min(
            remaining, maxClustersPerPage - pageState.clusterCount);
        uint64_t spanBytes = 0u;
        if (settings.maxPageBytes != 0u)
        {
          uint32_t byBudget = 0u;
          uint64_t accumulated = 0u;
          while (byBudget < fits)
          {
            const uint64_t clusterBytes = estimateClusterBytes(
                data.clusters[groupClusters[groupId][placed + byBudget]]);
            // A page must hold at least one cluster, or a cluster larger than
            // the budget would never be placed and the cut would lose it.
            if (!pageState.groups.empty() &&
                pageState.estimatedBytes + accumulated + clusterBytes >
                    settings.maxPageBytes)
              break;
            accumulated += clusterBytes;
            ++byBudget;
          }
          // Without splitting the group is all-or-nothing, exactly as before:
          // a group that does not fit whole waits for the next page.
          if (!splitGroups && byBudget < fits)
            byBudget = 0u;
          fits = byBudget;
          spanBytes = accumulated;
        }
        else if (!splitGroups && fits < remaining)
        {
          fits = 0u;
        }
        if (fits == 0u)
          continue;
        if (!splitGroups)
          assert(fits == remaining);

        PageGroupSpan span;
        span.groupId = groupId;
        span.firstClusterInGroup = placed;
        span.clusterCount = fits;
        pageState.groups.push_back(span);
        pageState.clusterCount += fits;
        pageState.estimatedBytes += spanBytes;
        for (uint32_t offset = 0u; offset < fits; ++offset)
          clusterPage[groupClusters[groupId][placed + offset]] = pageIndex;
        groupPlacements[groupId].push_back(
            GroupPlacement{pageIndex, placed, fits});
        groupPlacedClusters[groupId] = placed + fits;
        if (groupPlacedClusters[groupId] == groupClusterCount)
          groupAssigned[groupId] = 1u;
        ++groupsAdded;
      }

      if (groupsAdded == 0u)
        break;
    }

    std::vector<uint32_t> pendingInstallVec(pendingInstalls.begin(), pendingInstalls.end());
    std::sort(pendingInstallVec.begin(), pendingInstallVec.end());
    std::vector<uint32_t> completedInstalls;
    completedInstalls.reserve(pendingInstallVec.size());
    for (uint32_t g : pendingInstallVec)
    {
      const auto bits = emitUpdate(pageState, g, pageIndex);
      const uint8_t fullMask = fullMaskForCount(static_cast<uint32_t>(groupClusters[g].size()));
      if (bits.second == fullMask && bits.first == 0u)
        completedInstalls.push_back(g);
    }
    for (uint32_t g : completedInstalls)
      pendingInstalls.erase(g);

    {
      // A split group contributes several spans to one page only in the
      // degenerate case; emit once per group either way.
      std::vector<uint32_t> pageGroups;
      pageGroups.reserve(pageState.groups.size());
      for (const PageGroupSpan &span : pageState.groups)
        pageGroups.push_back(span.groupId);
      std::sort(pageGroups.begin(), pageGroups.end());
      pageGroups.erase(
          std::unique(pageGroups.begin(), pageGroups.end()), pageGroups.end());
      for (uint32_t g : pageGroups)
      {
        const auto bits = emitUpdate(pageState, g, pageIndex);
        const uint8_t fullMask = fullMaskForCount(static_cast<uint32_t>(groupClusters[g].size()));
        if (bits.second != fullMask || bits.first != 0u)
          pendingInstalls.insert(g);
      }
    }

    if (pageState.groups.empty() && pendingInstallVec.empty())
      break;

    if (!pages.empty())
      pageState.dependencies.insert(static_cast<uint32_t>(pages.size() - 1u));

    pages.push_back(std::move(pageState));
  }

  assert(pendingInstalls.empty() && "All pending installs must converge at full cut");

  // Cross-page dependencies from cluster DAG edges.
  for (uint32_t parent = 0; parent < clusterCount; ++parent)
  {
    const uint32_t parentPage = clusterPage[parent];
    for (uint32_t child : clusterDAG[parent])
    {
      const uint32_t childPage = clusterPage[child];
      if (childPage != parentPage)
        pages[childPage].dependencies.insert(parentPage);
    }
  }

  // Updates are materialised in the export pass below, once the hierarchy
  // split has given every span a node.

  // ---------------------------------------------------------------------------
  // Reorder clusters to page layout and patch hierarchy leaf node child_start.
  // ---------------------------------------------------------------------------
  std::vector<std::vector<uint32_t>> clustersByPage(pages.size());
  for (uint32_t pageIdx = 0; pageIdx < pages.size(); ++pageIdx)
  {
    for (const PageGroupSpan &span : pages[pageIdx].groups)
      for (uint32_t offset = 0u; offset < span.clusterCount; ++offset)
        clustersByPage[pageIdx].push_back(
            groupClusters[span.groupId][span.firstClusterInGroup + offset]);
  }

  std::vector<uint32_t> oldToNew(clusterCount, UINT32_MAX);
  std::vector<VirtualGeometryCluster> reorderedClusters;
  reorderedClusters.reserve(clusterCount);

  std::vector<uint32_t> pageClusterOffsets(pages.size(), 0u);
  std::vector<uint32_t> clusterToPage(clusterCount, UINT32_MAX);

  for (uint32_t pageIdx = 0; pageIdx < pages.size(); ++pageIdx)
  {
    pageClusterOffsets[pageIdx] = static_cast<uint32_t>(reorderedClusters.size());
    for (uint32_t oldIdx : clustersByPage[pageIdx])
    {
      assert(oldIdx < clusterCount);
      assert(oldToNew[oldIdx] == UINT32_MAX);

      const uint32_t newIdx = static_cast<uint32_t>(reorderedClusters.size());
      oldToNew[oldIdx] = newIdx;
      clusterToPage[newIdx] = pageIdx;
      reorderedClusters.push_back(clusters[oldIdx]);
    }
  }
  assert(reorderedClusters.size() == clusterCount);

  // Patch every leaf to its page, and split the leaves whose group straddles
  // one.
  //
  // A leaf addresses exactly one page and one contiguous run of that page's
  // clusters - `node.pageIndex` plus a page-local `child_start`, and the
  // culler derives each cluster's mask bit as `local - child_start`. A group
  // spanning pages cannot be expressed that way, so it becomes one leaf per
  // page: the leaves are appended at the end of the hierarchy (appending never
  // shifts an existing index, which an insert would) and the group's original
  // node becomes a synthetic internal node whose children are that contiguous
  // appended run. Its bounds and errors already describe the whole group, so
  // traversal reaches the halves through exactly the test the leaf used to
  // make.
  //
  // `spanNodes` records, per group, the node that owns each placed span, in
  // the same order as `groupPlacements` - the export pass resolves its
  // deferred updates through it.
  std::vector<std::vector<uint32_t>> spanNodes(groupCount);
  {
    const uint32_t originalNodeCount = static_cast<uint32_t>(hierarchy.size());
    for (uint32_t nodeIndex = 0u; nodeIndex < originalNodeCount; ++nodeIndex)
    {
      VirtualGeometryHierarchy &node = hierarchy[nodeIndex];
      if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
        continue;

      assert(node.child_start < clusterCount);
      node.flags &= ~(HIERARCHY_STREAMING_MASK_BITS | HIERARCHY_ENABLED_MASK_BITS | STREAMING_LEAF_FLAG);

      // The leaf's clusters are contiguous in the pre-page ordering; after the
      // reorder they are contiguous per page and the pages are in order, so
      // the runs below are exactly the group's spans.
      const uint32_t firstOld = node.child_start;
      const uint32_t count = node.child_count;
      const uint32_t groupId =
          count != 0u ? clusters[firstOld].groupId : UINT32_MAX;

      struct Run
      {
        uint32_t page = UINT32_MAX;
        uint32_t start = 0u;
        uint32_t count = 0u;
      };
      std::vector<Run> runs;
      for (uint32_t offset = 0u; offset < count; ++offset)
      {
        const uint32_t newIndex = oldToNew[firstOld + offset];
        const uint32_t page = clusterToPage[newIndex];
        if (!runs.empty() && runs.back().page == page)
        {
          ++runs.back().count;
          continue;
        }
        runs.push_back(Run{page, newIndex, 1u});
      }

      if (runs.size() <= 1u)
      {
        const uint32_t newGlobalStart = oldToNew[firstOld];
        const uint32_t pageIdx = clusterToPage[newGlobalStart];
        node.child_start = newGlobalStart - pageClusterOffsets[pageIdx];
        node.pageIndex = PAGE_NOT_INSTALLED_BIT | pageIdx;
        if (groupId < groupCount)
          spanNodes[groupId].assign(1u, nodeIndex);
        continue;
      }

      assert(splitGroups && "a group may only straddle pages when splitting is enabled");
      const uint32_t firstChild = static_cast<uint32_t>(hierarchy.size());
      const VirtualGeometryHierarchy leafTemplate = node;
      for (const Run &run : runs)
      {
        VirtualGeometryHierarchy child = leafTemplate;
        child.child_start = run.start - pageClusterOffsets[run.page];
        child.child_count = run.count;
        child.pageIndex = PAGE_NOT_INSTALLED_BIT | run.page;
        child.flags |= HIERARCHY_LEAF_FLAG;
        if (groupId < groupCount)
          spanNodes[groupId].push_back(static_cast<uint32_t>(hierarchy.size()));
        hierarchy.push_back(child);
      }
      // `node` is a dangling reference once `hierarchy` reallocates.
      VirtualGeometryHierarchy &parentNode = hierarchy[nodeIndex];
      parentNode.flags &= ~HIERARCHY_LEAF_FLAG;
      parentNode.child_start = firstChild;
      parentNode.child_count = static_cast<uint32_t>(runs.size());
      parentNode.pageIndex = UINT32_MAX;
    }
  }

  for (ClusterGroupInfo &info : groupInfos)
  {
    for (int32_t &idx : info.originalClusterIndices)
      idx = static_cast<int32_t>(oldToNew[static_cast<uint32_t>(idx)]);
    for (int32_t &idx : info.simplifiedClusterIndices)
      idx = static_cast<int32_t>(oldToNew[static_cast<uint32_t>(idx)]);
  }

  // Remap cluster DAG to reordered cluster indices.
  std::vector<std::vector<uint32_t>> remappedClusterDAG(clusterCount);
  for (uint32_t oldParent = 0; oldParent < clusterCount; ++oldParent)
  {
    const uint32_t newParent = oldToNew[oldParent];
    auto &dst = remappedClusterDAG[newParent];
    dst.reserve(clusterDAG[oldParent].size());
    for (uint32_t oldChild : clusterDAG[oldParent])
      dst.push_back(oldToNew[oldChild]);
    std::sort(dst.begin(), dst.end());
    dst.erase(std::unique(dst.begin(), dst.end()), dst.end());
  }

  data.clusters = std::move(reorderedClusters);
  data.clusterDAG = std::move(remappedClusterDAG);

  // buildToLinearizedClusterIndex maps a build cluster to its linearized index.
  // The page pass above reorders the linearized clusters, so the two maps have
  // to be composed here: without this the field still points at the pre-page
  // ordering and every consumer that uses it to tie a meshlet back to its source
  // reads a different cluster than the one it built.
  for (uint32_t &linearized : data.buildToLinearizedClusterIndex)
    if (linearized < clusterCount)
      linearized = oldToNew[linearized];

  // ---------------------------------------------------------------------------
  // Export build pages and logs
  // ---------------------------------------------------------------------------
  data.pages.clear();
  data.pages.reserve(pages.size());

  size_t totalUpdates = 0u;
  for (uint32_t pageIdx = 0; pageIdx < pages.size(); ++pageIdx)
  {
    VirtualGeometryBuildPage page;
    page.clusterOffset = pageClusterOffsets[pageIdx];
    page.clusterCount = static_cast<uint32_t>(clustersByPage[pageIdx].size());
    assert(page.clusterCount == pages[pageIdx].clusterCount);
    assert(page.clusterCount <= maxClustersPerPage && "Pages must not exceed the fixed decoded-cluster capacity");
    // Resolve the deferred rewrites: each names a group and one of its spans,
    // which the split above gave a node. The bitsets were computed over the
    // whole group, so they are shifted down to the span's own clusters - bit j
    // of a span's mask is its j-th cluster, which is what the culler reads.
    for (const PendingHierarchyUpdate &pendingUpdate : pages[pageIdx].updates)
    {
      const auto &nodes = spanNodes[pendingUpdate.groupId];
      const auto &placements = groupPlacements[pendingUpdate.groupId];
      assert(nodes.size() == placements.size());
      for (size_t spanIndex = 0u; spanIndex < placements.size(); ++spanIndex)
      {
        if (placements[spanIndex].firstClusterInGroup != pendingUpdate.firstClusterInGroup ||
            placements[spanIndex].clusterCount != pendingUpdate.clusterCount)
          continue;
        const uint32_t shift = pendingUpdate.firstClusterInGroup;
        const uint8_t spanMask = fullMaskForCount(pendingUpdate.clusterCount);
        HierarchyClusterFlagsUpdate resolved;
        resolved.hierarchyNodeIndex = nodes[spanIndex];
        resolved.streamingLeafsBitset = static_cast<uint8_t>(
            (pendingUpdate.streamingLeafsBitset >> shift) & spanMask);
        resolved.enabledClustersBitset = static_cast<uint8_t>(
            (pendingUpdate.enabledClustersBitset >> shift) & spanMask);
        page.installUpdates.hierarchyUpdates.push_back(resolved);
        break;
      }
    }
    dedupeAndSortUpdates(page.installUpdates);
    page.uninstallUpdates = pages[pageIdx].uninstallUpdates;
    assert(pages[pageIdx].groups.size() <= MAX_GROUPS_PER_PAGE && "Pages must not exceed the fixed page header group capacity");

    uint32_t localClusterOffset = 0u;
    page.groups.reserve(pages[pageIdx].groups.size());
    for (uint32_t localGroupIndex = 0u; localGroupIndex < pages[pageIdx].groups.size(); ++localGroupIndex)
    {
      const PageGroupSpan &pageSpan = pages[pageIdx].groups[localGroupIndex];
      VirtualGeometryBuildPage::GroupSpan span;
      span.globalGroupId = pageSpan.groupId;
      span.localGroupIndex = localGroupIndex;
      span.clusterOffset = localClusterOffset;
      span.clusterCount = pageSpan.clusterCount;
      page.groups.push_back(span);
      localClusterOffset += span.clusterCount;
    }

    page.dependencies.assign(pages[pageIdx].dependencies.begin(), pages[pageIdx].dependencies.end());
    std::sort(page.dependencies.begin(), page.dependencies.end());

    totalUpdates += page.installUpdates.hierarchyUpdates.size();
    data.pages.push_back(std::move(page));
  }

  VG_BUILD_LOG("  Generated %zu page(s), %zu rewrite update(s)\n", data.pages.size(), totalUpdates);
  for (uint32_t pageIdx = 0; pageIdx < data.pages.size(); ++pageIdx)
  {
    const auto &p = data.pages[pageIdx];
    VG_BUILD_LOG("    page %u: clusters=[%u..%u) deps=%zu updates=%zu\n", pageIdx, p.clusterOffset, p.clusterOffset + p.clusterCount, p.dependencies.size(), p.installUpdates.hierarchyUpdates.size());

    for (const auto &u : p.installUpdates.hierarchyUpdates)
    {
      VG_BUILD_LOG(
          "      node=%u streamingMask=0x%02X enabledMask=0x%02X selfError=%f, parentError=%f\n",
          u.hierarchyNodeIndex,
          u.streamingLeafsBitset,
          u.enabledClustersBitset,
          hierarchy[u.hierarchyNodeIndex].min_lod_error,
          hierarchy[u.hierarchyNodeIndex].max_parent_lod_error);
    }
  }

  for (auto &shape : data.shapes)
    shape.root_page_index = findShapeRootPage(hierarchy, shape.root_node_index, data.pages);
}

} // namespace virtualgeometry
