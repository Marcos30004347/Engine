#include "voxel/svraster/TerrainGPUBuildScheduler.hpp"
#include "voxel/svraster/rendering/TerrainGPUBuildSyntheticChain.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{

using namespace voxel::svraster;

static_assert(
    voxel::svraster::rendering::TerrainGPUBuildSyntheticChain::kStageCount ==
        10u);

constexpr uint64_t kBundle = 0xB17D5EEDull;

TerrainGPUBuildBudget budget()
{
  TerrainGPUBuildBudget result{};
  result.targetGPUTimeMicroseconds = 750u;
  result.admissionCeilingMicroseconds = 1250u;
  result.maximumDispatchesPerFrame = 8u;
  result.maximumPagesStartedPerFrame = 2u;
  result.maximumPagesPublishedPerFrame = 4u;
  result.maximumMapUploadBytesPerFrame = 1024u;
  result.maximumScratchBytes = 4096u;
  result.maximumInFlightJobs = 8u;
  result.starvationAgeFrames = 120u;
  return result;
}

TerrainGPUBuildRequest request(const uint32_t slot,
    const TerrainGPUJobClass jobClass = TerrainGPUJobClass::Background,
    const bool mapsReady = true, const uint64_t uploadBytes = 0u,
    const uint64_t scratchBytes = 256u)
{
  TerrainGPUBuildRequest result{};
  result.key.bundleSignature = kBundle;
  result.key.chunkX = static_cast<int32_t>(slot) - 7;
  result.key.chunkZ = static_cast<int32_t>(slot) + 3;
  result.key.internalLOD = slot % 3u;
  result.key.generatorRevision = 13u;
  result.key.terrainSlot = slot;
  result.key.expectedTerrainGeneration = 1u;
  result.jobClass = jobClass;
  result.projectedError = static_cast<float>(slot);
  result.distance = 100.0f + slot;
  result.columnUploadBytes = uploadBytes;
  result.scratchBytes = scratchBytes;
  result.semanticRevision = 1u;
  result.mapsReady = mapsReady;
  return result;
}

void initialize(TerrainGPUBuildScheduler &scheduler,
    const uint32_t firstSlot, const uint32_t count)
{
  scheduler.setBundleSignature(kBundle);
  for (uint32_t slot = firstSlot; slot < firstSlot + count; ++slot)
    scheduler.setTerrainGeneration(slot, 1u);
}

TerrainGPUFrameInput frame(const uint64_t index,
    const uint64_t fence = 0u, const bool publication = false)
{
  return {index, fence, 0u, 16667u, publication};
}

void completeAll(TerrainGPUBuildScheduler &scheduler,
    const TerrainGPUFrameSchedule &schedule, uint64_t &fence,
    const uint32_t elapsed = 50u)
{
  for (const TerrainGPUDispatch &dispatch : schedule.dispatches)
  {
    std::string error;
    assert(scheduler.completeDispatch(
        {dispatch.dispatchId, ++fence, TerrainGPUFailure::None, elapsed},
        &error));
  }
}

void driveReady(TerrainGPUBuildScheduler &scheduler,
    const uint64_t jobId, uint64_t &nextFrame, uint64_t &fence)
{
  for (uint32_t iteration = 0u; iteration < 128u; ++iteration)
  {
    const auto current = scheduler.job(jobId);
    assert(current);
    if (current->stage == TerrainGPUBuildStage::ReadyToPublish)
      return;
    const auto scheduled = scheduler.scheduleFrame(frame(nextFrame++, fence));
    completeAll(scheduler, scheduled, fence);
  }
  assert(false && "synthetic job did not reach validation");
}

void testScratchAllocator()
{
  TerrainGPUFixedScratchAllocator allocator(1024u);
  const auto a = allocator.allocate(256u);
  const auto b = allocator.allocate(256u);
  const auto c = allocator.allocate(512u);
  assert(a && b && c);
  assert(!allocator.allocate(1u));
  assert(allocator.stats().highWatermarkBytes == 1024u);
  assert(allocator.releaseDeferred(*b, 7u));
  assert(!allocator.releaseDeferred(*b, 7u));
  assert(allocator.reclaim(6u) == 0u);
  assert(allocator.reclaim(7u) == 1u);
  const auto replacement = allocator.allocate(128u);
  assert(replacement && replacement->offset == b->offset);
  assert(allocator.releaseDeferred(*a, 8u));
  assert(allocator.releaseDeferred(*c, 8u));
  assert(allocator.releaseDeferred(*replacement, 8u));
  assert(allocator.reclaim(8u) == 3u);
  const auto whole = allocator.allocate(1024u);
  assert(whole && whole->offset == 0u);
  assert(allocator.stats().invalidReleaseAttempts == 1u);
}

void testSyntheticProgressAndPublicationGate()
{
  TerrainGPUBuildScheduler scheduler(budget());
  initialize(scheduler, 1u, 1u);
  uint64_t jobId = 0u;
  std::string error;
  assert(scheduler.request(request(1u), jobId, &error));
  uint64_t nextFrame = 1u;
  uint64_t fence = 0u;
  driveReady(scheduler, jobId, nextFrame, fence);
  assert(scheduler.diagnostics().publishedPages == 0u);
  auto noPublish = scheduler.scheduleFrame(frame(nextFrame++, fence, false));
  assert(noPublish.publications.empty());
  auto publish = scheduler.scheduleFrame(frame(nextFrame++, fence, true));
  assert(publish.publications.size() == 1u);
  assert(publish.publications.front().jobId == jobId);
  assert(scheduler.finishPublication(jobId, &error));
  assert(scheduler.job(jobId)->stage == TerrainGPUBuildStage::Published);
  assert(scheduler.diagnostics().scratch.deferredAllocations == 1u);
  scheduler.scheduleFrame(frame(nextFrame++, fence));
  assert(scheduler.diagnostics().scratch.liveAllocations == 0u);
  assert(scheduler.idle());
}

void testIndependentAdmissionLimits()
{
  {
    auto limits = budget();
    limits.maximumDispatchesPerFrame = 2u;
    limits.maximumPagesStartedPerFrame = 8u;
    limits.targetGPUTimeMicroseconds = 1000u;
    TerrainGPUBuildScheduler scheduler(limits);
    initialize(scheduler, 10u, 4u);
    for (uint32_t slot = 10u; slot < 14u; ++slot)
    {
      uint64_t id = 0u;
      assert(scheduler.request(request(slot), id));
    }
    const auto scheduled = scheduler.scheduleFrame(frame(1u));
    assert(scheduled.dispatches.size() == 2u);
  }
  {
    auto limits = budget();
    limits.maximumPagesStartedPerFrame = 1u;
    TerrainGPUBuildScheduler scheduler(limits);
    initialize(scheduler, 20u, 3u);
    for (uint32_t slot = 20u; slot < 23u; ++slot)
    {
      uint64_t id = 0u;
      assert(scheduler.request(request(slot), id));
    }
    scheduler.scheduleFrame(frame(1u));
    assert(scheduler.diagnostics().scratch.liveAllocations == 1u);
  }
  {
    auto limits = budget();
    limits.maximumMapUploadBytesPerFrame = 64u;
    TerrainGPUBuildScheduler scheduler(limits);
    initialize(scheduler, 30u, 1u);
    uint64_t id = 0u;
    assert(scheduler.request(request(30u,
        TerrainGPUJobClass::Background, true, 128u), id));
    const auto first = scheduler.scheduleFrame(frame(1u));
    assert(first.uploads.size() == 1u &&
        first.uploads.front().byteCount == 64u);
    assert(first.dispatches.empty());
    const auto second = scheduler.scheduleFrame(frame(2u));
    assert(second.uploads.size() == 1u &&
        second.uploads.front().byteCount == 64u);
    assert(second.dispatches.size() == 1u);
  }
  {
    auto limits = budget();
    limits.maximumScratchBytes = 512u;
    limits.maximumPagesStartedPerFrame = 4u;
    TerrainGPUBuildScheduler scheduler(limits);
    initialize(scheduler, 40u, 2u);
    for (uint32_t slot = 40u; slot < 42u; ++slot)
    {
      uint64_t id = 0u;
      assert(scheduler.request(request(slot,
          TerrainGPUJobClass::Background, true, 0u, 512u), id));
    }
    scheduler.scheduleFrame(frame(1u));
    assert(scheduler.diagnostics().scratch.liveAllocations == 1u);
    assert(scheduler.diagnostics().scratchLimitedStarts == 1u);
  }
  {
    auto limits = budget();
    limits.maximumInFlightJobs = 1u;
    limits.maximumPagesStartedPerFrame = 4u;
    TerrainGPUBuildScheduler scheduler(limits);
    initialize(scheduler, 50u, 2u);
    for (uint32_t slot = 50u; slot < 52u; ++slot)
    {
      uint64_t id = 0u;
      assert(scheduler.request(request(slot), id));
    }
    scheduler.scheduleFrame(frame(1u));
    assert(scheduler.diagnostics().scratch.liveAllocations == 1u);
  }
  {
    auto limits = budget();
    limits.targetGPUTimeMicroseconds = 60u;
    limits.admissionCeilingMicroseconds = 60u;
    limits.maximumPagesStartedPerFrame = 4u;
    TerrainGPUBuildScheduler scheduler(limits);
    initialize(scheduler, 60u, 3u);
    for (uint32_t slot = 60u; slot < 63u; ++slot)
    {
      uint64_t id = 0u;
      assert(scheduler.request(request(slot), id));
    }
    assert(scheduler.scheduleFrame(frame(1u)).dispatches.size() == 1u);
  }
  {
    auto limits = budget();
    limits.targetGPUTimeMicroseconds = 50u;
    limits.admissionCeilingMicroseconds = 50u;
    TerrainGPUBuildScheduler scheduler(limits);
    initialize(scheduler, 70u, 1u);
    uint64_t id = 0u;
    assert(scheduler.request(request(70u), id));
    const auto reduced = scheduler.scheduleFrame(frame(1u));
    assert(reduced.dispatches.size() == 1u);
    assert(reduced.dispatches.front().estimatedMicroseconds <= 50u);
    assert(reduced.dispatches.front().workUnits < 16u);
  }
  {
    auto limits = budget();
    limits.targetGPUTimeMicroseconds = 0u;
    TerrainGPUBuildScheduler scheduler(limits);
    initialize(scheduler, 80u, 1u);
    uint64_t id = 0u;
    assert(scheduler.request(request(80u), id));
    assert(scheduler.scheduleFrame(frame(1u)).dispatches.empty());
  }
}

void testPriorityAndStableTies()
{
  auto limits = budget();
  limits.maximumDispatchesPerFrame = 1u;
  limits.maximumPagesStartedPerFrame = 8u;
  TerrainGPUBuildScheduler scheduler(limits);
  initialize(scheduler, 90u, 4u);
  std::vector<std::pair<uint64_t, TerrainGPUJobClass>> ids;
  for (const auto [slot, kind] : {
       std::pair<uint32_t, TerrainGPUJobClass>{90u, TerrainGPUJobClass::Background},
       {91u, TerrainGPUJobClass::NearFrustumPrefetch},
       {92u, TerrainGPUJobClass::VisibleRefinement},
       {93u, TerrainGPUJobClass::VisibleMissing}})
  {
    uint64_t id = 0u;
    assert(scheduler.request(request(slot, kind), id));
    ids.push_back({id, kind});
  }
  const auto scheduled = scheduler.scheduleFrame(frame(1u));
  assert(scheduled.dispatches.size() == 1u);
  assert(scheduled.dispatches.front().jobId == ids.back().first);

  TerrainGPUBuildScheduler tied(limits);
  initialize(tied, 100u, 2u);
  auto highAddress = request(101u, TerrainGPUJobClass::VisibleMissing);
  auto lowAddress = request(100u, TerrainGPUJobClass::VisibleMissing);
  highAddress.key.internalLOD = lowAddress.key.internalLOD = 1u;
  highAddress.projectedError = lowAddress.projectedError = 1.0f;
  highAddress.distance = lowAddress.distance = 1.0f;
  uint64_t highId = 0u;
  uint64_t lowId = 0u;
  assert(tied.request(highAddress, highId));
  assert(tied.request(lowAddress, lowId));
  assert(tied.scheduleFrame(frame(1u)).dispatches.front().jobId == lowId);

  TerrainGPUBuildScheduler aged(limits);
  initialize(aged, 102u, 2u);
  auto oldRequest = request(102u, TerrainGPUJobClass::NearFrustumPrefetch);
  auto newRequest = request(103u, TerrainGPUJobClass::NearFrustumPrefetch);
  oldRequest.distance = newRequest.distance = 5.0f;
  uint64_t oldId = 0u;
  uint64_t newId = 0u;
  assert(aged.request(oldRequest, oldId));
  auto pressure = frame(10u);
  pressure.foregroundGPUTimeMicroseconds = 16000u;
  assert(aged.scheduleFrame(pressure).dispatches.empty());
  assert(aged.request(newRequest, newId));
  assert(aged.scheduleFrame(frame(11u)).dispatches.front().jobId == oldId);

  TerrainGPUBuildScheduler starved(limits);
  initialize(starved, 104u, 2u);
  auto oldVisible = request(104u, TerrainGPUJobClass::VisibleRefinement);
  oldVisible.projectedError = 1.0f;
  uint64_t oldVisibleId = 0u;
  assert(starved.request(oldVisible, oldVisibleId));
  pressure.frameIndex = 200u;
  assert(starved.scheduleFrame(pressure).dispatches.empty());
  auto newVisible = request(105u, TerrainGPUJobClass::VisibleRefinement);
  newVisible.projectedError = 100.0f;
  uint64_t newVisibleId = 0u;
  assert(starved.request(newVisible, newVisibleId));
  assert(starved.scheduleFrame(frame(201u)).dispatches.front().jobId ==
      oldVisibleId);
}

void testPublicationAndCancellationLifecycle()
{
  auto limits = budget();
  limits.maximumPagesPublishedPerFrame = 1u;
  limits.maximumPagesStartedPerFrame = 4u;
  TerrainGPUBuildScheduler scheduler(limits);
  initialize(scheduler, 130u, 6u);
  uint64_t first = 0u;
  uint64_t second = 0u;
  assert(scheduler.request(request(130u), first));
  assert(scheduler.request(request(131u), second));
  uint64_t nextFrame = 1u;
  uint64_t fence = 0u;
  driveReady(scheduler, first, nextFrame, fence);
  driveReady(scheduler, second, nextFrame, fence);
  assert(scheduler.scheduleFrame(
      frame(nextFrame++, fence, true)).publications.size() == 1u);

  uint64_t waiting = 0u;
  assert(scheduler.request(request(132u,
      TerrainGPUJobClass::Background, false), waiting));
  assert(scheduler.cancel(waiting));
  assert(!scheduler.cancel(waiting));

  uint64_t uploading = 0u;
  assert(scheduler.request(request(133u,
      TerrainGPUJobClass::Background, true, 2048u), uploading));
  scheduler.scheduleFrame(frame(nextFrame++, fence));
  assert(scheduler.job(uploading)->stage ==
      TerrainGPUBuildStage::UploadColumns);
  assert(scheduler.cancel(uploading));

  uint64_t active = 0u;
  assert(scheduler.request(request(134u), active));
  const auto activeFrame = scheduler.scheduleFrame(
      frame(nextFrame++, fence));
  const auto activeDispatch = std::find_if(activeFrame.dispatches.begin(),
      activeFrame.dispatches.end(), [&](const TerrainGPUDispatch &value)
      { return value.jobId == active; });
  assert(activeDispatch != activeFrame.dispatches.end());
  assert(scheduler.cancel(active));
  assert(scheduler.completeDispatch({activeDispatch->dispatchId, ++fence,
      TerrainGPUFailure::None, 10u}));
  assert(scheduler.job(active)->stage == TerrainGPUBuildStage::Cancelled);

  assert(scheduler.cancel(first));
  assert(scheduler.cancel(second));
  // Phase 5 keeps a submitted publication alive until its CAS fence, even
  // when cancellation wins while it is in flight.
  assert(scheduler.completePublication(
      {first, ++fence, false}));
  scheduler.scheduleFrame(frame(nextFrame++,
      std::numeric_limits<uint64_t>::max()));
  const auto diagnostics = scheduler.diagnostics();
  assert(diagnostics.scratch.liveAllocations == 0u);
  assert(diagnostics.scratch.invalidReleaseAttempts == 0u);
}

void testStaleCancellationFailureAndTiming()
{
  auto limits = budget();
  limits.maximumPagesStartedPerFrame = 8u;
  TerrainGPUBuildScheduler scheduler(limits);
  initialize(scheduler, 110u, 5u);

  uint64_t waiting = 0u;
  assert(scheduler.request(request(110u,
      TerrainGPUJobClass::Background, false), waiting));
  scheduler.setTerrainGeneration(110u, 2u);
  scheduler.scheduleFrame(frame(1u));
  assert(scheduler.job(waiting)->stage == TerrainGPUBuildStage::Cancelled);
  assert(scheduler.diagnostics().staleQueuedJobs == 1u);

  uint64_t active = 0u;
  assert(scheduler.request(request(111u), active));
  auto activeFrame = scheduler.scheduleFrame(frame(2u));
  const auto activeDispatch = *std::find_if(activeFrame.dispatches.begin(),
      activeFrame.dispatches.end(), [&](const TerrainGPUDispatch &value)
      { return value.jobId == active; });
  scheduler.setTerrainGeneration(111u, 2u);
  scheduler.scheduleFrame(frame(3u));
  assert(scheduler.completeDispatch({activeDispatch.dispatchId, 3u,
      TerrainGPUFailure::None, 4000u}));
  assert(scheduler.job(active)->stage == TerrainGPUBuildStage::Cancelled);
  assert(scheduler.diagnostics().gpuDebtMicroseconds != 0u);

  uint64_t failed = 0u;
  assert(scheduler.request(request(112u), failed));
  auto failedFrame = scheduler.scheduleFrame(frame(4u, 3u));
  auto failedDispatch = std::find_if(failedFrame.dispatches.begin(),
      failedFrame.dispatches.end(), [&](const TerrainGPUDispatch &value)
      { return value.jobId == failed; });
  // The underestimated prior quantum creates debt, so pressure admission is
  // suppressed until a later zero-cost timing sample clears it.
  if (failedDispatch == failedFrame.dispatches.end())
  {
    for (uint64_t debtFrame = 5u;
         debtFrame < 12u && failedDispatch == failedFrame.dispatches.end();
         ++debtFrame)
    {
      failedFrame = scheduler.scheduleFrame(frame(debtFrame, 3u));
      failedDispatch = std::find_if(failedFrame.dispatches.begin(),
          failedFrame.dispatches.end(), [&](const TerrainGPUDispatch &value)
          { return value.jobId == failed; });
    }
  }
  assert(failedDispatch != failedFrame.dispatches.end());
  assert(scheduler.completeDispatch({failedDispatch->dispatchId, 5u,
      TerrainGPUFailure::CounterOverflow, 10u}));
  assert(scheduler.job(failed)->stage == TerrainGPUBuildStage::Failed);
  assert(scheduler.result(failed)->status ==
      TerrainGPUFailure::CounterOverflow);
  assert(scheduler.result(failed)->semanticRevision == 1u);

  TerrainGPUBuildScheduler delayed(limits);
  initialize(delayed, 120u, 1u);
  uint64_t delayedId = 0u;
  assert(delayed.request(request(120u), delayedId));
  const auto delayedFrame = delayed.scheduleFrame(frame(1u));
  assert(delayed.completeDispatch({delayedFrame.dispatches.front().dispatchId,
      1u, TerrainGPUFailure::None, std::nullopt}));
  assert(delayed.diagnostics().delayedTimingSamples == 1u);
  assert(delayed.reportTiming(delayedFrame.dispatches.front().dispatchId, 77u));
  assert(delayed.stageTiming(TerrainGPUBuildStage::SampleColumns).sampleCount == 1u);
}

void testPressureAndRandomizedLifecycle()
{
  auto limits = budget();
  limits.maximumPagesStartedPerFrame = 4u;
  limits.maximumScratchBytes = 2048u;
  limits.maximumInFlightJobs = 4u;
  TerrainGPUBuildScheduler scheduler(limits);
  initialize(scheduler, 200u, 32u);
  std::vector<uint64_t> jobs;
  for (uint32_t slot = 200u; slot < 216u; ++slot)
  {
    uint64_t id = 0u;
    assert(scheduler.request(request(slot), id));
    jobs.push_back(id);
  }
  auto pressured = frame(1u);
  pressured.foregroundGPUTimeMicroseconds = 16000u;
  assert(scheduler.scheduleFrame(pressured).dispatches.empty());

  std::mt19937 random(0xF2A5E2u);
  uint64_t fence = 0u;
  for (uint64_t frameIndex = 2u; frameIndex < 300u; ++frameIndex)
  {
    if ((random() & 7u) == 0u)
      scheduler.cancel(jobs[random() % jobs.size()]);
    if ((random() & 31u) == 0u)
    {
      const uint32_t slot = 200u + random() % 16u;
      scheduler.setTerrainGeneration(slot, 2u);
    }
    auto input = frame(frameIndex, fence);
    if ((random() & 15u) == 0u)
      input.foregroundGPUTimeMicroseconds = 16000u;
    const auto scheduled = scheduler.scheduleFrame(input);
    assert(scheduled.dispatches.size() <=
        limits.maximumDispatchesPerFrame);
    assert(scheduled.uploads.size() <= limits.maximumInFlightJobs);
    for (const TerrainGPUDispatch &dispatch : scheduled.dispatches)
    {
      const bool inject = (random() & 63u) == 0u;
      const bool delayed = (random() & 7u) == 0u;
      assert(scheduler.completeDispatch({dispatch.dispatchId, ++fence,
          inject ? TerrainGPUFailure::Injected : TerrainGPUFailure::None,
          delayed ? std::optional<uint32_t>{}
                  : std::optional<uint32_t>{20u + random() % 100u}}));
      if (delayed)
        assert(scheduler.reportTiming(dispatch.dispatchId,
            20u + random() % 100u));
    }
    const auto diagnostics = scheduler.diagnostics();
    assert(diagnostics.scratch.allocatedBytes <=
        limits.maximumScratchBytes);
    assert(diagnostics.scratch.liveAllocations -
        diagnostics.scratch.deferredAllocations <=
        limits.maximumInFlightJobs);
    assert(diagnostics.scratch.invalidReleaseAttempts == 0u);
  }
  scheduler.scheduleFrame(frame(301u,
      std::numeric_limits<uint64_t>::max()));
  assert(scheduler.diagnostics().scratch.invalidReleaseAttempts == 0u);
}

void testPermanentlyMissingTimingIsBounded()
{
  auto limits = budget();
  limits.maximumInFlightJobs = 4u;
  limits.maximumPagesStartedPerFrame = 4u;
  TerrainGPUBuildScheduler scheduler(limits);
  initialize(scheduler, 240u, 4u);
  for (uint32_t slot = 240u; slot < 244u; ++slot)
  {
    uint64_t id = 0u;
    assert(scheduler.request(request(slot), id));
  }
  uint64_t fence = 0u;
  for (uint64_t frameIndex = 1u; frameIndex < 80u; ++frameIndex)
  {
    const auto scheduled = scheduler.scheduleFrame(frame(frameIndex, fence));
    for (const auto &dispatch : scheduled.dispatches)
      assert(scheduler.completeDispatch({dispatch.dispatchId, ++fence,
          TerrainGPUFailure::None, std::nullopt}));
  }
  assert(scheduler.diagnostics().droppedTimingSamples != 0u);
  assert(scheduler.diagnostics().scratch.invalidReleaseAttempts == 0u);
}

} // namespace

int main()
{
  testScratchAllocator();
  testSyntheticProgressAndPublicationGate();
  testIndependentAdmissionLimits();
  testPriorityAndStableTies();
  testPublicationAndCancellationLifecycle();
  testStaleCancellationFailureAndTiming();
  testPressureAndRandomizedLifecycle();
  testPermanentlyMissingTimingIsBounded();
  std::cout << "GPU terrain Phase 2 scheduler tests passed\n";
  return 0;
}
