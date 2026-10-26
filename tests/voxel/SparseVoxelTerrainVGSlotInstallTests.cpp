// Phase 2 of docs/terrain-vg-unified-hierarchy-plan.md: many CPU-meshed chunks
// in one runtime object, each owning a slice of the object's hierarchy.
//
// These are pure remap/layout checks - no GPU, no scene - because the arithmetic
// is where this goes wrong: a chunk's DAG is tens of nodes, every child run and
// page index has to shift into its slice, and a single off-by-one publishes a
// node that points into a neighbouring chunk's geometry.

#include "voxel/svraster/SparseVoxelTerrainVirtualGeometrySceneBridge.hpp"

#include <iostream>
#include <string>

namespace svr = voxel::svraster;
namespace vg = virtualgeometry;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";
    return;
  }
  std::cout << "  FAILED " << what << "\n";
  ++failures;
}

vg::AABB makeBounds()
{
  vg::AABB bounds{};
  bounds.minPoint[0] = -10.0f;
  bounds.minPoint[1] = -1.0f;
  bounds.minPoint[2] = -10.0f;
  bounds.maxPoint[0] = 10.0f;
  bounds.maxPoint[1] = 5.0f;
  bounds.maxPoint[2] = 10.0f;
  return bounds;
}

// A chunk DAG shaped like a real one: a root over two interior nodes, each with
// two leaves that name pages.
std::vector<vg::VirtualGeometryHierarchy> makeChunkHierarchy()
{
  std::vector<vg::VirtualGeometryHierarchy> nodes(7);
  nodes[0].child_start = 1u;
  nodes[0].child_count = 2u;
  nodes[0].pageIndex = UINT32_MAX;
  nodes[1].child_start = 3u;
  nodes[1].child_count = 2u;
  nodes[1].pageIndex = UINT32_MAX;
  nodes[2].child_start = 5u;
  nodes[2].child_count = 2u;
  nodes[2].pageIndex = UINT32_MAX;
  nodes[3].pageIndex = 0u;
  nodes[4].pageIndex = 1u;
  nodes[5].pageIndex = vg::PAGE_NOT_INSTALLED_BIT | 1u;
  nodes[6].pageIndex = UINT32_MAX;
  return nodes;
}

void testLayout()
{
  std::cout << "shared object layout\n";
  const svr::SparseVoxelTerrainVGSharedObjectLayout layout{4u, 8u};
  check(layout.valid(), "the layout is valid");
  check(layout.hierarchyNodeCount() == 1u + 4u + 4u * 7u, "node count covers root, stubs and slices");
  // Chunk roots are gathered in front so the shared root's children are one
  // contiguous run.
  check(layout.nodeIndex(0u, 0u) == 1u, "slot 0's chunk root follows the root");
  check(layout.nodeIndex(3u, 0u) == 4u, "slot 3's chunk root is the last stub");
  check(layout.nodeIndex(0u, 1u) == 5u, "slot 0's slice starts after the stubs");
  check(layout.nodeIndex(1u, 1u) == 5u + 7u, "slot 1's slice follows slot 0's");

  // No two (slot, node) pairs may collide, or one chunk overwrites another.
  bool collided = false;
  for (uint32_t slotA = 0u; slotA < layout.capacity && !collided; ++slotA)
    for (uint32_t nodeA = 0u; nodeA < layout.maxChunkNodes && !collided; ++nodeA)
      for (uint32_t slotB = 0u; slotB < layout.capacity && !collided; ++slotB)
        for (uint32_t nodeB = 0u; nodeB < layout.maxChunkNodes && !collided; ++nodeB)
        {
          if (slotA == slotB && nodeA == nodeB)
            continue;
          if (layout.nodeIndex(slotA, nodeA) == layout.nodeIndex(slotB, nodeB))
            collided = true;
        }
  check(!collided, "no two chunk nodes share an object node");

  std::vector<vg::VirtualGeometryHierarchy> hierarchy;
  std::string error;
  check(
      svr::buildSparseVoxelTerrainVGSharedObjectHierarchy(
          layout, makeBounds(), hierarchy, &error),
      "the shared hierarchy builds: " + error);
  check(hierarchy.size() == layout.hierarchyNodeCount(), "the hierarchy is fully sized");
  check(
      hierarchy[0].child_start == 1u && hierarchy[0].child_count == layout.capacity,
      "the root's children are the contiguous stub run");
  check(
      (hierarchy[0].flags & vg::HIERARCHY_FORCE_TRAVERSAL_FLAG) != 0u,
      "the root forces traversal");
  bool stubsInert = true;
  for (uint32_t slot = 0u; slot < layout.capacity; ++slot)
  {
    const auto &stub = hierarchy[layout.nodeIndex(slot, 0u)];
    stubsInert = stubsInert && stub.child_count == 0u && stub.pageIndex == UINT32_MAX;
  }
  check(stubsInert, "every empty slot is inert");
}

void testChunkRemap()
{
  std::cout << "chunk hierarchy remap\n";
  const svr::SparseVoxelTerrainVGSharedObjectLayout layout{4u, 8u};
  const auto chunk = makeChunkHierarchy();
  const std::vector<uint32_t> pageSlots{11u, 23u};

  std::vector<vg::VirtualGeometryHierarchy> chunkRoot;
  std::vector<vg::VirtualGeometryHierarchy> slice;
  std::string error;
  check(
      svr::remapSparseVoxelTerrainVGChunkHierarchy(
          layout, 2u, chunk, pageSlots, chunkRoot, slice, &error),
      "the chunk remaps into slot 2: " + error);
  check(chunkRoot.size() == 1u, "the chunk root is written on its own");
  check(slice.size() == chunk.size() - 1u, "the slice holds the remaining nodes");

  // Every child run must land inside slot 2's own nodes.
  check(
      chunkRoot[0].child_start == layout.nodeIndex(2u, 1u),
      "the root's children point at the slice");
  check(
      slice[0].child_start == layout.nodeIndex(2u, 3u),
      "an interior node's children shift by the slice base");
  const uint32_t sliceFirst = layout.nodeIndex(2u, 1u);
  const uint32_t sliceLast = layout.nodeIndex(2u, layout.maxChunkNodes - 1u);
  bool inRange = true;
  for (const auto &node : slice)
    if (node.child_count != 0u)
      inRange = inRange && node.child_start >= sliceFirst &&
          node.child_start + node.child_count - 1u <= sliceLast;
  check(inRange, "no child run escapes the slot's slice");

  // Page indices map through the slot's page slots, and the not-installed bit
  // survives - it is what tells the culler the page is parked, not missing.
  check(slice[2].pageIndex == 11u, "a leaf's page maps to the owning page slot");
  check(slice[3].pageIndex == 23u, "the second page maps too");
  check(
      slice[4].pageIndex == (vg::PAGE_NOT_INSTALLED_BIT | 23u),
      "the not-installed bit is preserved through the remap");
  check(slice[5].pageIndex == UINT32_MAX, "a node with no page keeps none");

  // Rejections.
  auto oversized = chunk;
  oversized.resize(layout.maxChunkNodes + 1u);
  check(
      !svr::remapSparseVoxelTerrainVGChunkHierarchy(
          layout, 0u, oversized, pageSlots, chunkRoot, slice, &error),
      "a chunk larger than its reservation is rejected");
  auto namesRoot = chunk;
  namesRoot[1].child_start = 0u;
  check(
      !svr::remapSparseVoxelTerrainVGChunkHierarchy(
          layout, 0u, namesRoot, pageSlots, chunkRoot, slice, &error),
      "a child run beginning at the chunk root is rejected");
  auto badPage = chunk;
  badPage[3].pageIndex = 7u;
  check(
      !svr::remapSparseVoxelTerrainVGChunkHierarchy(
          layout, 0u, badPage, pageSlots, chunkRoot, slice, &error),
      "a page the slot does not own is rejected");
  check(
      !svr::remapSparseVoxelTerrainVGChunkHierarchy(
          layout, layout.capacity, chunk, pageSlots, chunkRoot, slice, &error),
      "a slot beyond capacity is rejected");
}

void testRewriteRemap()
{
  std::cout << "rewrite remap\n";
  const svr::SparseVoxelTerrainVGSharedObjectLayout layout{4u, 8u};
  std::vector<vg::HierarchyClusterFlagsUpdate> updates(3);
  updates[0].hierarchyNodeIndex = 0u;
  updates[0].streamingLeafsBitset = 0x0fu;
  updates[1].hierarchyNodeIndex = 2u;
  updates[1].enabledClustersBitset = 0xf0u;
  updates[2].hierarchyNodeIndex = layout.maxChunkNodes;  // out of range

  const auto remapped =
      svr::remapSparseVoxelTerrainVGChunkRewrites(layout, 1u, updates);
  check(remapped.size() == 2u, "an out-of-range rewrite is dropped");
  check(
      remapped[0].hierarchyNodeIndex == layout.nodeIndex(1u, 0u),
      "a rewrite naming the chunk root lands on the slot's stub");
  check(
      remapped[1].hierarchyNodeIndex == layout.nodeIndex(1u, 2u),
      "an interior rewrite lands in the slice");
  check(
      remapped[0].streamingLeafsBitset == 0x0fu &&
          remapped[1].enabledClustersBitset == 0xf0u,
      "the cluster bitsets are carried through unchanged");
}

// Phase 3: a coarse chunk stops being the streaming boundary where its children
// have arrived.
void testResidentChildRewrites()
{
  std::cout << "resident child rewrites\n";
  const svr::SparseVoxelTerrainVGSharedObjectLayout layout{4u, 8u};

  // A chunk centred on the origin: node 0 is the root spanning everything,
  // nodes 1..4 sit one per quadrant, node 5 straddles the x axis.
  std::vector<vg::VirtualGeometryHierarchy> nodes(6);
  const auto box = [&](const size_t index, const float minX, const float maxX,
                       const float minZ, const float maxZ)
  {
    nodes[index].min_x = minX;
    nodes[index].max_x = maxX;
    nodes[index].min_z = minZ;
    nodes[index].max_z = maxZ;
  };
  box(0u, -10.0f, 10.0f, -10.0f, 10.0f);
  box(1u, -10.0f, -1.0f, -10.0f, -1.0f);  // -x -z  → quadrant 0
  box(2u, 1.0f, 10.0f, -10.0f, -1.0f);    // +x -z  → quadrant 1
  box(3u, -10.0f, -1.0f, 1.0f, 10.0f);    // -x +z  → quadrant 2
  box(4u, 1.0f, 10.0f, 1.0f, 10.0f);      // +x +z  → quadrant 3
  box(5u, -5.0f, 5.0f, -10.0f, -1.0f);    // straddles x, both -z quadrants

  const auto quadrants = svr::sparseVoxelTerrainVGChunkNodeQuadrants(nodes, 0.0f, 0.0f);
  check(quadrants[0] == 0x0fu, "the chunk root reaches every quadrant");
  check(quadrants[1] == 0x1u, "a -x -z node is quadrant 0");
  check(quadrants[2] == 0x2u, "a +x -z node is quadrant 1");
  check(quadrants[3] == 0x4u, "a -x +z node is quadrant 2");
  check(quadrants[4] == 0x8u, "a +x +z node is quadrant 3");
  check(quadrants[5] == 0x3u, "a node straddling x reaches both -z quadrants");

  std::vector<vg::HierarchyClusterFlagsUpdate> base(6);
  for (uint32_t index = 0u; index < base.size(); ++index)
  {
    base[index].hierarchyNodeIndex = index;
    base[index].streamingLeafsBitset = 0xffu;
    base[index].enabledClustersBitset = 0x0fu;
  }

  // Nothing resident: every node is still a streaming boundary.
  auto none = svr::sparseVoxelTerrainVGChunkRewritesForResidentChildren(
      layout, 1u, base, quadrants, 0u);
  check(none.size() == base.size(), "every base rewrite survives");
  bool allBoundaries = true;
  for (const auto &update : none)
    allBoundaries = allBoundaries && update.streamingLeafsBitset == 0xffu;
  check(allBoundaries, "with no child resident nothing stops being a boundary");
  check(
      none[0].hierarchyNodeIndex == layout.nodeIndex(1u, 0u),
      "rewrites are remapped into the slot");

  // Quadrant 0 only: its node clears, the straddling node does not - it also
  // reaches quadrant 1, whose geometry has not arrived.
  auto one = svr::sparseVoxelTerrainVGChunkRewritesForResidentChildren(
      layout, 1u, base, quadrants, 0x1u);
  check(one[1].streamingLeafsBitset == 0u, "the covered quadrant's node clears");
  check(one[2].streamingLeafsBitset == 0xffu, "an uncovered quadrant is untouched");
  check(
      one[5].streamingLeafsBitset == 0xffu,
      "a straddling node waits for every quadrant it reaches");
  check(
      one[0].streamingLeafsBitset == 0xffu,
      "the chunk root waits for all four children");
  check(
      one[1].enabledClustersBitset == 0x0fu,
      "clearing a boundary does not disable the parent's clusters");

  // Three of four: the straddling node clears once both its quadrants are in.
  auto three = svr::sparseVoxelTerrainVGChunkRewritesForResidentChildren(
      layout, 1u, base, quadrants, 0x1u | 0x2u | 0x4u);
  check(three[5].streamingLeafsBitset == 0u, "the straddling node clears with both halves");
  check(three[0].streamingLeafsBitset == 0xffu, "the root still waits for the fourth");

  // All four: everything clears, including the root.
  auto all = svr::sparseVoxelTerrainVGChunkRewritesForResidentChildren(
      layout, 1u, base, quadrants, 0x0fu);
  bool cleared = true;
  for (const auto &update : all)
    cleared = cleared && update.streamingLeafsBitset == 0u;
  check(cleared, "with every child resident no node is a boundary");

  // A child leaving must restore the boundary exactly, or the cut descends into
  // geometry that is no longer there.
  auto restored = svr::sparseVoxelTerrainVGChunkRewritesForResidentChildren(
      layout, 1u, base, quadrants, 0x1u);
  bool identical = restored.size() == one.size();
  for (size_t index = 0u; identical && index < restored.size(); ++index)
    identical = restored[index].hierarchyNodeIndex == one[index].hierarchyNodeIndex &&
        restored[index].streamingLeafsBitset == one[index].streamingLeafsBitset &&
        restored[index].enabledClustersBitset == one[index].enabledClustersBitset;
  check(identical, "residency is a pure function of the mask, so a child leaving restores exactly");

  const auto replacement = svr::sparseVoxelTerrainVGChunkRewritesForResidentChildren(
      layout, 1u, base, quadrants, 0x1u, true);
  check(replacement[1].enabledClustersBitset == 0u,
        "a drawable child hierarchy replaces its covered parent clusters");
  check(replacement[5].enabledClustersBitset == base[5].enabledClustersBitset,
        "partial replacement keeps a parent straddling an absent quadrant");
  const auto fallback = svr::sparseVoxelTerrainVGChunkRewritesForResidentChildren(
      layout, 1u, base, quadrants, 0u, true);
  check(fallback[1].enabledClustersBitset == base[1].enabledClustersBitset &&
            fallback[1].streamingLeafsBitset == base[1].streamingLeafsBitset,
        "losing the child restores the parent's original intra-chunk cut");

  svr::SparseVoxelTerrainVGChunkClusterQuadrants perCluster;
  perCluster.byNode.resize(base.size());
  perCluster.byNode[1][0].quadrants = 1u;
  perCluster.byNode[1][1].quadrants = 3u;
  std::array<svr::SparseVoxelTerrainVGChildCoverage, 4> children{};
  children[0].installed = true;
  const auto fineReplacement =
      svr::sparseVoxelTerrainVGChunkRewritesForResidentChildrenPerCluster(
          layout, 1u, base, perCluster, children, nullptr, true);
  check(fineReplacement[1].enabledClustersBitset == 0x0eu,
        "replacement is per cluster and preserves straddling or unknown coverage");
  children[0].installed = false;
  const auto fineFallback =
      svr::sparseVoxelTerrainVGChunkRewritesForResidentChildrenPerCluster(
          layout, 1u, base, perCluster, children, nullptr, true);
  check(fineFallback[1].enabledClustersBitset == 0x0fu,
        "eviction restores the per-cluster fallback");
}

} // namespace

int main()
{
  std::cout << "Terrain virtual geometry slot install tests\n";
  testLayout();
  testChunkRemap();
  testRewriteRemap();
  testResidentChildRewrites();
  if (failures != 0)
  {
    std::cout << failures << " slot install checks failed\n";
    return 1;
  }
  std::cout << "SparseVoxelTerrainVGSlotInstall: all checks passed\n";
  return 0;
}
