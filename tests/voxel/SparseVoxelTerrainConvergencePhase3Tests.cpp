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

svr::SparseVoxelPageProducer terrainProducer(const uint32_t revision = 9u)
{
  svr::SparseVoxelPageProducer producer{};
  producer.producerId = 0x7007u;
  producer.kind = svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain;
  producer.producerRevision = revision;
  producer.producesOnGPU = true;
  producer.debugName = "phase3-terrain-producer";
  return producer;
}

svr::SparseVoxelPhysicalPageAllocation requirements(
    const svr::SparseVoxelPageId page,
    const uint32_t nodes,
    const uint32_t children,
    const uint32_t tree64Nodes = 0u,
    const uint32_t cullErrorWords = 0u,
    const uint64_t geometryRevision = 0u)
{
  svr::SparseVoxelPhysicalPageAllocation result{};
  result.page = page;
  result.geometryRevision = geometryRevision;
  result.hierarchyNodes.count = nodes;
  result.childIndices.count = children;
  result.brickNodes.count = nodes;
  result.brickOccupancy.count = nodes;
  result.materials.count = nodes;
  result.materialCells.count = nodes;
  result.tree64Nodes.count = tree64Nodes;
  result.tree64Materials.count = tree64Nodes;
  result.cullErrorWords.count = cullErrorWords;
  return result;
}

svr::SparseVoxelProducedPagePayload producedPayload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t payloadRevision,
    const uint32_t nodes,
    const uint32_t children,
    const uint32_t tree64Nodes = 0u,
    const uint32_t cullErrorWords = 0u,
    const uint64_t geometryRevision = 0u)
{
  svr::SparseVoxelProducedPagePayload payload{};
  payload.page = page;
  payload.expectedPageGeneration = expectedGeneration;
  payload.requiredProducerRevision = producerRevision;
  payload.payloadRevision = payloadRevision;
  payload.geometryRevision = geometryRevision;
  payload.productionFence = 3u;
  payload.requirements = requirements(page, nodes, children, tree64Nodes, cullErrorWords, geometryRevision);
  payload.installRewriteCount = 1u;
  payload.uninstallRewriteCount = 1u;
  payload.payloadComplete = true;
  return payload;
}

svr::TerrainGPUV2PayloadResultGPU
v2PayloadResult(const uint32_t producerRevision, const uint32_t terrainGeneration, const uint64_t payloadRevision, const uint32_t nodes, const uint32_t children, const uint32_t tree64Nodes)
{
  svr::TerrainGPUV2PayloadResultGPU result{};
  result.status = static_cast<uint32_t>(svr::TerrainGPUV2PublicationStatus::Approved);
  result.flags = svr::TerrainGPUV2PayloadOccupancyOnly | svr::TerrainGPUV2PayloadNoMicroGeometry | svr::TerrainGPUV2PayloadRasterHierarchy;
  result.rasterNodeCount = nodes;
  result.rasterMaterialCount = nodes;
  result.rasterChildCount = children;
  result.rasterRootCount = 1u;
  result.rasterOccupiedVoxelCount = nodes;
  result.terrainGeneration = terrainGeneration;
  result.rasterGeneratorRevision = producerRevision;
  result.treeNodeCount = tree64Nodes;
  result.treeVisitedNodeCount = tree64Nodes;
  result.treeOccupiedVoxelCount = nodes;
  result.treeGeneratorRevision = producerRevision;
  result.geometryRevisionLow = static_cast<uint32_t>(payloadRevision);
  result.geometryRevisionHigh = static_cast<uint32_t>(payloadRevision >> 32u);
  return result;
}

svr::SparseVoxelPageInstallRewrite installRewrite(const svr::SparseVoxelReservedPagePayload &reservation)
{
  svr::SparseVoxelPageInstallRewrite rewrite{};
  rewrite.page = reservation.produced.page;
  rewrite.allocation = reservation.allocation;
  rewrite.requiredProducerRevision = reservation.produced.requiredProducerRevision;
  rewrite.rewrite.nodeIndex = reservation.allocation.hierarchyNodes.first;
  rewrite.rewrite.firstChild = reservation.allocation.childIndices.first;
  rewrite.rewrite.childCount = reservation.allocation.childIndices.count;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

bool isResident(const svr::SparseVoxelSharedPageTable &table, const svr::SparseVoxelPageId page)
{
  const auto *entry = table.entry(page);
  return entry != nullptr && entry->resident();
}

void installPinnedRoot(svr::SparseVoxelSharedPageTable &table, const svr::SparseVoxelPageProducer &producer, const svr::SparseVoxelPageId root, std::string &error)
{
  check(table.addPage(root, producer, {}, true, &error), "root/coarse fallback page is registered: " + error);
  const auto payload = producedPayload(root, 0u, producer.producerRevision, 1u, 1u, 1u);
  svr::SparseVoxelReservedPagePayload reservation{};
  check(table.reserveProducedPagePayload(payload, reservation, &error), "root payload reserves ranges: " + error);
  check(table.markPayloadCopied(reservation, 4u, &error), "root payload copy completes: " + error);
  check(table.installCopiedPagePayload(reservation, {installRewrite(reservation)}, &error), "root install rewrite exposes fallback page: " + error);
}

void testTerrainProductionDoesNotPublish()
{
  std::cout << "terrain payload production is separate from page installation\n";

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(table.configure({8u, 8u, 8u, 8u, 8u, 8u, 4u, 4u}, &error), "phase3 shared table configures: " + error);

  const auto producer = terrainProducer();
  const svr::SparseVoxelPageId root{producer.producerId, 0u};
  const svr::SparseVoxelPageId terrainPage{producer.producerId, 1u};
  installPinnedRoot(table, producer, root, error);
  check(table.addPage(terrainPage, producer, {root}, false, &error), "terrain child page is registered: " + error);

  const auto v2Result = v2PayloadResult(producer.producerRevision, 77u, 2u, 2u, 2u, 2u);
  svr::SparseVoxelProducedPagePayload produced{};
  check(
      svr::terrainGPUV2ProducedPagePayload(terrainPage, table.entry(terrainPage)->generation, producer.producerRevision, v2Result, produced, &error),
      "Terrain GPU V2 payload result becomes neutral produced metadata: " + error);
  check(produced.geometryRevision == 2u && produced.requirements.geometryRevision == 2u, "terrain produced metadata carries geometry revision for future culling sidecars");

  check(!isResident(table, terrainPage), "metadata-only production does not expose terrain page to traversal");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).active == 1u, "metadata-only production does not reserve hierarchy storage");
  check(table.installRewrites().size() == 1u, "metadata-only production records no terrain install rewrites");

  svr::SparseVoxelReservedPagePayload reservation{};
  check(table.reserveProducedPagePayload(produced, reservation, &error), "produced metadata reserves shared physical ranges: " + error);
  check(!isResident(table, terrainPage), "reservation still does not expose page to traversal");
  check(table.entry(terrainPage)->state == svr::SparseVoxelPageState::UploadQueued, "reservation marks page as upload-queued, not resident");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::Tree64Nodes).active == 2u, "Tree64 sidecar storage reserves with the same payload ticket");

  check(!table.installCopiedPagePayload(reservation, {installRewrite(reservation)}, &error), "install is rejected before payload copy completes");
  check(!isResident(table, terrainPage), "failed pre-copy install does not expose page");

  check(table.markPayloadCopied(reservation, 5u, &error), "copy/finalize fence marks reserved payload copied: " + error);
  check(!isResident(table, terrainPage), "copied payload is still hidden until install rewrites run");

  check(table.installCopiedPagePayload(reservation, {installRewrite(reservation)}, &error), "install rewrites expose copied page: " + error);
  check(isResident(table, terrainPage), "terrain page becomes resident only after install rewrite step");
  check(table.installRewrites().size() == 2u, "install rewrite storage records root plus terrain page install");
}

void testCullErrorSidecarInstallsBesideTree64()
{
  std::cout << "q8 culling sidecar installs beside current Tree64 payload\n";

  std::vector<float> childErrors(svr::kVoxelTraceChildCount);
  for (uint32_t child = 0u; child < svr::kVoxelTraceChildCount; ++child)
    childErrors[child] = static_cast<float>(child);
  svr::VoxelTraceCullErrorSidecarCPU sidecar{};
  std::string error;
  check(svr::voxelTraceBuildCullErrorSidecar(childErrors, sidecar, &error), "q8 child-error sidecar builds: " + error);
  check(sidecar.childErrorWords.size() == 16u, "64 child errors pack into sixteen u32 sidecar words");

  svr::SparseVoxelSharedPageTable table{};
  check(
      table.configure({8u, 8u, 8u, 8u, 8u, 8u, 4u, 4u, 0u, 0u, 16u}, &error),
      "sidecar table configures cull-error allocator without trace-node capacity: " + error);

  const auto producer = terrainProducer();
  const svr::SparseVoxelPageId root{producer.producerId, 0u};
  const svr::SparseVoxelPageId terrainPage{producer.producerId, 1u};
  const svr::SparseVoxelPageId replacement{producer.producerId, 2u};
  installPinnedRoot(table, producer, root, error);
  check(table.addPage(terrainPage, producer, {root}, false, &error), "sidecar terrain page is registered: " + error);
  check(table.addPage(replacement, producer, {root}, false, &error), "sidecar replacement page is registered: " + error);

  const uint64_t geometryRevision = 0x3300'0001ull;
  const auto payload = producedPayload(
      terrainPage,
      table.entry(terrainPage)->generation,
      producer.producerRevision,
      geometryRevision,
      2u,
      2u,
      2u,
      static_cast<uint32_t>(sidecar.childErrorWords.size()),
      geometryRevision);
  svr::SparseVoxelReservedPagePayload reservation{};
  check(table.reserveProducedPagePayload(payload, reservation, &error), "sidecar payload reserves raster, Tree64 and cull-error ranges together: " + error);
  check(reservation.allocation.tree64Nodes.count == 2u, "current Tree64 sidecar remains present");
  check(reservation.allocation.traceNodes.empty(), "direct trace nodes are not required for the culling sidecar experiment");
  check(reservation.allocation.cullErrorWords.count == sidecar.childErrorWords.size(), "q8 culling sidecar gets its own physical range");
  check(table.markPayloadCopied(reservation, 5u, &error), "sidecar payload copy completes: " + error);
  check(table.installCopiedPagePayload(reservation, {installRewrite(reservation)}, &error), "sidecar payload installs atomically: " + error);

  const auto *entry = table.entry(terrainPage);
  check(entry != nullptr && entry->resident(), "sidecar page is resident after install");
  check(entry != nullptr && entry->hasCullErrorSidecar(), "resident page exposes culling sidecar with matching geometry revision");
  check(entry != nullptr && !entry->traceable(), "culling sidecar alone does not make the page directly traceable");

  check(
      svr::voxelTraceCullExpansionDecision(&sidecar, 0u, 0.2f) ==
          svr::VoxelTraceCullExpansionDecision::DefinitelySkip,
      "low-error child is definitely skipped without exact f32 fallback");
  check(
      svr::voxelTraceCullExpansionDecision(&sidecar, 63u, 62.8f) ==
          svr::VoxelTraceCullExpansionDecision::MustExpand,
      "high-error child is definitely expanded without exact f32 fallback");
  check(
      svr::voxelTraceCullExpansionDecision(
          &sidecar,
          32u,
          svr::voxelTraceCullSidecarDecodedError(sidecar, 32u)) ==
          svr::VoxelTraceCullExpansionDecision::ExactFallback,
      "borderline q8 child falls back to exact f32/current path");

  const auto uninstall = [&reservation, rootEntry = table.entry(root)]()
  {
    svr::SparseVoxelPageUninstallRewrite rewrite{};
    rewrite.page = reservation.produced.page;
    rewrite.retiredAllocationRevision = reservation.allocation.allocationRevision;
    rewrite.rewrite.nodeIndex = rootEntry->allocation.hierarchyNodes.first;
    rewrite.rewrite.firstChild = 0u;
    rewrite.rewrite.childCount = 0u;
    rewrite.rewrite.flags = 0u;
    return rewrite;
  }();
  check(table.uninstallPage(terrainPage, {uninstall}, 11u, &error), "sidecar page uninstalls through parent fallback rewrite: " + error);
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::CullErrorWords).retiring == sidecar.childErrorWords.size(), "cull-error sidecar range retires behind the same fence");

  const auto replacementPayload = producedPayload(
      replacement,
      table.entry(replacement)->generation,
      producer.producerRevision,
      0x3300'0002ull,
      2u,
      2u,
      2u,
      static_cast<uint32_t>(sidecar.childErrorWords.size()),
      0x3300'0002ull);
  svr::SparseVoxelReservedPagePayload earlyReplacement{};
  check(!table.reserveProducedPagePayload(replacementPayload, earlyReplacement, &error), "retiring cull sidecar range is not reused before fence completion");
  check(table.reclaim(11u) >= 1u, "completion fence reclaims cull sidecar and payload ranges");
  svr::SparseVoxelReservedPagePayload replacementReservation{};
  check(table.reserveProducedPagePayload(replacementPayload, replacementReservation, &error), "replacement sidecar reserves after reclaim: " + error);
  check(replacementReservation.allocation.cullErrorWords.first == reservation.allocation.cullErrorWords.first, "cull sidecar range is reused only after reclaim");
}

void testStaleProducerResultRejectedBeforeAllocation()
{
  std::cout << "stale producer result rejection\n";

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(table.configure({4u, 4u, 4u, 4u, 4u, 4u, 0u, 0u}, &error), "stale-result table configures: " + error);

  const auto producer = terrainProducer(12u);
  const svr::SparseVoxelPageId root{producer.producerId, 0u};
  const svr::SparseVoxelPageId terrainPage{producer.producerId, 1u};
  installPinnedRoot(table, producer, root, error);
  check(table.addPage(terrainPage, producer, {root}, false, &error), "stale target page is registered: " + error);

  const auto activeBefore = table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).active;
  auto staleRevision = producedPayload(terrainPage, table.entry(terrainPage)->generation, producer.producerRevision - 1u, 2u, 1u, 1u);
  svr::SparseVoxelReservedPagePayload staleReservation{};
  check(!table.reserveProducedPagePayload(staleRevision, staleReservation, &error), "late payload with stale producer revision is rejected");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).active == activeBefore, "stale producer result reserves no physical storage");
  check(table.entry(terrainPage)->state == svr::SparseVoxelPageState::Unloaded, "stale producer result does not change page state");

  auto incomplete = producedPayload(terrainPage, table.entry(terrainPage)->generation, producer.producerRevision, 3u, 1u, 1u);
  incomplete.payloadComplete = false;
  check(!table.reserveProducedPagePayload(incomplete, staleReservation, &error), "incomplete producer metadata is rejected before allocation");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).active == activeBefore, "incomplete producer result reserves no physical storage");

  auto badCullSidecar = producedPayload(terrainPage, table.entry(terrainPage)->generation, producer.producerRevision, 4u, 1u, 1u, 1u, 1u, 0u);
  check(!table.reserveProducedPagePayload(badCullSidecar, staleReservation, &error), "culling sidecar without geometry revision is rejected before allocation");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).active == activeBefore, "bad culling sidecar reserves no physical storage");
}

} // namespace

int main()
{
  std::cout << "Terrain / virtual geometry convergence Phase 3 production split\n";
  testTerrainProductionDoesNotPublish();
  testCullErrorSidecarInstallsBesideTree64();
  testStaleProducerResultRejectedBeforeAllocation();

  if (failures != 0)
  {
    std::cout << failures << " checks failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
