#include "voxel/svraster/TerrainGPUV2Scan.hpp"
#include "voxel/svraster/TerrainGPUV2Scheduling.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <set>
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

svr::TerrainGPUV2Budget budget()
{
  svr::TerrainGPUV2Budget result{};
  result.targetGenerationMicroseconds = 2000u;
  result.hardGenerationMicroseconds = 2500u;
  result.maximumSingleDispatchMicroseconds = 500u;
  result.maximumChunksPerFrame = 128u;
  result.maximumNewChunksPerFrame = 128u;
  result.maximumWorkgroupsPerFrame = 8192u;
  result.scratchCapacityBytes = 1u << 20u;
  result.outputCapacityBytes = 1u << 20u;
  result.maximumScratchReservationBytesPerFrame = 1u << 20u;
  result.maximumOutputReservationBytesPerFrame = 1u << 20u;
  result.minimumAllocationClassBytes = 256u;
  result.parameterLaneCount = 256u;
  return result;
}

svr::TerrainGPUV2Request request(const int32_t x, const float errorPixels = 1.0f, const svr::TerrainGPUV2PriorityClass priority = svr::TerrainGPUV2PriorityClass::VisibleMissing, const bool atlasResident = true)
{
  svr::TerrainGPUV2Request result{};
  result.key = {41u, x, 0, 0, 0u, 3u, 7u, 11u};
  result.shape = {1u, 1u, 1u, 1u};
  result.priorityClass = priority;
  result.projectedErrorPixels = errorPixels;
  result.distanceMetres = 1000.0f - errorPixels;
  result.scratchBytes = 256u;
  result.outputReservationBytes = 256u;
  result.atlasUploadBytes = atlasResident ? 0u : 1024u;
  result.atlasResident = atlasResident;
  return result;
}

void initialize(svr::TerrainGPUV2Scheduler &scheduler)
{
  scheduler.setWorldBundleRevision(41u);
  scheduler.setTerrainGeneration(7u, 11u);
}

uint64_t enqueue(svr::TerrainGPUV2Scheduler &scheduler, const svr::TerrainGPUV2Request &value)
{
  uint64_t id = 0u;
  std::string error;
  check(scheduler.request(value, id, &error), "job request is accepted: " + error);
  return id;
}

void finishDispatches(svr::TerrainGPUV2Scheduler &scheduler, const svr::TerrainGPUV2FrameSchedule &schedule, uint64_t &fence, const bool immediateTiming = true, const uint32_t measuredMicroseconds = 0u)
{
  for (const auto &dispatch : schedule.dispatches)
  {
    svr::TerrainGPUV2DispatchCompletion completion{};
    completion.dispatchId = dispatch.id;
    completion.completionFence = ++fence;
    if (immediateTiming)
      completion.elapsedMicroseconds = measuredMicroseconds == 0u ? dispatch.predictedMicroseconds : measuredMicroseconds;
    std::string error;
    check(scheduler.completeDispatch(completion, &error), "scheduled dispatch completes asynchronously: " + error);
  }
}

bool driveReady(svr::TerrainGPUV2Scheduler &scheduler, const uint64_t jobId, uint64_t &frame, uint64_t &fence)
{
  for (uint32_t iteration = 0u; iteration < 32u; ++iteration)
  {
    const auto state = scheduler.job(jobId);
    if (state && state->stage == svr::TerrainGPUJobStage::ReadyToPublish)
      return true;
    const auto schedule = scheduler.scheduleFrame({++frame, fence, 17u, 0u, 16667u, false});
    finishDispatches(scheduler, schedule, fence);
  }
  return false;
}

void testScanAndCompaction()
{
  std::cout << "exclusive scan and stable compaction\n";
  const std::vector<uint32_t> sizes{0u, 1u, 255u, 256u, 257u, 65535u, 65536u, 65537u, svr::kTerrainGPUV2ScanMaximumElements};
  for (const uint32_t size : sizes)
  {
    std::vector<uint32_t> input(size);
    for (uint32_t index = 0u; index < size; ++index)
      input[index] = index % 3u;
    std::vector<uint32_t> output;
    svr::TerrainGPUV2ScanStatusGPU status{};
    std::string error;
    const bool scanned = svr::terrainGPUV2ExclusiveScanCPU(input, output, status, &error);
    uint64_t expected = 0u;
    bool exact = scanned && output.size() == input.size();
    for (uint32_t index = 0u; exact && index < size; ++index)
    {
      exact &= output[index] == expected;
      expected += input[index];
    }
    exact &= status.total == expected && status.overflow == 0u;
    check(exact, "scan matches serial oracle at size " + std::to_string(size) + ": " + error);
  }

  std::mt19937 random(0x52A91u);
  for (uint32_t iteration = 0u; iteration < 32u; ++iteration)
  {
    const uint32_t size = 1u + random() % 12000u;
    std::vector<uint32_t> flags(size);
    std::vector<uint32_t> expected;
    for (uint32_t index = 0u; index < size; ++index)
      if ((flags[index] = (random() & 3u) == 0u ? 1u : 0u) != 0u)
        expected.push_back(index);
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> compacted;
    svr::TerrainGPUV2ScanStatusGPU status{};
    svr::IndirectDispatchArgsGPU indirect{};
    std::string error;
    const bool compact = svr::terrainGPUV2CompactCPU(flags, static_cast<uint32_t>(expected.size()), offsets, compacted, status, indirect, &error);
    check(compact && status.overflow == 0u && compacted == expected && indirect.x == (expected.size() + 63u) / 64u, "random compaction retains deterministic input order: " + error);
  }

  std::vector<uint32_t> scanSentinel{19u, 23u};
  svr::TerrainGPUV2ScanStatusGPU status{};
  std::string error;
  check(
      !svr::terrainGPUV2ExclusiveScanCPU({std::numeric_limits<uint32_t>::max(), 1u}, scanSentinel, status, &error) && status.overflow == 1u && scanSentinel == std::vector<uint32_t>({19u, 23u}),
      "arithmetic overflow is latched before scan output is committed");

  std::vector<uint32_t> offsets;
  std::vector<uint32_t> compactSentinel{0xABCDEF01u, 0x12345678u};
  svr::IndirectDispatchArgsGPU indirect{9u, 9u, 9u, 9u};
  check(
      svr::terrainGPUV2CompactCPU({1u, 0u, 1u}, 1u, offsets, compactSentinel, status, indirect, &error) && status.overflow == 1u && indirect.x == 0u &&
          compactSentinel == std::vector<uint32_t>({0xABCDEF01u, 0x12345678u}),
      "capacity overflow is reported before compacted output writes");
  check(!svr::terrainGPUV2CompactCPU({1u, 2u}, 2u, offsets, compactSentinel, status, indirect, &error) && status.overflow == 1u && indirect.x == 0u, "non-binary compaction input fails closed");
}

void testFencedStorage()
{
  std::cout << "fenced size classes and immutable parameter lanes\n";
  svr::TerrainGPUV2FencedArena arena(4096u, 256u);
  const auto a = arena.allocate(257u);
  const auto b = arena.allocate(700u);
  check(a && b && a->reservedBytes == 512u && b->reservedBytes == 1024u, "arena rounds reservations to bounded power-of-two classes");
  check(arena.releaseDeferred(*a, 8u) && arena.reclaim(7u) == 0u && arena.stats().reservedBytes == 1536u, "released storage remains owned before its completion fence");
  check(
      arena.reclaim(8u) == 1u && arena.stats().reservedBytes == 1024u && arena.releaseDeferred(*b, 9u) && arena.reclaim(9u) == 1u && arena.allocate(4096u).has_value(),
      "fenced blocks reclaim and buddy-coalesce to the full arena");

  svr::TerrainGPUV2ParameterRing ring(2u);
  svr::TerrainGPUChunkJobGPU p0{};
  svr::TerrainGPUChunkJobGPU p1{};
  p0.key.chunkX = 3;
  p1.key.chunkX = 9;
  const auto l0 = ring.acquire(p0, 0u);
  const auto l1 = ring.acquire(p1, 0u);
  check(l0 && l1 && !ring.acquire(p0, 0u) && ring.retire(*l0, 12u) && !ring.acquire(p0, 11u) && ring.acquire(p0, 12u), "an immutable job lane is never reused before its fence");
  check(ring.parameters(*l1)->key.chunkX == 9, "in-flight lane parameters retain their original job record");
}

void testPriorityAndHardLimits()
{
  std::cout << "screen priority and independent hard budgets\n";
  auto config = budget();
  config.maximumChunksPerFrame = 1u;
  config.maximumNewChunksPerFrame = 1u;
  svr::TerrainGPUV2Scheduler scheduler(config);
  initialize(scheduler);
  const uint64_t far = enqueue(scheduler, request(0, 2.0f));
  const uint64_t near = enqueue(scheduler, request(1, 40.0f));
  const auto first = scheduler.scheduleFrame({1u, 0u, 0u, 0u, 16667u, false});
  check(first.dispatches.size() == 1u && first.dispatches.front().jobId == near && first.dispatches.front().jobId != far, "larger projected screen error runs before a farther request");

  config = budget();
  config.targetGenerationMicroseconds = 300u;
  config.hardGenerationMicroseconds = 400u;
  config.maximumSingleDispatchMicroseconds = 200u;
  config.maximumChunksPerFrame = 128u;
  config.maximumNewChunksPerFrame = 128u;
  config.maximumWorkgroupsPerFrame = 17u;
  config.maximumScratchReservationBytesPerFrame = 9u * 256u;
  config.maximumOutputReservationBytesPerFrame = 7u * 256u;
  config.parameterLaneCount = 256u;
  svr::TerrainGPUV2Scheduler bounded(config);
  initialize(bounded);
  for (uint32_t index = 0u; index < 128u; ++index)
    enqueue(bounded, request(static_cast<int32_t>(index), static_cast<float>(128u - index)));
  const auto subset = bounded.scheduleFrame({1u, 0u, 0u, 0u, 16667u, false});
  check(
      !subset.dispatches.empty() && subset.dispatches.size() < 128u && subset.advancedChunks <= config.maximumChunksPerFrame && subset.newlyAdmittedChunks <= config.maximumNewChunksPerFrame &&
          subset.predictedMicroseconds <= subset.effectiveTargetMicroseconds && subset.predictedMicroseconds <= config.hardGenerationMicroseconds && subset.workgroups <= config.maximumWorkgroupsPerFrame &&
          subset.scratchBytesReserved <= config.maximumScratchReservationBytesPerFrame && subset.outputBytesReserved <= config.maximumOutputReservationBytesPerFrame,
      "128 ready chunks advance only the subset fitting time, workgroup, scratch and output budgets");
  check(subset.dispatches.size() == 7u, "the tightest independent output reservation budget wins");

  config = budget();
  config.maximumAtlasUploadBytesPerFrame = 4096u;
  svr::TerrainGPUV2Scheduler atlas(config);
  initialize(atlas);
  for (int32_t index = 0; index < 12; ++index)
    enqueue(atlas, request(index, 1.0f, svr::TerrainGPUV2PriorityClass::VisibleMissing, false));
  const auto uploads = atlas.scheduleFrame({1u, 0u, 0u, 0u, 16667u, false});
  check(uploads.atlasUploads.size() == 4u && uploads.atlasUploadBytes == 4096u && uploads.dispatches.empty(), "atlas transfer bytes independently bound admission");

  config = budget();
  config.maximumChunksPerFrame = 3u;
  config.maximumNewChunksPerFrame = 3u;
  svr::TerrainGPUV2Scheduler capped(config);
  initialize(capped);
  for (int32_t index = 0; index < 10; ++index)
    enqueue(capped, request(index, static_cast<float>(index)));
  check(capped.scheduleFrame({1u, 0u, 0u, 0u, 16667u, false}).advancedChunks == 3u, "configured maximumChunksPerFrame is an absolute cap");
}

void testFairnessAndCostFeedback()
{
  std::cout << "fair scheduling and timestamp feedback\n";
  auto config = budget();
  config.targetGenerationMicroseconds = 30u;
  config.hardGenerationMicroseconds = 40u;
  config.maximumSingleDispatchMicroseconds = 20u;
  config.fairnessReservePermille = 500u;
  svr::TerrainGPUV2Scheduler scheduler(config);
  initialize(scheduler);
  enqueue(scheduler, request(0, 20.0f));
  enqueue(scheduler, request(1, 10.0f));
  const auto frame = scheduler.scheduleFrame({1u, 0u, 0u, 0u, 16667u, false});
  check(frame.dispatches.size() == 2u && scheduler.diagnostics().secondRoundDispatches >= 1u, "a second fair round recovers unused reserved time");

  uint64_t fence = 0u;
  finishDispatches(scheduler, frame, fence, false);
  for (const auto &dispatch : frame.dispatches)
  {
    std::string error;
    check(scheduler.reportTiming(dispatch.id, 18u, &error), "delayed timestamp is accepted after dispatch completion: " + error);
  }
  const auto learned = scheduler.stageCost(svr::TerrainGPUJobStage::SampleColumns, {1u, 1u, 1u, 1u}, 0u, 1u);
  check(learned.samples == frame.dispatches.size() && learned.predictedMicroseconds >= 17u && learned.predictedMicroseconds <= 19u, "stage/shape/device cost estimate converges from delayed timestamps");

  auto pressureConfig = budget();
  pressureConfig.targetGenerationMicroseconds = 120u;
  pressureConfig.hardGenerationMicroseconds = 150u;
  pressureConfig.maximumSingleDispatchMicroseconds = 120u;
  pressureConfig.maximumWorkgroupsPerFrame = 8u;
  svr::TerrainGPUV2Scheduler pressure(pressureConfig);
  initialize(pressure);
  for (int32_t index = 0; index < 32; ++index)
    enqueue(pressure, request(index, static_cast<float>(index)));
  fence = 0u;
  bool bounded = true;
  for (uint64_t frameIndex = 1u; frameIndex <= 12u; ++frameIndex)
  {
    const auto scheduled = pressure.scheduleFrame({frameIndex, fence, 91u, 0u, 16667u, false});
    bounded &= scheduled.predictedMicroseconds <= pressureConfig.hardGenerationMicroseconds && scheduled.workgroups <= pressureConfig.maximumWorkgroupsPerFrame &&
               scheduled.advancedChunks <= pressureConfig.maximumChunksPerFrame;
    finishDispatches(pressure, scheduled, fence, true, frameIndex < 4u ? 90u : 20u);
  }
  check(
      bounded && pressure.diagnostics().timingSamples != 0u && pressure.diagnostics().rendererDrainCalls == 0u && pressure.diagnostics().waitIdleCalls == 0u,
      "wrong timing estimates adapt while every non-time hard limit holds and no renderer drain is introduced");

  auto urgentConfig = budget();
  urgentConfig.targetGenerationMicroseconds = 30u;
  urgentConfig.hardGenerationMicroseconds = 40u;
  urgentConfig.minimumUrgentGenerationMicroseconds = 20u;
  urgentConfig.maximumSingleDispatchMicroseconds = 20u;
  urgentConfig.maximumChunksPerFrame = 1u;
  urgentConfig.maximumNewChunksPerFrame = 1u;
  urgentConfig.fairnessReservePermille = 0u;
  urgentConfig.starvationAgeFrames = 2u;
  svr::TerrainGPUV2Scheduler urgent(urgentConfig);
  initialize(urgent);
  const uint64_t background = enqueue(
      urgent,
      request(200, 100.0f, svr::TerrainGPUV2PriorityClass::Background));
  const auto saturatedBackground = urgent.scheduleFrame(
      {1u, 0u, 0u, 16667u, 16667u, false});
  check(
      saturatedBackground.effectiveTargetMicroseconds == 0u &&
          saturatedBackground.dispatches.empty(),
      "saturated rendering still defers speculative background generation");
  const uint64_t visible = enqueue(
      urgent,
      request(201, 1.0f, svr::TerrainGPUV2PriorityClass::VisibleMissing));
  const auto saturatedVisible = urgent.scheduleFrame(
      {10u, 0u, 0u, 16667u, 16667u, false});
  check(
      saturatedVisible.effectiveTargetMicroseconds == 20u &&
          saturatedVisible.dispatches.size() == 1u &&
          saturatedVisible.dispatches.front().jobId == visible &&
          saturatedVisible.dispatches.front().jobId != background,
      "a fresh visible hole receives the urgent floor and preempts an aged "
      "background request");

  fence = 0u;
  finishDispatches(urgent, saturatedVisible, fence);
  bool reachedPublication = false;
  uint64_t urgentFrame = 10u;
  for (uint32_t iteration = 0u; iteration < 16u && !reachedPublication;
       ++iteration)
  {
    const auto scheduled = urgent.scheduleFrame(
        {++urgentFrame, fence, 0u, 16667u, 16667u, true});
    finishDispatches(urgent, scheduled, fence);
    if (!scheduled.publications.empty())
    {
      std::string error;
      reachedPublication = scheduled.publications.front().jobId == visible &&
          urgent.completePublication(visible, ++fence, true, &error);
    }
  }
  check(
      reachedPublication &&
          urgent.job(visible)->stage == svr::TerrainGPUJobStage::Complete,
      "visible generation reaches renderer publication under continuous "
      "foreground saturation");
}

void testCancellationAndStalePublication()
{
  std::cout << "cancellation, stale publication and fence retirement\n";
  svr::TerrainGPUV2Scheduler scheduler(budget());
  initialize(scheduler);
  const uint64_t id = enqueue(scheduler, request(0, 10.0f));
  uint64_t frame = 0u;
  uint64_t fence = 0u;
  check(driveReady(scheduler, id, frame, fence), "small chunk reaches hidden ready-to-publish state");
  const auto publication = scheduler.scheduleFrame({++frame, fence, 0u, 0u, 16667u, true});
  check(publication.publications.size() == 1u && scheduler.job(id)->stage == svr::TerrainGPUJobStage::SubmittedForPublication, "publication submission has an explicit hidden in-flight state");
  scheduler.setTerrainGeneration(7u, 12u);
  std::string error;
  check(
      scheduler.completePublication(id, ++fence, true, &error) && scheduler.job(id)->stage == svr::TerrainGPUJobStage::Cancelled && scheduler.job(id)->failure == svr::TerrainGPUV2Failure::StaleGeneration,
      "a stale generation cannot become the visible owner: " + error);

  svr::TerrainGPUV2Scheduler cancelled(budget());
  initialize(cancelled);
  const uint64_t cancelledId = enqueue(cancelled, request(3, 1.0f));
  auto dispatch = cancelled.scheduleFrame({1u, 0u, 0u, 0u, 16667u, false});
  check(dispatch.dispatches.size() == 1u && cancelled.cancel(cancelledId), "an in-flight job records cancellation without synchronously draining");
  svr::TerrainGPUV2DispatchCompletion completion{};
  completion.dispatchId = dispatch.dispatches.front().id;
  completion.completionFence = 20u;
  completion.elapsedMicroseconds = dispatch.dispatches.front().predictedMicroseconds;
  check(
      cancelled.completeDispatch(completion, &error) && cancelled.diagnostics().scratch.deferredLeases == 1u && cancelled.diagnostics().output.deferredLeases == 1u,
      "cancelled scratch and output become fence-deferred leases");
  cancelled.scheduleFrame({2u, 19u, 0u, 0u, 16667u, false});
  const bool retained = cancelled.diagnostics().scratch.reservedBytes != 0u && cancelled.diagnostics().output.reservedBytes != 0u;
  cancelled.scheduleFrame({3u, 20u, 0u, 0u, 16667u, false});
  check(
      retained && cancelled.diagnostics().scratch.reservedBytes == 0u && cancelled.diagnostics().output.reservedBytes == 0u && cancelled.diagnostics().parameterRing.inFlightLanes == 0u,
      "cancelled resources and parameter lanes reclaim only after the fence");
}

void testBatchedPageLaneAdmission()
{
  std::cout << "batched page lane admission\n";
  svr::TerrainGPUV2BatchAdmission admission{
    .maximumLanes = 4u,
    .queuedUrgentPages = 9u,
    .generationTimeBudgetMicroseconds = 1600u,
    .p95PageGenerationMicroseconds = 400u,
    .transientMemoryBudgetBytes = 40u << 20u,
    .scratchBytesPerLane = 6u << 20u,
    .hiddenOutputBytesPerLane = 4u << 20u,
  };
  check(
      svr::terrainGPUV2BatchLaneAdmission(admission) == 4u,
      "four lanes complete one ownership refinement group");
  admission.generationTimeBudgetMicroseconds = 900u;
  check(
      svr::terrainGPUV2BatchLaneAdmission(admission) == 2u,
      "p95 generation time constrains admission independently");
  admission.generationTimeBudgetMicroseconds = 1600u;
  admission.transientMemoryBudgetBytes = 19u << 20u;
  check(
      svr::terrainGPUV2BatchLaneAdmission(admission) == 1u,
      "scratch plus hidden output memory constrains admission independently");
  admission.transientMemoryBudgetBytes = 40u << 20u;
  admission.queuedUrgentPages = 3u;
  check(
      svr::terrainGPUV2BatchLaneAdmission(admission) == 3u,
      "the urgent queue depth avoids dispatching empty lanes");
  admission.queuedUrgentPages = 9u;
  admission.maximumLanes = 2u;
  check(
      svr::terrainGPUV2BatchLaneAdmission(admission) == 2u,
      "the ownership model remains the final lane cap");
}

} // namespace

int main()
{
  testScanAndCompaction();
  testFencedStorage();
  testPriorityAndHardLimits();
  testFairnessAndCostFeedback();
  testCancellationAndStalePublication();
  testBatchedPageLaneAdmission();
  std::cout << (failures == 0 ? "GPU terrain V2 Phase 2 CPU tests passed\n" : "GPU terrain V2 Phase 2 CPU tests failed\n");
  return failures == 0 ? 0 : 1;
}
