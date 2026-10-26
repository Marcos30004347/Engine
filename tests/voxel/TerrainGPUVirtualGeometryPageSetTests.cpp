// CPU contract of the GPU-generated terrain VG page set: stable slots for
// directory addresses, parent/child links by quadrant, bounds fixed at
// assignment, and resident child masks that follow installs and releases.
#include "voxel/svraster/TerrainGPUVirtualGeometryPageSet.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace
{
namespace svr = voxel::svraster;
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  if (!condition)
    ++failures;
}

svr::TerrainGPUVGDirectoryPage page(const int32_t x, const int32_t z, const uint32_t lod, const bool record = false, const double minY = 0.0, const double maxY = 0.0)
{
  svr::TerrainGPUVGDirectoryPage result{};
  result.address = {x, z, lod};
  result.hasRecord = record;
  result.recordMinimumSurfaceMetres = minY;
  result.recordMaximumSurfaceMetres = maxY;
  return result;
}

svr::TerrainGPUVirtualGeometryPageSet makeSet(const uint32_t capacity)
{
  svr::TerrainGPUVirtualGeometryPageSet set;
  svr::TerrainGPUVGPageSetSettings settings{};
  settings.capacity = capacity;
  settings.baseEdgeMetres = 6.4;
  settings.worldExtentMetres = 128000.0;
  settings.finestVoxelMetres = 0.1;
  settings.elevationOriginMetres = 100.0;
  std::string error;
  check(set.configure(settings, &error), "page set configures: " + error);
  return set;
}

void testAssignmentAndBounds()
{
  std::cout << "slots, parents, quadrants and bounds\n";
  auto set = makeSet(16u);
  // A root at LOD 2 with a record, and its four children without one.
  std::vector<svr::TerrainGPUVGDirectoryPage> pages{
    page(10, 7, 2u, true, 300.0, 420.0),
    page(20, 14, 1u), page(21, 14, 1u), page(20, 15, 1u), page(21, 15, 1u)};
  const auto sync = set.sync(pages);
  check(sync.assigned.size() == 5u && sync.released.empty() && sync.rejected == 0u, "five pages take five slots");
  check(set.liveCount() == 5u, "five live pages");
  const auto *root = set.find({10, 7, 2u});
  check(root != nullptr && root->rootPage && root->parentSlot == UINT32_MAX, "the LOD 2 page is a root (no parent in the set)");
  check(root != nullptr && root->boundsValid, "a page with a record has bounds");
  // X/Z from the address: minimum = x * edge - extent/2, edge = 6.4 * 4.
  // Single precision at 64 km resolves about 4 mm.
  check(root != nullptr && std::fabs(root->boundsMin[0] - (10.0 * 25.6 - 64000.0)) < 2e-2 && std::fabs(root->boundsMax[0] - root->boundsMin[0] - 25.6) < 2e-2,
        "root X bounds follow the address lattice");
  check(root != nullptr && std::fabs(root->boundsMin[1] - 200.0) < 1e-3 && std::fabs(root->boundsMax[1] - 320.0) < 1e-3,
        "root Y bounds are the record's surface range in renderer space");
  const auto *child = set.find({21, 15, 1u});
  check(child != nullptr && child->parentSlot == set.slotOf({10, 7, 2u}), "children link to the parent slot");
  check(child != nullptr && svr::TerrainGPUVirtualGeometryPageSet::quadrantOf(child->address) == 3u, "(+x,+z) child is quadrant 3");
  check(root != nullptr && root->childSlots[3] == set.slotOf({21, 15, 1u}) && root->childSlots[0] == set.slotOf({20, 14, 1u}),
        "the parent records its children by quadrant");
  check(child != nullptr && child->boundsValid && child->boundsMin[1] < root->boundsMin[1] && child->boundsMax[1] > root->boundsMax[1],
        "a child without a record takes the parent's Y range widened by one parent voxel");
  check(std::fabs(set.selfError(1u) - 0.2f) < 1e-6f && std::fabs(set.parentError(1u) - 0.4f) < 1e-6f, "errors are the level's voxel and its parent's");
  // Bounds stay fixed once assigned, even when a record appears later.
  pages[4] = page(21, 15, 1u, true, 0.0, 9000.0);
  const auto again = set.sync(pages);
  check(again.assigned.empty() && again.released.empty() && again.boundsChanged.empty(), "a second sync with the same addresses changes nothing");
  check(child->boundsMax[1] < 1000.0f, "bounds do not move when a record arrives later");
}

void testInstallMasksAndRelease()
{
  std::cout << "resident child masks and releases\n";
  auto set = makeSet(16u);
  std::vector<svr::TerrainGPUVGDirectoryPage> pages{
    page(0, 0, 1u, true, 10.0, 20.0), page(0, 0, 0u), page(1, 0, 0u), page(0, 1, 0u), page(1, 1, 0u)};
  set.sync(pages);
  const uint32_t parent = set.slotOf({0, 0, 1u});
  const uint32_t child1 = set.slotOf({1, 0, 0u});
  check(set.markInstalled(child1, 100u, 7u, 3u) == parent, "installing a child reports the parent");
  check(set.page(parent)->residentChildMask == 0x2u, "quadrant 1 becomes resident");
  check(set.markInstalled(child1, 100u, 7u, 3u) == UINT32_MAX, "re-installing reports no mask change");
  const uint32_t child3 = set.slotOf({1, 1, 0u});
  set.markInstalled(child3, 200u, 9u, 3u);
  check(set.page(parent)->residentChildMask == 0xau, "quadrants 1 and 3 are resident");
  check(set.page(child3)->clusterOffset == 200u && set.page(child3)->clusterCount == 9u, "an installed page remembers its cluster range");
  check(set.markUninstalled(child3) == parent && set.page(parent)->residentChildMask == 0x2u, "uninstalling clears the quadrant");
  // The cut coarsens: the children leave the directory.
  pages.resize(1u);
  const auto sync = set.sync(pages);
  check(sync.released.size() == 4u && set.liveCount() == 1u, "four children are released");
  check(sync.maskChanged.size() == 1u && sync.maskChanged[0] == parent && set.page(parent)->residentChildMask == 0u,
        "an installed child leaving clears the parent's mask");
  check(set.page(parent)->childSlots[1] == UINT32_MAX, "the parent forgets released children");
  // Slots are reused.
  pages.push_back(page(1, 0, 0u));
  const auto reuse = set.sync(pages);
  check(reuse.assigned.size() == 1u && set.liveCount() == 2u, "a returning child takes a slot again");
  check(set.page(set.slotOf({1, 0, 0u}))->parentSlot == parent && set.page(parent)->childSlots[1] == set.slotOf({1, 0, 0u}),
        "the returning child relinks to its parent");
}

void testCapacity()
{
  std::cout << "capacity\n";
  auto set = makeSet(2u);
  std::vector<svr::TerrainGPUVGDirectoryPage> pages{page(0, 0, 3u, true, 0.0, 1.0), page(0, 0, 2u), page(1, 0, 2u)};
  const auto sync = set.sync(pages);
  check(sync.assigned.size() == 2u && sync.rejected == 1u, "a full set rejects the overflow and keeps the rest");
  pages.erase(pages.begin() + 1);
  const auto next = set.sync(pages);
  check(next.released.size() == 1u && next.assigned.size() == 1u && set.liveCount() == 2u, "a released slot serves the next address");
}

// Phase 1 of docs/terrain-vg-unified-hierarchy-plan.md: a CPU-meshed chunk is a
// multi-page asset, so a chunk owns several runtime page slots.
void testPageSlotReservation()
{
  std::cout << "page slot reservation\n";
  auto set = makeSet(8u);
  const auto sync = set.sync({page(0, 0, 3u, true, 0.0, 1.0)});
  check(sync.assigned.size() == 1u, "the chunk takes a slot");
  const uint32_t primary = sync.assigned[0];

  std::string error;
  check(set.reservePageSlots(primary, 3u, &error), "three pages reserve: " + error);
  check(set.reservedPageCount(primary) == 3u, "the chunk owns three slots");
  check(set.liveCount() == 3u, "the extras are live");
  const uint32_t first = set.slotForAssetPage(primary, 0u);
  const uint32_t second = set.slotForAssetPage(primary, 1u);
  const uint32_t third = set.slotForAssetPage(primary, 2u);
  check(first == primary, "asset page zero is the primary slot");
  check(
      second != UINT32_MAX && third != UINT32_MAX && second != third &&
          second != primary && third != primary,
      "the extra slots are distinct");
  check(
      set.slotForAssetPage(primary, 3u) == UINT32_MAX,
      "a page beyond the reservation has no slot");

  check(set.reservePageSlots(primary, 2u, &error), "shrinking releases: " + error);
  check(set.reservedPageCount(primary) == 2u, "the chunk owns two slots");
  check(set.liveCount() == 2u, "the returned slot is free again");

  // Exhaustion must leave the reservation untouched, or a chunk installs a
  // partial DAG the culler cannot distinguish from a complete one.
  check(!set.reservePageSlots(primary, 32u, &error), "an oversized reservation fails");
  check(!error.empty(), "exhaustion explains itself");
  check(
      set.reservedPageCount(primary) == 2u && set.liveCount() == 2u,
      "a failed reservation changes nothing");

  // Releasing the chunk returns every slot it owned.
  check(set.reservePageSlots(primary, 4u, &error), "four pages reserve: " + error);
  check(set.liveCount() == 4u, "four slots live");
  const auto released = set.sync({});
  check(released.released.size() == 1u, "the chunk reports one released address");
  check(set.liveCount() == 0u, "every page slot the chunk owned came back");
  const auto reused = set.sync({page(0, 0, 3u, true, 0.0, 1.0)});
  check(reused.assigned.size() == 1u, "the pool serves the next chunk");
}

// Phase 4 of docs/terrain-vg-unified-hierarchy-plan.md: GPU demand becomes chunk
// builds.
void testDemandPlanning()
{
  std::cout << "demand planning\n";
  auto set = makeSet(16u);
  const auto sync = set.sync({
    page(0, 0, 3u, true, 0.0, 1.0),
    page(0, 0, 2u, true, 0.0, 1.0),
    page(1, 0, 2u, true, 0.0, 1.0)});
  check(sync.assigned.size() == 3u, "three chunks take slots");
  const uint32_t coarse = set.slotOf({0, 0, 3u});
  const uint32_t childA = set.slotOf({0, 0, 2u});
  const uint32_t childB = set.slotOf({1, 0, 2u});

  // Highest priority first, ties by slot, so a frame's plan is reproducible.
  auto plan = svr::planTerrainVGPageBuilds(
      set, {{childA, 10u}, {coarse, 90u}, {childB, 10u}}, {}, 8u);
  check(plan.size() == 3u, "every demanded chunk is planned");
  check(plan[0].slot == coarse, "the highest priority is planned first");
  check(
      plan[1].slot == std::min(childA, childB) &&
          plan[2].slot == std::max(childA, childB),
      "ties break by slot");
  check(plan[0].address.spatialLOD == 3u, "the request carries the ground address");

  // A chunk already being built must not be queued twice.
  auto deduped = svr::planTerrainVGPageBuilds(
      set, {{childA, 10u}, {coarse, 90u}}, {coarse}, 8u);
  check(deduped.size() == 1u && deduped[0].slot == childA, "an in-flight chunk is dropped");

  // An installed chunk is not demanded again.
  set.markInstalled(childA, 0u, 4u, 1u);
  auto installed = svr::planTerrainVGPageBuilds(
      set, {{childA, 50u}, {childB, 10u}}, {}, 8u);
  check(
      installed.size() == 1u && installed[0].slot == childB,
      "an installed chunk is dropped");

  // Demand naming one of a chunk's extra page slots is demand for the chunk:
  // a chunk is built and installed whole.
  std::string error;
  check(set.reservePageSlots(childB, 3u, &error), "child B reserves three pages: " + error);
  const uint32_t extra = set.slotForAssetPage(childB, 2u);
  check(extra != UINT32_MAX && extra != childB, "the extra slot exists");
  auto viaExtra = svr::planTerrainVGPageBuilds(set, {{extra, 5u}}, {}, 8u);
  check(
      viaExtra.size() == 1u && viaExtra[0].slot == childB,
      "demand for an extra page resolves to its chunk");
  // And several demands across one chunk's pages collapse to one build.
  auto collapsed = svr::planTerrainVGPageBuilds(
      set, {{childB, 5u}, {extra, 7u}, {set.slotForAssetPage(childB, 1u), 6u}}, {}, 8u);
  check(collapsed.size() == 1u, "several pages of one chunk are one build");

  // The budget is honoured, and a slot that holds nothing is ignored.
  auto budgeted = svr::planTerrainVGPageBuilds(
      set, {{childB, 5u}, {coarse, 9u}}, {}, 1u);
  check(budgeted.size() == 1u && budgeted[0].slot == coarse, "the budget caps the plan");
  auto empty = svr::planTerrainVGPageBuilds(set, {{15u, 9u}}, {}, 8u);
  check(empty.empty(), "demand for an unassigned slot is ignored");
  check(
      svr::planTerrainVGPageBuilds(set, {{coarse, 9u}}, {}, 0u).empty(),
      "a zero budget plans nothing");
}

} // namespace

int main()
{
  std::cout << "Terrain GPU virtual geometry page set tests\n";
  testAssignmentAndBounds();
  testInstallMasksAndRelease();
  testCapacity();
  testPageSlotReservation();
  testDemandPlanning();
  if (failures != 0)
  {
    std::cout << failures << " page set checks failed\n";
    return 1;
  }
  std::cout << "TerrainGPUVirtualGeometryPageSet: all checks passed\n";
  return 0;
}
