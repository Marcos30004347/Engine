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
  key.worldBundleRevision = 0x504841534536ull;
  key.chunkX = chunkX;
  key.chunkY = 0;
  key.chunkZ = chunkZ;
  key.clipmapLOD = 1u;
  key.generatorRevision = 9u;
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
    const uint32_t tree64Nodes = 0u)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = payloadRevision;
  result.requirements.page = page;
  result.requirements.hierarchyNodes.count = nodes;
  result.requirements.childIndices.count = children;
  result.requirements.brickNodes.count = nodes;
  result.requirements.brickOccupancy.count = nodes;
  result.requirements.materials.count = nodes;
  result.requirements.materialCells.count = nodes;
  result.requirements.tree64Nodes.count = tree64Nodes;
  result.requirements.tree64Materials.count = tree64Nodes;
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

svr::SparseVoxelPagePriorityView viewAt(const float cameraZ)
{
  svr::SparseVoxelPagePriorityView view{};
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
  {
    view.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
    view.worldToCamera[diagonal * 4u + diagonal] = 1.0f;
  }
  view.worldToCamera[14] = -cameraZ;
  view.cameraWorldPosition = {0.0f, 0.0f, cameraZ};
  view.focalPixels = {800.0f, 800.0f};
  view.nearPlane = 0.1f;
  return view;
}

svr::VirtualSparseVoxelFilePageRecord bounds(const float z, const float halfExtent)
{
  svr::VirtualSparseVoxelFilePageRecord record{};
  record.localBoundsMin = {-halfExtent, -halfExtent, z - halfExtent};
  record.localBoundsMax = {halfExtent, halfExtent, z + halfExtent};
  return record;
}

std::vector<svr::SparseVoxelTraversalCandidate> candidates(
    const svr::SparseVoxelStreamedInstance &nearInstance,
    const svr::TerrainGPUV2StreamedChunkPages &nearPages,
    const svr::SparseVoxelStreamedInstance &farInstance,
    const svr::TerrainGPUV2StreamedChunkPages &farPages)
{
  return {
    {nearInstance, nearPages.desiredPage, bounds(4.0f, 1.0f), true},
    {farInstance, farPages.desiredPage, bounds(18.0f, 1.0f), true},
  };
}

void testTraversalProducesCutAndRequestsFromView()
{
  std::cout << "GPU traversal oracle selects resident cut and view-derived requests\n";

  const auto nearKey = terrainKey(0, 0, 3u);
  const auto farKey = terrainKey(0, 1, 4u);
  svr::TerrainGPUV2StreamedChunkPages nearPages{};
  svr::TerrainGPUV2StreamedChunkPages farPages{};
  svr::SparseVoxelStreamedInstance nearInstance{};
  svr::SparseVoxelStreamedInstance farInstance{};
  std::string error;
  check(svr::terrainGPUV2StreamedChunkPages(nearKey, nearPages, &error), "near terrain chunk pages build: " + error);
  check(svr::terrainGPUV2StreamedChunkPages(farKey, farPages, &error), "far terrain chunk pages build: " + error);
  check(svr::terrainGPUV2StreamedChunkInstance(nearKey, 0u, 0u, 2u, nearInstance, &error), "near terrain chunk instance builds: " + error);
  check(svr::terrainGPUV2StreamedChunkInstance(farKey, 1u, 64u, 2u, farInstance, &error), "far terrain chunk instance builds: " + error);

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({16u, 16u, 16u, 16u, 16u, 16u, 8u, 8u}, &error), "phase6 table configures: " + error);
  check(table.addPage(nearPages.rootPage, nearPages.producer, {}, true, &error), "near root registers: " + error);
  check(table.addPage(nearPages.desiredPage, nearPages.producer, {nearPages.rootPage}, false, &error), "near desired page registers: " + error);
  check(table.addPage(farPages.rootPage, farPages.producer, {}, true, &error), "far root registers: " + error);
  check(table.addPage(farPages.desiredPage, farPages.producer, {farPages.rootPage}, false, &error), "far desired page registers: " + error);
  check(reserveCopyInstall(table, payload(nearPages.rootPage, table.entry(nearPages.rootPage)->generation, nearPages.producer.producerRevision, 10u, 1u, 1u), 2u, error), "near coarse fallback root installs: " + error);
  check(reserveCopyInstall(table, payload(farPages.rootPage, table.entry(farPages.rootPage)->generation, farPages.producer.producerRevision, 11u, 1u, 1u), 3u, error), "far coarse fallback root installs: " + error);

  svr::SparseVoxelTraversalFrameResult originResult{};
  const auto candidateList = candidates(nearInstance, nearPages, farInstance, farPages);
  check(svr::traverseSparseVoxelStreamedInstancesLOD(table, candidateList, viewAt(0.0f), originResult, &error), "origin traversal runs without CPU visible terrain set: " + error);
  check(originResult.emissions.size() == 2u && originResult.pageRequests.size() == 2u, "origin traversal emits two fallback pages and two fine-page requests");
  check(originResult.emissions[0].page == nearPages.rootPage && originResult.emissions[1].page == farPages.rootPage, "missing fine pages draw resident parent roots");
  check(originResult.pageRequests.front().page == nearPages.desiredPage, "near camera prioritizes near missing fine terrain page");
  check(originResult.diagnostics.parentFallbackCount == 2u && originResult.diagnostics.pageResidencyMissCount == 2u, "origin traversal diagnostics report parent fallback residency misses");

  svr::SparseVoxelTraversalFrameResult movedResult{};
  check(svr::traverseSparseVoxelStreamedInstancesLOD(table, candidateList, viewAt(16.0f), movedResult, &error), "moved-camera traversal runs from the same page table: " + error);
  check(movedResult.pageRequests.front().page == farPages.desiredPage, "camera movement changes page priority order through traversal feedback");
  check(movedResult.pageRequests.front().priority > movedResult.pageRequests.back().priority, "moved-camera priority is numerically higher for the now-near page");
  check(movedResult.emissions[0].page == nearPages.rootPage && movedResult.emissions[1].page == farPages.rootPage, "camera movement does not require CPU-authored terrain visible-page sets");

  check(
      reserveCopyInstall(table, payload(farPages.desiredPage, table.entry(farPages.desiredPage)->generation, farPages.producer.producerRevision, 20u, 3u, 2u, 2u), 4u, error),
      "far fine page installs through residency after traversal request: " + error);
  svr::SparseVoxelTraversalFrameResult refinedResult{};
  check(svr::traverseSparseVoxelStreamedInstancesLOD(table, candidateList, viewAt(16.0f), refinedResult, &error), "refined traversal runs from resident hierarchy state: " + error);
  check(refinedResult.emissions.size() == 2u && refinedResult.pageRequests.size() == 1u, "resident fine page is emitted and no longer requested");
  check(
      std::any_of(
          refinedResult.emissions.begin(),
          refinedResult.emissions.end(),
          [farPages](const auto &emission)
          {
            return emission.page == farPages.desiredPage && !emission.fallback;
          }),
      "traversal selects resident fine terrain page without CPU publication of a render cut");
  check(refinedResult.pageRequests.front().page == nearPages.desiredPage, "only the still-missing desired page remains in streaming feedback");
  check(refinedResult.diagnostics.childSelectionCount == 1u && refinedResult.diagnostics.parentFallbackCount == 1u, "refined traversal diagnostics distinguish selected child from fallback parent");
}

void testTraversalRejectsHoles()
{
  std::cout << "traversal refuses to emit holes when no fallback root is resident\n";

  const auto key = terrainKey(7, 2, 9u);
  svr::TerrainGPUV2StreamedChunkPages pages{};
  svr::SparseVoxelStreamedInstance instance{};
  std::string error;
  check(svr::terrainGPUV2StreamedChunkPages(key, pages, &error), "hole-test chunk pages build: " + error);
  check(svr::terrainGPUV2StreamedChunkInstance(key, 5u, 128u, 2u, instance, &error), "hole-test chunk instance builds: " + error);

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({4u, 4u, 4u, 4u, 4u, 4u, 2u, 2u}, &error), "hole-test table configures: " + error);
  check(table.addPage(pages.rootPage, pages.producer, {}, true, &error), "hole-test root registers but remains unloaded: " + error);
  check(table.addPage(pages.desiredPage, pages.producer, {pages.rootPage}, false, &error), "hole-test fine page registers: " + error);

  svr::SparseVoxelTraversalFrameResult result{};
  check(
      !svr::traverseSparseVoxelStreamedInstancesLOD(table, {{instance, pages.desiredPage, bounds(8.0f, 1.0f), true}}, viewAt(0.0f), result, &error),
      "traversal rejects a terrain chunk that would otherwise become a hole");
  check(result.emissions.empty() && result.pageRequests.empty(), "failed traversal emits no selected-neither-parent-nor-child cut");
}

} // namespace

int main()
{
  std::cout << "Terrain / virtual geometry convergence Phase 6 GPU traversal cut\n";
  testTraversalProducesCutAndRequestsFromView();
  testTraversalRejectsHoles();

  if (failures != 0)
  {
    std::cout << failures << " checks failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
