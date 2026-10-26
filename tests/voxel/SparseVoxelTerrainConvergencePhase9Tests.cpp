#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/TerrainGPUV2Publication.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

svr::SparseVoxelPageProducer producer()
{
  svr::SparseVoxelPageProducer result{};
  result.producerId = 0x504841534539ull;
  result.kind = svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain;
  result.producerRevision = 9u;
  result.producesOnGPU = true;
  result.debugName = "phase9-terrain";
  return result;
}

svr::SparseVoxelPhysicalPageAllocation requirements(const svr::SparseVoxelPageId page, const uint32_t nodes, const uint32_t children, const uint32_t tree64Nodes = 0u)
{
  svr::SparseVoxelPhysicalPageAllocation result{};
  result.page = page;
  result.hierarchyNodes.count = nodes;
  result.childIndices.count = children;
  result.brickNodes.count = nodes;
  result.brickOccupancy.count = nodes;
  result.materials.count = nodes;
  result.materialCells.count = nodes;
  result.tree64Nodes.count = tree64Nodes;
  result.tree64Materials.count = tree64Nodes;
  return result;
}

svr::SparseVoxelProducedPagePayload payload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t payloadRevision,
    const uint32_t nodes,
    const uint32_t children,
    const uint32_t tree64Nodes = 0u)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = payloadRevision;
  result.requirements = requirements(page, nodes, children, tree64Nodes);
  result.installRewriteCount = 1u;
  result.uninstallRewriteCount = 1u;
  result.payloadComplete = true;
  return result;
}

svr::SparseVoxelPageInstallRewrite installRewrite(const svr::SparseVoxelReservedPagePayload &reservation)
{
  svr::SparseVoxelPageInstallRewrite rewrite{};
  rewrite.page = reservation.produced.page;
  rewrite.allocation = reservation.allocation;
  rewrite.requiredProducerRevision = reservation.produced.requiredProducerRevision;
  rewrite.rewrite.nodeIndex = reservation.allocation.hierarchyNodes.first;
  rewrite.rewrite.firstChild = reservation.allocation.childIndices.empty() ? 0u : reservation.allocation.childIndices.first;
  rewrite.rewrite.childCount = reservation.allocation.childIndices.count;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

svr::SparseVoxelPageUninstallRewrite uninstallRewrite(
    const svr::SparseVoxelPageId page,
    const svr::SparseVoxelPhysicalPageAllocation &allocation,
    const uint32_t parentNodeIndex)
{
  svr::SparseVoxelPageUninstallRewrite rewrite{};
  rewrite.page = page;
  rewrite.retiredAllocationRevision = allocation.allocationRevision;
  rewrite.rewrite.nodeIndex = parentNodeIndex;
  rewrite.rewrite.firstChild = 0u;
  rewrite.rewrite.childCount = 0u;
  rewrite.rewrite.flags = 0u;
  return rewrite;
}

bool reserveCopyInstall(svr::SparseVoxelSharedPageTable &table, const svr::SparseVoxelProducedPagePayload &produced, const uint64_t copyFence, svr::SparseVoxelReservedPagePayload &reservation, std::string &error)
{
  if (!table.reserveProducedPagePayload(produced, reservation, &error))
    return false;
  if (!table.markPayloadCopied(reservation, copyFence, &error))
    return false;
  return table.installCopiedPagePayload(reservation, {installRewrite(reservation)}, &error);
}

svr::SparseVoxelStreamedInstance instanceFor(const svr::SparseVoxelPageProducer &pageProducer, const svr::SparseVoxelPageId root)
{
  svr::SparseVoxelStreamedInstance instance{};
  instance.instanceIndex = 9u;
  instance.producer = pageProducer;
  instance.rootPage = root;
  instance.firstRootNode = 0u;
  instance.rootNodeCount = 1u;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    instance.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return instance;
}

void testParentEvictionWaitsForResidentAndRetiringChildren()
{
  std::cout << "dependency-aware eviction protects resident and retiring children\n";

  const auto terrain = producer();
  const svr::SparseVoxelPageId root{terrain.producerId, 0u};
  const svr::SparseVoxelPageId parent{terrain.producerId, 1u};
  const svr::SparseVoxelPageId child{terrain.producerId, 2u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({8u, 8u, 8u, 8u, 8u, 8u, 4u, 4u}, &error), "phase9 table configures: " + error);
  check(table.addPage(root, terrain, {}, true, &error), "root fallback registers pinned: " + error);
  check(table.addPage(parent, terrain, {root}, false, &error), "parent detail page depends on root: " + error);
  check(table.addPage(child, terrain, {parent}, false, &error), "child detail page depends on parent: " + error);

  svr::SparseVoxelReservedPagePayload rootReservation{};
  svr::SparseVoxelReservedPagePayload parentReservation{};
  svr::SparseVoxelReservedPagePayload childReservation{};
  check(reserveCopyInstall(table, payload(root, table.entry(root)->generation, terrain.producerRevision, 10u, 1u, 1u), 2u, rootReservation, error), "root fallback installs: " + error);
  check(reserveCopyInstall(table, payload(parent, table.entry(parent)->generation, terrain.producerRevision, 11u, 2u, 1u), 3u, parentReservation, error), "parent detail installs: " + error);
  check(reserveCopyInstall(table, payload(child, table.entry(child)->generation, terrain.producerRevision, 12u, 2u, 1u, 2u), 4u, childReservation, error), "child detail installs with Tree64 sidecar: " + error);

  svr::SparseVoxelPageEvictionDecision rootDecision{};
  check(!table.canEvictPage(root, &rootDecision, &error), "pinned root fallback cannot be evicted");
  check(rootDecision.pageKnown && rootDecision.resident && rootDecision.pinned && !rootDecision.evictable, "root eviction diagnostic records pinned resident fallback");

  svr::SparseVoxelPageEvictionDecision parentDecision{};
  check(!table.canEvictPage(parent, &parentDecision, &error), "parent cannot evict while child is resident");
  check(parentDecision.liveDependentCount == 1u && parentDecision.residentDependentCount == 1u, "parent eviction diagnostic sees resident child dependency");

  const auto childUninstall = uninstallRewrite(child, childReservation.allocation, parentReservation.allocation.hierarchyNodes.first);
  check(table.uninstallPage(child, {childUninstall}, 40u, &error), "child detail uninstalls into fenced retirement: " + error);
  check(table.entry(child) != nullptr && table.entry(child)->retiring(), "child remains live while its retired ranges wait on a fence");
  check(table.uninstallRewrites().size() == 1u, "uninstall rewrite is recorded before retired memory can be reclaimed");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::Tree64Nodes).retiring == childReservation.allocation.tree64Nodes.count, "child Tree64 payload retires behind the same fence");

  parentDecision = {};
  check(!table.canEvictPage(parent, &parentDecision, &error), "parent cannot evict while child is retiring");
  check(parentDecision.liveDependentCount == 1u && parentDecision.residentDependentCount == 0u, "live-dependent scan catches retiring child after resident count drops");
  check(table.reclaim(39u) == 0u, "pre-fence reclaim releases no physical ranges");
  check(!table.uninstallPage(parent, {uninstallRewrite(parent, parentReservation.allocation, rootReservation.allocation.hierarchyNodes.first)}, 41u, &error), "parent uninstall remains blocked before child fence completes");

  check(table.reclaim(40u) >= 1u, "child fence completion reclaims retired ranges");
  check(table.entry(child) != nullptr && table.entry(child)->state == svr::SparseVoxelPageState::Unloaded, "child becomes unloaded only after fence reclamation");
  check(table.canEvictPage(parent, &parentDecision, &error), "parent becomes evictable after child is fully gone: " + error);
  check(table.uninstallPage(parent, {uninstallRewrite(parent, parentReservation.allocation, rootReservation.allocation.hierarchyNodes.first)}, 50u, &error), "parent detail can evict after child retirement completes: " + error);

  svr::SparseVoxelInstanceLODSelection selection{};
  const auto instance = instanceFor(terrain, root);
  check(svr::selectSparseVoxelStreamedInstanceLOD(table, instance, parent, selection, &error), "LOD selection still has root fallback after parent eviction: " + error);
  check(selection.selectedPage == root && selection.rootResident && !selection.desiredResident && selection.selectedFallback && selection.requestedDesiredPage, "eviction lowers detail and requests refinement without creating a terrain hole");
}

void testUploadQueuedChildBlocksParentEviction()
{
  std::cout << "dependency-aware eviction protects upload-queued children\n";

  const auto terrain = producer();
  const svr::SparseVoxelPageId root{terrain.producerId, 100u};
  const svr::SparseVoxelPageId parent{terrain.producerId, 101u};
  const svr::SparseVoxelPageId queuedChild{terrain.producerId, 102u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({8u, 8u, 8u, 8u, 8u, 8u, 4u, 4u}, &error), "queued table configures: " + error);
  check(table.addPage(root, terrain, {}, true, &error), "queued root registers pinned: " + error);
  check(table.addPage(parent, terrain, {root}, false, &error), "queued parent registers: " + error);
  check(table.addPage(queuedChild, terrain, {parent}, false, &error), "queued child registers: " + error);

  svr::SparseVoxelReservedPagePayload rootReservation{};
  svr::SparseVoxelReservedPagePayload parentReservation{};
  check(reserveCopyInstall(table, payload(root, table.entry(root)->generation, terrain.producerRevision, 20u, 1u, 1u), 2u, rootReservation, error), "queued root installs: " + error);
  check(reserveCopyInstall(table, payload(parent, table.entry(parent)->generation, terrain.producerRevision, 21u, 2u, 1u), 3u, parentReservation, error), "queued parent installs: " + error);

  svr::SparseVoxelReservedPagePayload queuedReservation{};
  check(table.reserveProducedPagePayload(payload(queuedChild, table.entry(queuedChild)->generation, terrain.producerRevision, 22u, 2u, 1u, 1u), queuedReservation, &error), "queued child reserves upload memory: " + error);
  check(table.entry(queuedChild) != nullptr && table.entry(queuedChild)->state == svr::SparseVoxelPageState::UploadQueued, "reserved child is upload queued before publication");

  svr::SparseVoxelPageEvictionDecision parentDecision{};
  check(!table.canEvictPage(parent, &parentDecision, &error), "parent cannot evict while child upload is queued");
  check(parentDecision.liveDependentCount == 1u && parentDecision.residentDependentCount == 0u, "eviction diagnostic reports in-flight child as a live dependency");
  check(!table.uninstallPage(parent, {uninstallRewrite(parent, parentReservation.allocation, rootReservation.allocation.hierarchyNodes.first)}, 30u, &error), "uninstall path uses the same live-dependent eviction policy");

  check(table.markPayloadCopied(queuedReservation, 25u, &error), "queued child copy fence completes: " + error);
  check(!table.canEvictPage(parent, &parentDecision, &error), "copied but unpublished child still blocks parent eviction until publication or cancellation");
}

} // namespace

int main()
{
  std::cout << "Terrain / virtual geometry convergence Phase 9 unified eviction\n";
  testParentEvictionWaitsForResidentAndRetiringChildren();
  testUploadQueuedChildBlocksParentEviction();

  if (failures != 0)
  {
    std::cout << failures << " checks failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
