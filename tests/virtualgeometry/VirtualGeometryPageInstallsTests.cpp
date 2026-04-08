#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "./utils/File.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "editor/virtualgeometry/VirtualGeometryHierarchyRewrites.hpp"

using namespace virtualgeometry;

static constexpr uint32_t PAGE_NOT_INSTALLED_BIT = (1u << 31);

namespace
{

static uint8_t nodeEnabledMask(const VirtualGeometryHierarchy &node)
{
  return static_cast<uint8_t>((node.flags & HIERARCHY_ENABLED_MASK_BITS) >> HIERARCHY_ENABLED_MASK_SHIFT);
}

static uint8_t nodeStreamingMask(const VirtualGeometryHierarchy &node)
{
  return static_cast<uint8_t>((node.flags & HIERARCHY_STREAMING_MASK_BITS) >> HIERARCHY_STREAMING_MASK_SHIFT);
}

static uint8_t fullMaskForCount(uint32_t count)
{
  if (count == 0u)
    return 0u;
  if (count >= 8u)
    return 0xFFu;
  return static_cast<uint8_t>((1u << count) - 1u);
}

struct ValidationMaps
{
  std::vector<std::vector<uint32_t>> clusterDAG;
  std::vector<std::vector<uint32_t>> reverseClusterDAG;
  std::vector<uint32_t> clusterToHierarchy;
  std::vector<uint8_t> clusterBitIndex;
};

static ValidationMaps buildValidationMaps(const VirtualGeometryBuildData &build)
{
  ValidationMaps vm;
  const uint32_t clusterCount = static_cast<uint32_t>(build.clusters.size());
  vm.clusterDAG = build.clusterDAG;
  vm.reverseClusterDAG.assign(clusterCount, {});
  vm.clusterToHierarchy.assign(clusterCount, UINT32_MAX);
  vm.clusterBitIndex.assign(clusterCount, 0u);

  for (uint32_t p = 0; p < vm.clusterDAG.size(); ++p)
    for (uint32_t c : vm.clusterDAG[p])
    {
      assert(c < clusterCount);
      vm.reverseClusterDAG[c].push_back(p);
    }

  const auto &hier = build.lodLevelHierarchy;
  for (uint32_t ni = 0; ni < hier.size(); ++ni)
  {
    const auto &node = hier[ni];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
      continue;

    const uint32_t page = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    assert(page < build.pages.size());

    for (uint32_t local = 0; local < node.child_count; ++local)
    {
      const uint32_t globalCluster = build.pages[page].clusterOffset + node.child_start + local;
      assert(globalCluster < clusterCount);
      vm.clusterToHierarchy[globalCluster] = ni;
      vm.clusterBitIndex[globalCluster] = static_cast<uint8_t>(local);
    }
  }

  for (uint32_t c = 0; c < clusterCount; ++c)
    assert(vm.clusterToHierarchy[c] != UINT32_MAX);

  return vm;
}

struct SimState
{
  const VirtualGeometryBuildData &build;
  const ValidationMaps &vm;
  std::vector<bool> installedPages;
  std::vector<VirtualGeometryHierarchy> hierarchy;

  SimState(const VirtualGeometryBuildData &b, const ValidationMaps &v)
      : build(b), vm(v), installedPages(b.pages.size(), false), hierarchy(b.lodLevelHierarchy)
  {
    for (auto &node : hierarchy)
      if ((node.flags & HIERARCHY_LEAF_FLAG) != 0u && node.pageIndex != UINT32_MAX)
        node.pageIndex |= PAGE_NOT_INSTALLED_BIT;
  }

  bool clusterEnabled(uint32_t clusterId) const
  {
    const uint32_t ni = vm.clusterToHierarchy[clusterId];
    const auto &node = hierarchy[ni];
    if ((node.pageIndex & PAGE_NOT_INSTALLED_BIT) != 0u)
      return false;
    const uint8_t bit = vm.clusterBitIndex[clusterId];
    const uint8_t mask = nodeEnabledMask(node);
    return ((mask >> bit) & 1u) != 0u;
  }

  bool clusterStreaming(uint32_t clusterId) const
  {
    const uint32_t ni = vm.clusterToHierarchy[clusterId];
    const auto &node = hierarchy[ni];
    if ((node.pageIndex & PAGE_NOT_INSTALLED_BIT) != 0u)
      return false;
    const uint8_t bit = vm.clusterBitIndex[clusterId];
    const uint8_t mask = nodeStreamingMask(node);
    return ((mask >> bit) & 1u) != 0u;
  }

  void setNodeMasks(VirtualGeometryHierarchy &node, uint8_t streamingMask, uint8_t enabledMask)
  {
    node.flags &= ~(HIERARCHY_STREAMING_MASK_BITS | HIERARCHY_ENABLED_MASK_BITS | STREAMING_LEAF_FLAG);
    node.flags |= static_cast<uint32_t>(streamingMask) << HIERARCHY_STREAMING_MASK_SHIFT;
    node.flags |= static_cast<uint32_t>(enabledMask) << HIERARCHY_ENABLED_MASK_SHIFT;
    if (streamingMask != 0u)
      node.flags |= STREAMING_LEAF_FLAG;
  }

  void rebuild()
  {
    // Reset leaf install bits + masks.
    for (auto &node : hierarchy)
    {
      if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
        continue;
      const uint32_t page = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
      const bool installed = page < installedPages.size() ? installedPages[page] : false;
      node.pageIndex = installed ? page : (PAGE_NOT_INSTALLED_BIT | page);
      setNodeMasks(node, 0u, 0u);
    }

    std::unordered_set<uint32_t> nodesWithUpdates;
    for (uint32_t page = 0; page < build.pages.size(); ++page)
    {
      for (const auto &u : build.pages[page].installUpdates.hierarchyUpdates)
        nodesWithUpdates.insert(u.hierarchyNodeIndex);

      if (!installedPages[page])
        continue;

      for (const auto &u : build.pages[page].installUpdates.hierarchyUpdates)
      {
        assert(u.hierarchyNodeIndex < hierarchy.size());
        setNodeMasks(hierarchy[u.hierarchyNodeIndex], u.streamingLeafsBitset, u.enabledClustersBitset);
      }
    }

    // Installed nodes never touched by rewrites become fully enabled.
    for (uint32_t ni = 0; ni < hierarchy.size(); ++ni)
    {
      auto &node = hierarchy[ni];
      if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
        continue;
      if ((node.pageIndex & PAGE_NOT_INSTALLED_BIT) != 0u)
        continue;
      if (nodesWithUpdates.find(ni) != nodesWithUpdates.end())
        continue;
      setNodeMasks(node, 0u, fullMaskForCount(node.child_count));
    }
  }

  void installPage(uint32_t page)
  {
    installedPages[page] = true;
    rebuild();
  }

  void uninstallPage(uint32_t page)
  {
    installedPages[page] = false;
    rebuild();
  }
};

static bool verifyValidCut(const SimState &sim, const std::string &label)
{
  const uint32_t clusterCount = static_cast<uint32_t>(sim.build.clusters.size());
  bool valid = true;

  std::vector<uint8_t> enabled(clusterCount, 0u);
  uint32_t enabledCount = 0u;
  uint32_t streamingCount = 0u;
  for (uint32_t c = 0; c < clusterCount; ++c)
  {
    const bool e = sim.clusterEnabled(c);
    enabled[c] = e ? 1u : 0u;
    if (e)
      ++enabledCount;
    if (sim.clusterStreaming(c))
      ++streamingCount;
  }

  // Valid cut property: enabled set is closed under incoming dependencies.
  for (uint32_t c = 0; c < clusterCount; ++c)
  {
    if (enabled[c] == 0u)
      continue;
    for (uint32_t p : sim.vm.reverseClusterDAG[c])
    {
      if (enabled[p] == 0u)
      {
        std::printf("  CUT ERROR [%s]: cluster %u enabled but parent %u is disabled\n", label.c_str(), c, p);
        valid = false;
      }
    }
  }

  // Streaming mask must be subset of enabled mask.
  for (uint32_t c = 0; c < clusterCount; ++c)
  {
    if (sim.clusterStreaming(c) && enabled[c] == 0u)
    {
      std::printf("  CUT ERROR [%s]: cluster %u marked streaming but not enabled\n", label.c_str(), c);
      valid = false;
    }
  }

  std::printf("    [DAG %s] enabled=%u/%u streaming=%u\n", label.c_str(), enabledCount, clusterCount, streamingCount);
  return valid;
}

static std::vector<uint32_t> topologicalInstallOrder(const std::vector<VirtualGeometryBuildPage> &pages)
{
  const uint32_t pageCount = static_cast<uint32_t>(pages.size());
  std::vector<std::vector<uint32_t>> children(pageCount);
  std::vector<uint32_t> inDegree(pageCount, 0u);

  for (uint32_t p = 0; p < pageCount; ++p)
  {
    inDegree[p] = static_cast<uint32_t>(pages[p].dependencies.size());
    for (uint32_t dep : pages[p].dependencies)
    {
      assert(dep < pageCount);
      children[dep].push_back(p);
    }
  }

  std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
  for (uint32_t p = 0; p < pageCount; ++p)
    if (inDegree[p] == 0u)
      ready.push(p);

  std::vector<uint32_t> order;
  order.reserve(pageCount);
  while (!ready.empty())
  {
    const uint32_t p = ready.top();
    ready.pop();
    order.push_back(p);
    for (uint32_t ch : children[p])
    {
      assert(inDegree[ch] > 0u);
      --inDegree[ch];
      if (inDegree[ch] == 0u)
        ready.push(ch);
    }
  }

  assert(order.size() == pageCount && "Page dependency graph must be acyclic");
  return order;
}

static void checkAllClustersAndHierarchyInstalled(const SimState &sim)
{
  for (uint32_t ni = 0; ni < sim.hierarchy.size(); ++ni)
  {
    const auto &node = sim.hierarchy[ni];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
      continue;
    assert((node.pageIndex & PAGE_NOT_INSTALLED_BIT) == 0u && "Leaf hierarchy node still marked as not installed");
  }

  for (uint32_t c = 0; c < sim.build.clusters.size(); ++c)
    assert(sim.clusterEnabled(c) && "All clusters must be enabled after full install");
}

static void runPageInstallTest(const VirtualGeometryBuildData &build, const std::string &meshName)
{
  std::printf("\n==================================================\n");
  std::printf("VirtualGeometryPageInstallsTests: %s\n", meshName.c_str());
  std::printf("==================================================\n");
  std::printf("Pages=%zu Clusters=%zu HierarchyNodes=%zu\n", build.pages.size(), build.clusters.size(), build.lodLevelHierarchy.size());

  std::printf("\n-- Rewrites Per Page --\n");
  for (uint32_t p = 0; p < build.pages.size(); ++p)
  {
    const auto &page = build.pages[p];
    std::printf(
        "  Page %u: clusters=[%u..%u) deps=%zu updates=%zu\n",
        p,
        page.clusterOffset,
        page.clusterOffset + page.clusterCount,
        page.dependencies.size(),
        page.installUpdates.hierarchyUpdates.size());
    for (const auto &u : page.installUpdates.hierarchyUpdates)
      std::printf("    node=%u streamingMask=0x%02X enabledMask=0x%02X\n", u.hierarchyNodeIndex, u.streamingLeafsBitset, u.enabledClustersBitset);
  }

  const ValidationMaps vm = buildValidationMaps(build);
  SimState sim(build, vm);
  const std::vector<uint32_t> installOrder = topologicalInstallOrder(build.pages);

  std::printf("\n-- Install Phase --\n");
  for (uint32_t p : installOrder)
  {
    std::printf("  install page %u\n", p);
    sim.installPage(p);
    const std::string ctx = "after install page " + std::to_string(p);
    assert(verifyValidCut(sim, ctx));
  }

  std::printf("\n-- Full Install Verification --\n");
  checkAllClustersAndHierarchyInstalled(sim);
  std::printf("  all hierarchy leaves and clusters are installed\n");

  std::printf("\n-- Uninstall Phase --\n");
  for (int i = static_cast<int>(installOrder.size()) - 1; i >= 0; --i)
  {
    const uint32_t p = installOrder[static_cast<size_t>(i)];
    std::printf("  uninstall page %u\n", p);
    sim.uninstallPage(p);
    const std::string ctx = "after uninstall page " + std::to_string(p);
    assert(verifyValidCut(sim, ctx));
  }
}

static VirtualGeometryBuildData buildSyntheticMultiRootPagingCase()
{
  VirtualGeometryBuildData build;
  build.clusters.resize(6u);
  build.groupInfos.resize(6u);
  build.clusterDAG = {
      {1u},
      {2u},
      {},
      {4u},
      {5u},
      {},
  };

  for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
  {
    build.clusters[clusterIndex].groupId = clusterIndex;
    build.groupInfos[clusterIndex].lodLevel = clusterIndex < 3u ? clusterIndex : (clusterIndex - 3u);
    build.groupInfos[clusterIndex].originalClusterIndices = {static_cast<int32_t>(clusterIndex)};

    VirtualGeometryHierarchy leaf{};
    leaf.child_start = clusterIndex;
    leaf.child_count = 1u;
    leaf.pageIndex = UINT32_MAX;
    leaf.meshPartIndex = UINT32_MAX;
    leaf.flags = HIERARCHY_LEAF_FLAG;
    leaf.min_lod_error = static_cast<float>(clusterIndex);
    leaf.max_parent_lod_error = static_cast<float>(clusterIndex + 1u);
    build.lodLevelHierarchy.push_back(leaf);
  }

  VirtualGeometryBuildSettings settings{};
  settings.maxGroupsPerPage = 4u;
  settings.maxRootPageGroups = 4u;
  VirtualGeometryHierarchyRewrites::buildPagesAndRewrites(build, settings);
  return build;
}

static VirtualGeometryBuildData buildSyntheticRootCutOverflowCase()
{
  VirtualGeometryBuildData build;
  build.clusters.resize(6u);
  build.groupInfos.resize(6u);
  build.clusterDAG = {
      {1u},
      {},
      {3u},
      {},
      {5u},
      {},
  };

  for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
  {
    build.clusters[clusterIndex].groupId = clusterIndex;
    build.groupInfos[clusterIndex].lodLevel = clusterIndex / 2u;
    build.groupInfos[clusterIndex].originalClusterIndices = {static_cast<int32_t>(clusterIndex)};

    VirtualGeometryHierarchy leaf{};
    leaf.child_start = clusterIndex;
    leaf.child_count = 1u;
    leaf.pageIndex = UINT32_MAX;
    leaf.meshPartIndex = UINT32_MAX;
    leaf.flags = HIERARCHY_LEAF_FLAG;
    leaf.min_lod_error = static_cast<float>(clusterIndex);
    leaf.max_parent_lod_error = static_cast<float>(clusterIndex + 1u);
    build.lodLevelHierarchy.push_back(leaf);
  }

  VirtualGeometryBuildSettings settings{};
  settings.maxGroupsPerPage = 2u;
  settings.maxRootPageGroups = 0u;
  VirtualGeometryHierarchyRewrites::buildPagesAndRewrites(build, settings);
  return build;
}

static std::string buildTwoLayerGridOBJ(uint32_t quadsPerSide)
{
  assert(quadsPerSide > 0u);

  std::ostringstream obj;
  const uint32_t vertsPerSide = quadsPerSide + 1u;
  const uint32_t verticesPerLayer = vertsPerSide * vertsPerSide;

  auto appendLayer = [&](float zOffset)
  {
    for (uint32_t y = 0u; y < vertsPerSide; ++y)
    {
      for (uint32_t x = 0u; x < vertsPerSide; ++x)
      {
        obj << "v " << static_cast<float>(x) << ' ' << static_cast<float>(y) << ' ' << zOffset << '\n';
      }
    }

    for (uint32_t y = 0u; y < vertsPerSide; ++y)
    {
      for (uint32_t x = 0u; x < vertsPerSide; ++x)
      {
        obj << "vt " << (static_cast<float>(x) / static_cast<float>(quadsPerSide)) << ' ' << (static_cast<float>(y) / static_cast<float>(quadsPerSide)) << '\n';
      }
    }
  };

  auto appendFaces = [&](const char *objectName, uint32_t layerIndex)
  {
    const uint32_t baseVertex = layerIndex * verticesPerLayer;
    const uint32_t baseTexcoord = layerIndex * verticesPerLayer;

    obj << "o " << objectName << '\n';
    for (uint32_t y = 0u; y < quadsPerSide; ++y)
    {
      for (uint32_t x = 0u; x < quadsPerSide; ++x)
      {
        const uint32_t i0 = baseVertex + y * vertsPerSide + x + 1u;
        const uint32_t i1 = i0 + 1u;
        const uint32_t i2 = i0 + vertsPerSide;
        const uint32_t i3 = i2 + 1u;

        const uint32_t t0 = baseTexcoord + y * vertsPerSide + x + 1u;
        const uint32_t t1 = t0 + 1u;
        const uint32_t t2 = t0 + vertsPerSide;
        const uint32_t t3 = t2 + 1u;

        obj << "f " << i0 << '/' << t0 << ' ' << i1 << '/' << t1 << ' ' << i3 << '/' << t3 << '\n';
        obj << "f " << i0 << '/' << t0 << ' ' << i3 << '/' << t3 << ' ' << i2 << '/' << t2 << '\n';
      }
    }
  };

  appendLayer(0.0f);
  appendLayer(1.0f);
  appendFaces("layer_a", 0u);
  appendFaces("layer_b", 1u);
  return obj.str();
}

static void testPagesStaySaturatedAcrossMultipleRoots()
{
  VirtualGeometryBuildData build = buildSyntheticMultiRootPagingCase();
  assert(build.pages.size() == 2u);
  assert(build.pages[0].groups.size() == 4u);
  assert(build.pages[1].groups.size() == 2u);

  std::unordered_set<uint32_t> firstPageGroups;
  for (const auto &group : build.pages[0].groups)
    firstPageGroups.insert(group.globalGroupId);

  assert(firstPageGroups.count(0u) == 1u);
  assert(firstPageGroups.count(1u) == 1u);
  assert(firstPageGroups.count(3u) == 1u);
  assert(firstPageGroups.count(4u) == 1u);

  runPageInstallTest(build, "synthetic_multi_root_page_saturation");
}

static void testRootPageRespectsMaxGroupsPerPage()
{
  VirtualGeometryBuildData build = buildSyntheticRootCutOverflowCase();
  assert(!build.pages.empty());
  for (const auto &page : build.pages)
    assert(page.groups.size() <= 2u);

  assert(build.pages[0].groups.size() == 2u);
  runPageInstallTest(build, "root_page_clamped_to_max_groups");
}

static void testBuilderSaturatesPagesAcrossMergedShapes()
{
  std::vector<Vertex> vertices;
  std::vector<Shape> shapes;
  VirtualGeometryEncoder::loadOBJ(buildTwoLayerGridOBJ(12u), false, vertices, shapes);
  assert(shapes.size() == 2u);

  VirtualGeometryBuildSettings settings{};
  settings.maxGroupsPerPage = 2u;
  settings.maxRootPageGroups = 2u;

  const VirtualGeometryBuildData separateA = VirtualGeometryBuilder::build(vertices, std::vector<Shape>{shapes[0]}, settings);
  const VirtualGeometryBuildData separateB = VirtualGeometryBuilder::build(vertices, std::vector<Shape>{shapes[1]}, settings);
  const VirtualGeometryBuildData combined = VirtualGeometryBuilder::build(vertices, shapes, settings);

  const size_t separatePageCount = separateA.pages.size() + separateB.pages.size();
  assert(separatePageCount > 0u);
  assert(combined.pages.size() < separatePageCount && "Merged paging should pack groups across shape boundaries");

  runPageInstallTest(combined, "merged_shapes_page_saturation");
}

} // namespace

int main()
{
  const std::vector<std::string> mesh_paths = {
      "assets/meshes/obj/suzanne.obj",
      "assets/meshes/obj/teapot.obj",
      "assets/meshes/obj/stanford-bunny.obj",
      "assets/meshes/obj/armadillo.obj",
  };

  std::cout << "Virtual Geometry Page Installs Tests\n";
  std::cout << "=====================================\n";

  testPagesStaySaturatedAcrossMultipleRoots();
  testRootPageRespectsMaxGroupsPerPage();
  testBuilderSaturatesPagesAcrossMergedShapes();

  bool anyTested = false;
  for (const auto &relative_path : mesh_paths)
  {
    const std::string full_path = utils::getExecutableDirectory() + "/" + relative_path;
    FILE *f = fopen(full_path.c_str(), "rb");
    if (!f)
    {
      std::printf("Skipping %s (file not found)\n", relative_path.c_str());
      continue;
    }
    fclose(f);
    anyTested = true;

    std::vector<Vertex> vertices;
    Shape shape;
    VirtualGeometryEncoder::loadOBJ(full_path, true, vertices, shape);
    VirtualGeometryBuildData build = VirtualGeometryBuilder::build(VirtualGeometryBuilder::buildLOD0Clusters(vertices, shape));
    runPageInstallTest(build, relative_path);
  }

  if (!anyTested)
  {
    std::cout << "No mesh files found — skipping all tests.\n";
    return 0;
  }

  std::cout << "\n=====================================\n";
  std::cout << "✓ ALL PAGE INSTALLS TESTS PASSED\n";
  std::cout << "=====================================\n";
  return 0;
}
