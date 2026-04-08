#include "VirtualGeometryHierarchyRewrites.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace virtualgeometry
{

namespace
{

static constexpr uint32_t PAGE_NOT_INSTALLED_BIT = (1u << 31);

struct PageBuildState
{
  std::vector<uint32_t> groups;
  std::unordered_set<uint32_t> dependencies;
  PageUpdateList installUpdates;
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

  std::printf("    Iteration %u: cut=%u clusters, borderLeafs=%u clusters\n", iteration, cutCount, leafCount);
}

static uint32_t findShapeRootPage(const std::vector<VirtualGeometryHierarchy> &hierarchy, uint32_t rootNodeIndex)
{
  if (rootNodeIndex >= hierarchy.size())
    return UINT32_MAX;

  uint32_t bestPageIndex = UINT32_MAX;
  float bestError = -std::numeric_limits<float>::infinity();

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

  return bestPageIndex;
}

} // namespace

void VirtualGeometryHierarchyRewrites::buildPagesAndRewrites(VirtualGeometryBuildData &data, const VirtualGeometryBuildSettings &settings)
{
  auto &hierarchy = data.lodLevelHierarchy;
  auto &clusters = data.clusters;
  auto &groupInfos = data.groupInfos;
  auto &clusterDAG = data.clusterDAG;
  const uint32_t maxGroupsPerPage = std::min(std::max(1u, settings.maxGroupsPerPage), MAX_GROUPS_PER_PAGE);

  const uint32_t clusterCount = static_cast<uint32_t>(clusters.size());
  const uint32_t groupCount = static_cast<uint32_t>(groupInfos.size());
  assert(clusterDAG.size() == clusterCount && "clusterDAG must contain one adjacency list per cluster");

  std::printf("Building pages and rewrites from cluster DAG...\n");
  std::printf("  clusters=%u groups=%u maxGroupsPerPage=%u maxRootPageGroups=%u\n", clusterCount, groupCount, maxGroupsPerPage, settings.maxRootPageGroups);

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
  std::printf("  mapped %zu group(s) to hierarchy leaf node(s)\n", groupToHierarchy.size());

  // ---------------------------------------------------------------------------
  // Page generation + rewrite generation (cut-driven)
  // ---------------------------------------------------------------------------
  std::vector<PageBuildState> pages;
  std::vector<uint8_t> cut(clusterCount, 0u);
  std::vector<uint8_t> leafs(clusterCount, 0u);
  std::vector<uint8_t> groupAssigned(groupCount, 0u);
  std::vector<uint32_t> groupToPage(groupCount, UINT32_MAX);
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

  auto emitUpdate = [&](PageBuildState &pageState, uint32_t groupId) -> std::pair<uint8_t, uint8_t>
  {
    const auto it = groupToHierarchy.find(groupId);
    if (it == groupToHierarchy.end())
      return {0u, 0u};

    HierarchyClusterFlagsUpdate u;
    u.hierarchyNodeIndex = it->second;
    u.streamingLeafsBitset = bitsetForGroup(groupClusters[groupId], leafs);
    u.enabledClustersBitset = bitsetForGroup(groupClusters[groupId], cut);
    pageState.installUpdates.hierarchyUpdates.push_back(u);
    return {u.streamingLeafsBitset, u.enabledClustersBitset};
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

    while (pageState.groups.size() < pageGroupLimit)
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

      const uint32_t remainingSlots = pageGroupLimit - static_cast<uint32_t>(pageState.groups.size());
      const uint32_t groupsToAdd = std::min<uint32_t>(remainingSlots, static_cast<uint32_t>(pendingGroups.size()));
      for (uint32_t pendingIndex = 0u; pendingIndex < groupsToAdd; ++pendingIndex)
      {
        const uint32_t groupId = pendingGroups[pendingIndex];
        pageState.groups.push_back(groupId);
        groupAssigned[groupId] = 1u;
        groupToPage[groupId] = pageIndex;
      }
    }

    std::vector<uint32_t> pendingInstallVec(pendingInstalls.begin(), pendingInstalls.end());
    std::sort(pendingInstallVec.begin(), pendingInstallVec.end());
    std::vector<uint32_t> completedInstalls;
    completedInstalls.reserve(pendingInstallVec.size());
    for (uint32_t g : pendingInstallVec)
    {
      const auto bits = emitUpdate(pageState, g);
      const uint8_t fullMask = fullMaskForCount(static_cast<uint32_t>(groupClusters[g].size()));
      if (bits.second == fullMask && bits.first == 0u)
        completedInstalls.push_back(g);
    }
    for (uint32_t g : completedInstalls)
      pendingInstalls.erase(g);

    for (uint32_t g : pageState.groups)
    {
      const auto bits = emitUpdate(pageState, g);
      const uint8_t fullMask = fullMaskForCount(static_cast<uint32_t>(groupClusters[g].size()));
      if (bits.second != fullMask || bits.first != 0u)
        pendingInstalls.insert(g);
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
    const uint32_t parentPage = groupToPage[clusterToGroup[parent]];
    for (uint32_t child : clusterDAG[parent])
    {
      const uint32_t childPage = groupToPage[clusterToGroup[child]];
      if (childPage != parentPage)
        pages[childPage].dependencies.insert(parentPage);
    }
  }

  for (PageBuildState &p : pages)
    dedupeAndSortUpdates(p.installUpdates);

  // ---------------------------------------------------------------------------
  // Reorder clusters to page layout and patch hierarchy leaf node child_start.
  // ---------------------------------------------------------------------------
  std::vector<std::vector<uint32_t>> clustersByPage(pages.size());
  for (uint32_t pageIdx = 0; pageIdx < pages.size(); ++pageIdx)
  {
    for (uint32_t g : pages[pageIdx].groups)
      for (uint32_t c : groupClusters[g])
        clustersByPage[pageIdx].push_back(c);
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

  for (VirtualGeometryHierarchy &node : hierarchy)
  {
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
      continue;

    assert(node.child_start < clusterCount);
    const uint32_t newGlobalStart = oldToNew[node.child_start];
    const uint32_t pageIdx = clusterToPage[newGlobalStart];
    node.child_start = newGlobalStart - pageClusterOffsets[pageIdx];
    node.pageIndex = PAGE_NOT_INSTALLED_BIT | pageIdx;
    node.flags &= ~(HIERARCHY_STREAMING_MASK_BITS | HIERARCHY_ENABLED_MASK_BITS | STREAMING_LEAF_FLAG);
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
    page.installUpdates = std::move(pages[pageIdx].installUpdates);
    page.uninstallUpdates = pages[pageIdx].uninstallUpdates;
    assert(pages[pageIdx].groups.size() <= MAX_GROUPS_PER_PAGE && "Pages must not exceed the fixed page header group capacity");

    uint32_t localClusterOffset = 0u;
    page.groups.reserve(pages[pageIdx].groups.size());
    for (uint32_t localGroupIndex = 0u; localGroupIndex < pages[pageIdx].groups.size(); ++localGroupIndex)
    {
      const uint32_t groupId = pages[pageIdx].groups[localGroupIndex];
      VirtualGeometryBuildPage::GroupSpan span;
      span.globalGroupId = groupId;
      span.localGroupIndex = localGroupIndex;
      span.clusterOffset = localClusterOffset;
      span.clusterCount = static_cast<uint32_t>(groupClusters[groupId].size());
      page.groups.push_back(span);
      localClusterOffset += span.clusterCount;
    }

    page.dependencies.assign(pages[pageIdx].dependencies.begin(), pages[pageIdx].dependencies.end());
    std::sort(page.dependencies.begin(), page.dependencies.end());

    totalUpdates += page.installUpdates.hierarchyUpdates.size();
    data.pages.push_back(std::move(page));
  }

  std::printf("  Generated %zu page(s), %zu rewrite update(s)\n", data.pages.size(), totalUpdates);
  for (uint32_t pageIdx = 0; pageIdx < data.pages.size(); ++pageIdx)
  {
    const auto &p = data.pages[pageIdx];
    std::printf("    page %u: clusters=[%u..%u) deps=%zu updates=%zu\n", pageIdx, p.clusterOffset, p.clusterOffset + p.clusterCount, p.dependencies.size(), p.installUpdates.hierarchyUpdates.size());

    for (const auto &u : p.installUpdates.hierarchyUpdates)
    {
      std::printf(
          "      node=%u streamingMask=0x%02X enabledMask=0x%02X selfError=%f, parentError=%f\n",
          u.hierarchyNodeIndex,
          u.streamingLeafsBitset,
          u.enabledClustersBitset,
          hierarchy[u.hierarchyNodeIndex].min_lod_error,
          hierarchy[u.hierarchyNodeIndex].max_parent_lod_error);
    }
  }

  for (auto &shape : data.shapes)
    shape.root_page_index = findShapeRootPage(hierarchy, shape.root_node_index);
}

} // namespace virtualgeometry
