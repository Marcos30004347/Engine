#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/TerrainGPUV2Publication.hpp"

#include <algorithm>
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

svr::TerrainGPUChunkKey terrainKey(const int32_t chunkX, const int32_t chunkZ, const uint32_t slot)
{
  svr::TerrainGPUChunkKey key{};
  key.worldBundleRevision = 0x504841534535ull;
  key.chunkX = chunkX;
  key.chunkY = 0;
  key.chunkZ = chunkZ;
  key.clipmapLOD = 2u;
  key.generatorRevision = 8u;
  key.terrainSlot = slot;
  key.expectedGeneration = 1u;
  return key;
}

svr::SparseVoxelProducedPagePayload payload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t payloadRevision,
    const uint32_t nodes,
    const uint32_t children,
    const uint32_t tree64Nodes = 0u,
    const uint32_t traceNodes = 0u,
    const uint64_t geometryRevision = 0u)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = payloadRevision;
  result.geometryRevision = geometryRevision;
  result.requirements.page = page;
  result.requirements.geometryRevision = geometryRevision;
  result.requirements.traceFormat = traceNodes != 0u ? svr::kVoxelTraceFormatCubic64 : svr::kVoxelTraceFormatInvalid;
  result.requirements.hierarchyNodes.count = nodes;
  result.requirements.childIndices.count = children;
  result.requirements.brickNodes.count = nodes;
  result.requirements.brickOccupancy.count = nodes;
  result.requirements.materials.count = nodes;
  result.requirements.materialCells.count = nodes;
  result.requirements.tree64Nodes.count = tree64Nodes;
  result.requirements.tree64Materials.count = tree64Nodes;
  result.requirements.traceNodes.count = traceNodes;
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

bool reserveCopyInstall(svr::SparseVoxelSharedPageTable &table, const svr::SparseVoxelProducedPagePayload &produced, const uint64_t copyFence, std::string &error)
{
  svr::SparseVoxelReservedPagePayload reservation{};
  if (!table.reserveProducedPagePayload(produced, reservation, &error))
    return false;
  if (!table.markPayloadCopied(reservation, copyFence, &error))
    return false;
  return table.installCopiedPagePayload(reservation, {installRewrite(reservation)}, &error);
}

svr::SparseVoxelInstanceLODSelection select(const svr::SparseVoxelSharedPageTable &table, const svr::SparseVoxelStreamedInstance &instance, const svr::SparseVoxelPageId desired, const std::string &message)
{
  std::string error;
  svr::SparseVoxelInstanceLODSelection selection{};
  check(svr::selectSparseVoxelStreamedInstanceLOD(table, instance, desired, selection, &error), message + ": " + error);
  return selection;
}

void testTerrainChunksBecomeOrdinaryInstances()
{
  std::cout << "terrain chunks map to ordinary streamed instances\n";

  const auto leftKey = terrainKey(0, 0, 3u);
  const auto rightKey = terrainKey(1, 0, 4u);
  svr::TerrainGPUV2StreamedChunkPages leftPages{};
  svr::TerrainGPUV2StreamedChunkPages rightPages{};
  std::string error;
  check(svr::terrainGPUV2StreamedChunkPages(leftKey, leftPages, &error), "left terrain chunk creates streamed page ids: " + error);
  check(svr::terrainGPUV2StreamedChunkPages(rightKey, rightPages, &error), "right terrain chunk creates streamed page ids: " + error);
  check(leftPages.valid() && rightPages.valid(), "terrain page descriptors are valid shared-page identities");
  check(leftPages.producer.producerId == rightPages.producer.producerId, "neighbor terrain chunks share one world/generator producer namespace");
  check(!(leftPages.rootPage == rightPages.rootPage) && !(leftPages.desiredPage == rightPages.desiredPage), "neighbor terrain chunks have independent root and desired pages");
  check(leftPages.rootPage.localPage != leftPages.desiredPage.localPage, "coarse fallback and desired LOD are separate pages for one chunk");

  svr::SparseVoxelStreamedInstance leftInstance{};
  svr::SparseVoxelStreamedInstance rightInstance{};
  check(svr::terrainGPUV2StreamedChunkInstance(leftKey, 0u, 0u, 2u, leftInstance, &error), "left chunk becomes a streamed instance: " + error);
  check(svr::terrainGPUV2StreamedChunkInstance(rightKey, 1u, 64u, 2u, rightInstance, &error), "right chunk becomes a streamed instance: " + error);
  const std::vector<svr::SparseVoxelStreamedInstance> instances{leftInstance, rightInstance};
  check(
      std::all_of(
          instances.begin(),
          instances.end(),
          [](const auto &instance)
          {
            return instance.valid() && instance.producer.kind == svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain;
          }),
      "world terrain chunks use the same streamed-instance contract as objects");

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({16u, 16u, 16u, 16u, 16u, 16u, 8u, 8u}, &error), "phase5 shared page table configures: " + error);
  check(table.addPage(leftPages.rootPage, leftPages.producer, {}, true, &error), "left coarse fallback root registers pinned: " + error);
  check(table.addPage(leftPages.desiredPage, leftPages.producer, {leftPages.rootPage}, false, &error), "left fine terrain page depends on coarse fallback: " + error);
  check(table.addPage(rightPages.rootPage, rightPages.producer, {}, true, &error), "right coarse fallback root registers pinned: " + error);
  check(table.addPage(rightPages.desiredPage, rightPages.producer, {rightPages.rootPage}, false, &error), "right fine terrain page depends on coarse fallback: " + error);

  check(
      reserveCopyInstall(table, payload(leftPages.rootPage, table.entry(leftPages.rootPage)->generation, leftPages.producer.producerRevision, 10u, 1u, 1u), 2u, error),
      "left coarse fallback installs through shared buffers: " + error);
  check(
      reserveCopyInstall(table, payload(rightPages.rootPage, table.entry(rightPages.rootPage)->generation, rightPages.producer.producerRevision, 11u, 1u, 1u), 3u, error),
      "right coarse fallback installs through shared buffers: " + error);
  check(table.entry(leftPages.rootPage)->pinned && table.entry(rightPages.rootPage)->pinned, "coarse terrain fallback pages stay pinned");

  const bool terrainGenerationEnabled = false;
  check(!terrainGenerationEnabled, "test disables fine terrain generation after roots are resident");
  const auto leftFallback = select(table, leftInstance, leftPages.desiredPage, "left chunk selects with fine generation disabled");
  const auto rightFallback = select(table, rightInstance, rightPages.desiredPage, "right chunk selects with fine generation disabled");
  check(leftFallback.selectedPage == leftPages.rootPage && leftFallback.selectedFallback && leftFallback.requestedDesiredPage, "left chunk renders fallback terrain and requests missing fine page");
  check(rightFallback.selectedPage == rightPages.rootPage && rightFallback.selectedFallback && rightFallback.requestedDesiredPage, "right chunk renders fallback terrain and requests missing fine page");

  check(
      reserveCopyInstall(table, payload(leftPages.desiredPage, table.entry(leftPages.desiredPage)->generation, leftPages.producer.producerRevision, 20u, 3u, 2u, 2u), 4u, error),
      "left fine chunk page installs after its fallback root is resident: " + error);
  const auto leftFine = select(table, leftInstance, leftPages.desiredPage, "left chunk selects resident fine page");
  const auto rightStillFallback = select(table, rightInstance, rightPages.desiredPage, "right chunk remains independent fallback");
  check(leftFine.selectedPage == leftPages.desiredPage && !leftFine.selectedFallback && !leftFine.requestedDesiredPage, "left chunk independently selects its resident fine LOD");
  check(rightStillFallback.selectedPage == rightPages.rootPage && rightStillFallback.selectedFallback && rightStillFallback.requestedDesiredPage, "right chunk remains fallback without a global terrain cut change");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).active == 5u, "terrain roots and one fine page occupy shared hierarchy ranges");
}

void testFineTerrainCannotPublishBeforeFallbackRoot()
{
  std::cout << "fine terrain page selection is gated by resident coarse fallback\n";

  const auto key = terrainKey(5, -2, 8u);
  svr::TerrainGPUV2StreamedChunkPages pages{};
  svr::SparseVoxelStreamedInstance instance{};
  std::string error;
  check(svr::terrainGPUV2StreamedChunkPages(key, pages, &error), "gated chunk page ids build: " + error);
  check(svr::terrainGPUV2StreamedChunkInstance(key, 2u, 128u, 2u, instance, &error), "gated chunk instance builds: " + error);

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({8u, 8u, 8u, 8u, 8u, 8u, 4u, 4u}, &error), "gated table configures: " + error);
  check(table.addPage(pages.rootPage, pages.producer, {}, true, &error), "gated root registers pinned: " + error);
  check(table.addPage(pages.desiredPage, pages.producer, {pages.rootPage}, false, &error), "gated fine page depends on root: " + error);

  svr::SparseVoxelInstanceLODSelection selection{};
  check(!svr::selectSparseVoxelStreamedInstanceLOD(table, instance, pages.desiredPage, selection, &error), "selector rejects terrain instance before fallback root is resident");

  const auto finePayload = payload(pages.desiredPage, table.entry(pages.desiredPage)->generation, pages.producer.producerRevision, 30u, 2u, 2u, 1u);
  svr::SparseVoxelReservedPagePayload fineReservation{};
  check(table.reserveProducedPagePayload(finePayload, fineReservation, &error), "fine payload may reserve/copy before root visibility: " + error);
  check(table.markPayloadCopied(fineReservation, 5u, &error), "fine payload copy can complete while hidden: " + error);
  check(!table.installCopiedPagePayload(fineReservation, {installRewrite(fineReservation)}, &error), "fine page install is rejected until coarse fallback dependency is resident");

  check(
      reserveCopyInstall(table, payload(pages.rootPage, table.entry(pages.rootPage)->generation, pages.producer.producerRevision, 31u, 1u, 1u), 6u, error),
      "coarse fallback root installs after rejected fine publication: " + error);
  check(table.installCopiedPagePayload(fineReservation, {installRewrite(fineReservation)}, &error), "fine page installs once fallback dependency is resident: " + error);
  const auto fine = select(table, instance, pages.desiredPage, "gated chunk selects resident fine page");
  check(fine.selectedPage == pages.desiredPage && fine.rootResident && fine.desiredResident, "fine terrain page becomes selectable only behind a resident fallback root");
}

void testDirectCubic64TraceViewInstallsThroughSharedPageTable()
{
  std::cout << "direct Cubic64 trace view installs through shared page table\n";

  const auto key = terrainKey(-3, 4, 10u);
  svr::TerrainGPUV2StreamedChunkPages pages{};
  svr::SparseVoxelStreamedInstance instance{};
  std::string error;
  check(svr::terrainGPUV2StreamedChunkPages(key, pages, &error), "direct trace chunk page ids build: " + error);
  check(svr::terrainGPUV2StreamedChunkInstance(key, 3u, 256u, 2u, instance, &error), "direct trace chunk instance builds: " + error);

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({16u, 16u, 16u, 16u, 16u, 16u, 8u, 8u, 8u, 0u, 0u}, &error), "direct trace table configures trace-node capacity: " + error);
  check(table.addPage(pages.rootPage, pages.producer, {}, true, &error), "direct trace root registers: " + error);
  check(table.addPage(pages.desiredPage, pages.producer, {pages.rootPage}, false, &error), "direct trace desired page registers: " + error);

  check(
      reserveCopyInstall(
          table,
          payload(pages.rootPage, table.entry(pages.rootPage)->generation, pages.producer.producerRevision, 40u, 1u, 1u),
          7u,
          error),
      "direct trace fallback root installs without direct trace view: " + error);
  check(table.entry(pages.rootPage)->resident() && !table.entry(pages.rootPage)->traceable(), "fallback root remains current Tree64/raster-only trace fallback");

  const uint64_t geometryRevision = 0x5500'0001ull;
  check(
      reserveCopyInstall(
          table,
          payload(
              pages.desiredPage,
              table.entry(pages.desiredPage)->generation,
              pages.producer.producerRevision,
              geometryRevision,
              2u,
              2u,
              0u,
              3u,
              geometryRevision),
          8u,
          error),
      "direct Cubic64 trace view installs with the fine terrain page: " + error);
  const auto *entry = table.entry(pages.desiredPage);
  check(entry != nullptr && entry->resident(), "direct trace desired page is resident");
  check(entry != nullptr && entry->traceable(), "direct trace desired page exposes traceability only with matching geometry revision and trace format");
  check(entry != nullptr && entry->allocation.traceNodes.count == 3u, "direct trace node range is allocated beside raster payload");
  check(entry != nullptr && entry->allocation.tree64Nodes.empty(), "direct trace fixture does not require an external Tree64 sidecar");

  const auto fine = select(table, instance, pages.desiredPage, "direct trace chunk selects resident fine page");
  check(fine.selectedPage == pages.desiredPage && fine.desiredResident && !fine.selectedFallback, "screen cut selects the same page identity that direct trace view installed");

  const svr::SparseVoxelPageId badTracePage{pages.producer.producerId, pages.desiredPage.localPage + 1u};
  check(table.addPage(badTracePage, pages.producer, {pages.rootPage}, false, &error), "bad direct trace page registers behind fallback: " + error);
  auto staleTrace = payload(
      badTracePage,
      table.entry(badTracePage)->generation,
      pages.producer.producerRevision,
      0x5500'0002ull,
      1u,
      1u,
      0u,
      1u,
      0u);
  svr::SparseVoxelReservedPagePayload rejected{};
  check(!table.reserveProducedPagePayload(staleTrace, rejected, &error), "direct trace payload without matching geometry revision is rejected before allocation");
}

} // namespace

int main()
{
  std::cout << "Terrain / virtual geometry convergence Phase 5 terrain instances\n";
  testTerrainChunksBecomeOrdinaryInstances();
  testFineTerrainCannotPublishBeforeFallbackRoot();
  testDirectCubic64TraceViewInstallsThroughSharedPageTable();

  if (failures != 0)
  {
    std::cout << failures << " checks failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
