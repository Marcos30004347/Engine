#include "voxel/svraster/TerrainGPUBuildScheduler.hpp"
#include "voxel/svraster/TerrainGPUJointPublication.hpp"
#include "voxel/svraster/rendering/TerrainGPUJointPublicationChain.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{
namespace svr = voxel::svraster;

constexpr uint64_t kBundle = 0x504841534535ull;

svr::TerrainGPUJointPublicationCapacities capacities()
{
  return {
    .requests = 4u,
    .rasterResults = 4u,
    .rasterNodes = 32u,
    .rasterChildren = 64u,
    .rasterMaterials = 64u,
    .rasterRoots = 8u,
    .tree64Results = 4u,
    .tree64Nodes = 64u,
    .destinationNodes = 128u,
    .destinationChildren = 256u,
    .destinationMaterials = 256u,
    .destinationRoots = 32u,
    .destinationTree64Nodes = 512u,
    .destinationTree64Instances = 16u,
  };
}

svr::TerrainGPUJointPublicationRequestGPU publicationRequest()
{
  svr::TerrainGPUJointPublicationRequestGPU result{};
  result.terrainSlot = 3u;
  result.rasterResultIndex = 1u;
  result.tree64ResultIndex = 1u;
  result.publishedFrame = 91u;
  result.internalLOD = 2u;
  result.regionMappingId = 7u;
  result.clipmapLOD = 4u;
  result.nodeOffset = 16u;
  result.nodeCapacity = 16u;
  result.childOffset = 32u;
  result.childCapacity = 32u;
  result.rootOffset = 4u;
  result.rootCapacity = 4u;
  result.materialOffset = 64u;
  result.materialCapacity = 32u;
  result.paletteOffset = 12u;
  result.paletteCapacity = 8u;
  result.tree64NodesPerPage = 64u;
  result.nodePageSlot = 1u;
  result.nodePageGeneration = 7u;
  result.materialPageSlot = 5u;
  result.materialPageGeneration = 9u;
  result.materialCellOffset = 2u;
  result.materialCellCount = 6u;
  const float receiverCellSize = 0.5f;
  std::memcpy(
      &result.receiverCellSizeBits, &receiverCellSize,
      sizeof(result.receiverCellSizeBits));
  result.translation = {1.0f, -2.0f, 3.0f, 0.0f};
  result.canonicalTranslationScale = {8, 16, 24, 2};
  return result;
}

void testRequestValidation()
{
  const auto limits = capacities();
  auto request = publicationRequest();
  std::string error;
  assert(svr::validateTerrainGPUJointPublicationRequest(request, limits, &error));

  request.materialOffset = 65u;
  assert(!svr::validateTerrainGPUJointPublicationRequest(request, limits, &error));
  request = publicationRequest();
  request.nodeOffset = 120u;
  assert(!svr::validateTerrainGPUJointPublicationRequest(request, limits, &error));
  request = publicationRequest();
  request.tree64NodesPerPage = 0u;
  assert(!svr::validateTerrainGPUJointPublicationRequest(request, limits, &error));
  request = publicationRequest();
  request.nodePageSlot = 8u;
  assert(!svr::validateTerrainGPUJointPublicationRequest(request, limits, &error));
  request = publicationRequest();
  request.finerClipPacked = 0x08080000u;
  assert(!svr::validateTerrainGPUJointPublicationRequest(request, limits, &error));
  request.pageSide = 8u;
  request.translation[3] = 4.0f;
  assert(svr::validateTerrainGPUJointPublicationRequest(request, limits, &error));
}

svr::TerrainGPUPhysicalPageLease lease(const uint64_t raster, const uint32_t page, const uint32_t generation)
{
  return {raster, page, generation, page + 1u, generation + 1u};
}

void testFenceDeferredPageOwnership()
{
  svr::TerrainGPUJointPublicationRetirementQueue queue;
  std::string error;
  const auto oldPage = lease(1u, 2u, 3u);
  const auto replacement = lease(4u, 5u, 6u);
  assert(queue.begin(11u, replacement, &error));
  assert(!queue.begin(11u, replacement, &error));
  assert(queue.complete(11u, true, oldPage, 20u, &error));
  assert(queue.pendingCount() == 0u && queue.retiredCount() == 1u);
  assert(queue.reclaim(19u).empty());
  const auto reclaimedOld = queue.reclaim(20u);
  assert(reclaimedOld.size() == 1u);
  assert(reclaimedOld.front().rasterAllocation == oldPage.rasterAllocation);

  assert(queue.begin(12u, replacement, &error));
  assert(queue.complete(12u, false, oldPage, 24u, &error));
  assert(queue.reclaim(23u).empty());
  const auto reclaimedReplacement = queue.reclaim(24u);
  assert(reclaimedReplacement.size() == 1u);
  assert(reclaimedReplacement.front().rasterAllocation == replacement.rasterAllocation);

  assert(queue.begin(13u, replacement, &error));
  assert(!queue.complete(13u, false, oldPage, 23u, &error));

  const auto evicted = lease(8u, 9u, 10u);
  assert(queue.retire(evicted, 30u, &error));
  assert(queue.reclaim(29u).empty());
  assert(queue.reclaim(30u).front().rasterAllocation ==
      evicted.rasterAllocation);
}

svr::TerrainGPUBuildBudget schedulerBudget()
{
  svr::TerrainGPUBuildBudget result{};
  result.targetGPUTimeMicroseconds = 5000u;
  result.admissionCeilingMicroseconds = 5000u;
  result.maximumDispatchesPerFrame = 8u;
  result.maximumPagesStartedPerFrame = 2u;
  result.maximumPagesPublishedPerFrame = 2u;
  result.maximumMapUploadBytesPerFrame = 4096u;
  result.maximumScratchBytes = 4096u;
  result.maximumInFlightJobs = 4u;
  return result;
}

svr::TerrainGPUBuildRequest buildRequest(const uint32_t slot)
{
  svr::TerrainGPUBuildRequest result{};
  result.key = {kBundle, 1, -2, 0u, 7u, slot, 1u};
  result.jobClass = svr::TerrainGPUJobClass::VisibleMissing;
  result.projectedError = 2.0f;
  result.distance = 10.0f;
  result.scratchBytes = 512u;
  result.semanticRevision = 1u;
  result.mapsReady = true;
  return result;
}

void driveReady(svr::TerrainGPUBuildScheduler &scheduler, const uint64_t jobId, uint64_t &frame, uint64_t &fence)
{
  for (uint32_t iteration = 0u; iteration < 128u; ++iteration)
  {
    if (scheduler.job(jobId)->stage == svr::TerrainGPUBuildStage::ReadyToPublish)
      return;
    const auto scheduled = scheduler.scheduleFrame({frame++, fence, 0u, 16667u, false});
    for (const auto &dispatch : scheduled.dispatches)
      assert(scheduler.completeDispatch({dispatch.dispatchId, ++fence, svr::TerrainGPUFailure::None, 10u}));
  }
  assert(false && "Phase 5 scheduler job did not become publishable");
}

uint64_t readyJob(svr::TerrainGPUBuildScheduler &scheduler, const uint32_t slot, uint64_t &frame, uint64_t &fence)
{
  scheduler.setTerrainGeneration(slot, 1u);
  uint64_t job = 0u;
  assert(scheduler.request(buildRequest(slot), job));
  driveReady(scheduler, job, frame, fence);
  return job;
}

void testPublicationCompletionContract()
{
  svr::TerrainGPUBuildScheduler scheduler(schedulerBudget());
  scheduler.setBundleSignature(kBundle);
  uint64_t frame = 1u;
  uint64_t fence = 0u;

  const uint64_t accepted = readyJob(scheduler, 1u, frame, fence);
  auto publication = scheduler.scheduleFrame({frame++, fence, 0u, 16667u, true});
  assert(publication.publications.size() == 1u);
  assert(scheduler.diagnostics().inFlightJobs == 1u);
  assert(scheduler.scheduleFrame({frame++, fence, 0u, 16667u, true}).publications.empty());
  const uint64_t publicationFence = fence + 5u;
  assert(scheduler.completePublication({accepted, publicationFence, true}));
  assert(scheduler.job(accepted)->stage == svr::TerrainGPUBuildStage::Published);
  assert(scheduler.diagnostics().scratch.deferredAllocations == 1u);
  scheduler.scheduleFrame({frame++, publicationFence - 1u});
  assert(scheduler.diagnostics().scratch.liveAllocations == 1u);
  scheduler.scheduleFrame({frame++, publicationFence});
  assert(scheduler.diagnostics().scratch.liveAllocations == 0u);
  fence = publicationFence;

  const uint64_t rejected = readyJob(scheduler, 2u, frame, fence);
  assert(scheduler.scheduleFrame({frame++, fence, 0u, 16667u, true}).publications.size() == 1u);
  assert(scheduler.completePublication({rejected, ++fence, false}));
  assert(scheduler.job(rejected)->stage == svr::TerrainGPUBuildStage::Failed);
  assert(scheduler.result(rejected)->status == svr::TerrainGPUFailure::InvalidResult);

  const uint64_t stale = readyJob(scheduler, 3u, frame, fence);
  assert(scheduler.scheduleFrame({frame++, fence, 0u, 16667u, true}).publications.size() == 1u);
  scheduler.setTerrainGeneration(3u, 2u);
  scheduler.scheduleFrame({frame++, fence});
  assert(scheduler.job(stale)->stage == svr::TerrainGPUBuildStage::ReadyToPublish);
  assert(scheduler.completePublication({stale, ++fence, false}));
  assert(scheduler.job(stale)->stage == svr::TerrainGPUBuildStage::Cancelled);
  assert(scheduler.result(stale)->status == svr::TerrainGPUFailure::StaleGeneration);
}

void testGPUResidentPageRebuildAndEviction()
{
  svr::TerrainGPUBuildScheduler scheduler(schedulerBudget());
  svr::TerrainGPUResidentPageCache cache({
      .maximumResidentPages = 2u,
      .maximumResidentBytes = 2048u,
      .evictUnrequired = true,
  });
  uint64_t frame = 1u;
  uint64_t fence = 0u;
  std::string error;

  auto pageA = buildRequest(1u);
  pageA.key.chunkX = 10;
  svr::TerrainGPUPageResidencyUpdate update;
  assert(cache.updateRequired(frame++, fence,
      {{pageA, 768u, false}}, scheduler, update, &error));
  assert(update.scheduledBuilds.size() == 1u);
  const uint64_t firstBuild = update.scheduledBuilds.front();
  driveReady(scheduler, firstBuild, frame, fence);
  assert(scheduler.scheduleFrame(
      {frame++, fence, 0u, 16667u, true}).publications.size() == 1u);
  const auto firstLease = lease(101u, 2u, 3u);
  assert(cache.beginPublication(firstBuild, firstLease, &error));
  assert(cache.completePublication(
      firstBuild, true, ++fence, scheduler, &error));
  assert(cache.diagnostics().pageInstallCount == 1u);

  const auto identityA = svr::TerrainGPUPageIdentity::from(pageA.key);
  assert(cache.resident(identityA).has_value());
  assert(cache.updateRequired(frame++, fence,
      {{pageA, 768u, false}}, scheduler, update, &error));
  assert(update.scheduledBuilds.empty());
  assert(update.evictions.empty());

  auto pageB = buildRequest(2u);
  pageB.key.chunkX = 11;
  const uint64_t evictionFence = fence + 4u;
  assert(cache.updateRequired(frame++, evictionFence,
      {{pageB, 640u, false}}, scheduler, update, &error));
  assert(update.evictions.size() == 1u);
  assert(update.evictions.front().identity == identityA);
  assert(!cache.resident(identityA).has_value());
  assert(cache.reclaim(evictionFence - 1u).empty());
  const auto reclaimed = cache.reclaim(evictionFence);
  assert(reclaimed.size() == 1u);
  assert(reclaimed.front().rasterAllocation == firstLease.rasterAllocation);

  assert(cache.updateRequired(frame++, evictionFence,
      {{pageA, 768u, false}}, scheduler, update, &error));
  assert(update.scheduledBuilds.size() == 1u);
  assert(update.scheduledBuilds.front() != firstBuild);
  assert(update.cancelledBuilds.size() == 1u);
  const auto diagnostics = cache.diagnostics();
  assert(diagnostics.residentPages == 0u);
  assert(diagnostics.pendingBuilds == 1u);
  assert(diagnostics.cacheHits == 1u);
  assert(diagnostics.rebuildRequests == 3u);
  assert(diagnostics.pageInstallCount == 1u);
  assert(diagnostics.pageUninstallCount == 1u);
  assert(diagnostics.evictedPages == 1u);
  assert(diagnostics.reclaimedLeases == 1u);
}

void testGPUResidentPageBudgetRejectsOversizedCut()
{
  svr::TerrainGPUBuildScheduler scheduler(schedulerBudget());
  svr::TerrainGPUResidentPageCache cache({1u, 512u, true});
  auto pageA = buildRequest(1u);
  auto pageB = buildRequest(2u);
  pageB.key.chunkX += 1;
  svr::TerrainGPUPageResidencyUpdate update;
  std::string error;
  assert(!cache.updateRequired(1u, 0u,
      {{pageA, 300u, false}, {pageB, 300u, false}},
      scheduler, update, &error));
  assert(!error.empty());
  assert(scheduler.idle());
}

} // namespace

int main()
{
  testRequestValidation();
  testFenceDeferredPageOwnership();
  testPublicationCompletionContract();
  testGPUResidentPageRebuildAndEviction();
  testGPUResidentPageBudgetRejectsOversizedCut();
  std::cout << "GPU terrain Phase 5 publication contract tests passed\n";
  return 0;
}
