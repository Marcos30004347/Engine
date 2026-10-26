#include "voxel/svraster/TerrainGPUV2Runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
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

svr::TerrainGPUV2DeviceCapabilities device(const bool timestamps = true)
{
  svr::TerrainGPUV2DeviceCapabilities value{};
  value.profile = 0x10203040u;
  value.deviceLocalMemoryBytes = 8ull << 30u;
  value.maximumStorageBufferBytes = 1ull << 30u;
  value.maximumStorageBuffersPerStage = 32u;
  value.maximumComputeWorkGroupInvocations = 1024u;
  value.compute = true;
  value.timestampQueries = timestamps;
  value.qualified = true;
  return value;
}

svr::TerrainGPUV2RuntimeSettings settings(const uint32_t chunkCap = 128u)
{
  svr::TerrainGPUV2RuntimeSettings value{};
  value.scheduler.targetGenerationMicroseconds = 10000u;
  value.scheduler.hardGenerationMicroseconds = 12000u;
  value.scheduler.maximumSingleDispatchMicroseconds = 1000u;
  value.scheduler.maximumChunksPerFrame = chunkCap;
  value.scheduler.maximumNewChunksPerFrame = chunkCap;
  value.scheduler.maximumPublicationsPerFrame = 128u;
  value.scheduler.maximumWorkgroupsPerFrame = 65536u;
  value.scheduler.scratchCapacityBytes = 1u << 20u;
  value.scheduler.outputCapacityBytes = 1u << 20u;
  value.scheduler.maximumScratchReservationBytesPerFrame = 1u << 20u;
  value.scheduler.maximumOutputReservationBytesPerFrame = 1u << 20u;
  value.scheduler.minimumAllocationClassBytes = 256u;
  value.scheduler.parameterLaneCount = 256u;
  value.scheduler.fairnessReservePermille = 0u;
  value.residency = {1u << 20u, 768u << 10u, 896u << 10u, 256u, 4096u};
  value.maximumConsecutiveBuildFailures = 2u;
  value.maximumConsecutiveBudgetViolations = 3u;
  return value;
}

svr::TerrainGPUV2View view(const double x = 0.0)
{
  svr::TerrainGPUV2View value{};
  value.cameraMetres = {x, 0.5, 0.5};
  value.viewportHeightPixels = 1080u;
  return value;
}

svr::TerrainGPUV2SpatialRequest chunk(const int32_t x, const uint32_t slot, const svr::TerrainGPUV2Visibility visibility = svr::TerrainGPUV2Visibility::VisibleMissing)
{
  svr::TerrainGPUV2SpatialRequest value{};
  value.key = {51u, x, 0, 0, 0u, 9u, slot, 7u};
  value.shape = {1u, 1u, 1u, 1u};
  value.visibility = visibility;
  value.voxelSizeMetres = 1.0;
  value.geometricErrorMetres = 1.0;
  value.scratchBytes = 256u;
  value.outputReservationBytes = 256u;
  value.atlasResident = true;
  return value;
}

void initialize(svr::TerrainGPUV2Runtime &runtime, const uint32_t cap = 128u, const bool timestamps = true)
{
  std::string error;
  check(runtime.initialize(device(timestamps), settings(cap), &error), "runtime initializes: " + error);
  runtime.setWorldBundleRevision(51u);
}

struct DriveResult
{
  std::vector<svr::TerrainGPUChunkKey> keys{};
  uint32_t firstFrameDispatches = 0u;
  uint32_t maximumAdvanced = 0u;
  uint32_t frames = 0u;
  uint32_t nearestLOD0LatencyFrames = 0u;
  uint32_t measuredP99Microseconds = 0u;
  uint64_t acceptedPublications = 0u;
  bool asynchronous = true;
};

DriveResult drive(const uint32_t cap, const uint32_t count)
{
  svr::TerrainGPUV2Runtime runtime;
  initialize(runtime, cap);
  svr::TerrainGPUV2RegionRequest region{};
  region.chunk = chunk(0, 0u);
  region.minimumChunk = {0, 0, 0};
  region.chunkCounts = {count, 1u, 1u};
  region.terrainSlotBase = 100u;
  std::vector<uint64_t> ids;
  std::string error;
  check(runtime.requestRegion(region, view(), ids, &error) && ids.size() == count, "region decomposes for cap " + std::to_string(cap) + ": " + error);
  uint64_t fence = 0u;
  DriveResult result{};
  for (uint64_t frame = 1u; frame <= 256u && runtime.completedKeys().size() != count; ++frame)
  {
    const auto scheduled = runtime.beginFrame({frame, fence, 0u, 16667u, view(), true});
    if (frame == 1u)
      result.firstFrameDispatches = static_cast<uint32_t>(scheduled.dispatches.size());
    result.maximumAdvanced = std::max(result.maximumAdvanced, scheduled.advancedChunks);
    for (const auto &upload : scheduled.atlasUploads)
      result.asynchronous &= runtime.completeAtlasUpload(upload.jobId, true, &error);
    for (const auto &dispatch : scheduled.dispatches)
    {
      svr::TerrainGPUV2DispatchCompletion completion{};
      completion.dispatchId = dispatch.id;
      completion.completionFence = ++fence;
      completion.elapsedMicroseconds = dispatch.predictedMicroseconds;
      result.asynchronous &= runtime.completeDispatch(completion, &error);
    }
    for (const auto &publication : scheduled.publications)
      result.asynchronous &= runtime.completePublication(publication.jobId, ++fence, true, &error);
    result.frames = static_cast<uint32_t>(frame);
  }
  result.keys = runtime.completedKeys();
  result.nearestLOD0LatencyFrames = runtime.telemetry().nearestLOD0LatencyFrames;
  result.measuredP99Microseconds = runtime.telemetry().measuredP99Microseconds;
  result.acceptedPublications = runtime.telemetry().acceptedPublications;
  check(result.keys.size() == count, "all decomposed chunks eventually publish for cap " + std::to_string(cap));
  check(result.asynchronous && runtime.telemetry().rendererDrainCalls == 0u && runtime.telemetry().waitIdleCalls == 0u, "cap " + std::to_string(cap) + " completes asynchronously without renderer drains: " + error);
  return result;
}

void testPriorityAndRegionDecomposition()
{
  std::cout << "screen-space priority and 3D decomposition\n";
  svr::TerrainGPUV2Runtime runtime;
  initialize(runtime, 1u);
  uint64_t far = 0u;
  uint64_t near = 0u;
  uint64_t offscreen = 0u;
  std::string error;
  check(runtime.requestChunk(chunk(40, 1u), view(), far, &error), "far request is accepted: " + error);
  auto nearRequest = chunk(0, 2u, svr::TerrainGPUV2Visibility::Offscreen);
  check(runtime.requestChunk(nearRequest, view(), near, &error), "near prefetch request is accepted: " + error);
  nearRequest.visibility = svr::TerrainGPUV2Visibility::VisibleMissing;
  uint64_t reprioritized = 0u;
  check(runtime.requestChunk(nearRequest, view(), reprioritized, &error) && reprioritized == near, "an existing chunk is reprioritized in place when it becomes visible: " + error);
  check(runtime.requestChunk(chunk(-1, 3u, svr::TerrainGPUV2Visibility::Offscreen), view(), offscreen, &error), "offscreen request is accepted: " + error);
  const auto scheduled = runtime.beginFrame({1u, 0u, 0u, 16667u, view(), false});
  check(
      scheduled.dispatches.size() == 1u && scheduled.dispatches[0].jobId == near && scheduled.dispatches[0].jobId != far && scheduled.dispatches[0].jobId != offscreen,
      "near visible chunk has the largest error-per-pixel and wins the one-chunk budget");
  const auto telemetry = runtime.telemetry();
  check(
      telemetry.nearestVisibleQueueRank == 1u && telemetry.nearestVisibleErrorPixels > 0.0f && telemetry.rendererDrainCalls == 0u && telemetry.waitIdleCalls == 0u,
      "nearest request priority and zero synchronization drains are exposed in telemetry");

  svr::TerrainGPUV2Runtime stacked;
  initialize(stacked);
  svr::TerrainGPUV2RegionRequest region{};
  region.chunk = chunk(0, 0u);
  region.minimumChunk = {-2, -1, 3};
  region.chunkCounts = {4u, 3u, 2u};
  region.terrainSlotBase = 20u;
  std::vector<uint64_t> ids;
  check(stacked.requestRegion(region, view(), ids, &error) && ids.size() == 24u, "one large request becomes 24 independently prioritized 3D chunks: " + error);
  std::set<uint64_t> unique(ids.begin(), ids.end());
  check(unique.size() == ids.size() && stacked.telemetry().maximumVerticalStack == 3u, "vertical chunk stacks are explicit and job identities remain unique");
}

void testIncrementalSchedulerPriorityQueueBounds()
{
  std::cout << "incremental scheduler priority queue bounds\n";
  auto boundedSettings = settings(128u);
  boundedSettings.scheduler.maximumBackgroundPriorityCandidatesPerFrame = 4u;
  svr::TerrainGPUV2Runtime runtime;
  std::string error;
  check(runtime.initialize(device(), boundedSettings, &error), "bounded-priority runtime initializes: " + error);
  runtime.setWorldBundleRevision(51u);

  uint64_t visible = 0u;
  check(runtime.requestChunk(chunk(0, 1u, svr::TerrainGPUV2Visibility::VisibleMissing), view(), visible, &error), "visible request queues: " + error);
  for (uint32_t i = 0u; i < 32u; ++i)
  {
    uint64_t background = 0u;
    check(
        runtime.requestChunk(
            chunk(static_cast<int32_t>(100u + i), 100u + i, svr::TerrainGPUV2Visibility::Offscreen),
            view(),
            background,
            &error),
        "background request queues: " + error);
  }

  const auto scheduled = runtime.beginFrame({1u, 0u, 0u, 16667u, view(), false});
  const auto scheduler = runtime.telemetry().scheduler;
  check(
      scheduled.priorityCandidatesVisited <=
              boundedSettings.scheduler.maximumBackgroundPriorityCandidatesPerFrame + 2u &&
          scheduled.priorityCandidatesSkipped != 0u &&
          scheduler.priorityCandidatesSkipped != 0u,
      "background priority selection is CPU-bounded and leaves the tail for a later frame");
  check(
      !scheduled.dispatches.empty() &&
          scheduled.dispatches.front().jobId == visible &&
          scheduler.priorityQueueEntries == 33u &&
          scheduler.fullPrioritySorts == 0u,
      "visible work still wins while the old full CPU sort path remains unused");
}

void testChunkCapsAndAdaptiveBudgets()
{
  std::cout << "chunk caps and adaptive budgets\n";
  auto one = drive(1u, 16u);
  auto eight = drive(8u, 16u);
  auto many = drive(128u, 16u);
  const auto order = [](std::vector<svr::TerrainGPUChunkKey> keys)
  {
    std::sort(keys.begin(), keys.end());
    return keys;
  };
  check(order(one.keys) == order(eight.keys) && order(one.keys) == order(many.keys), "caps 1, 8, and 128 produce identical final terrain ownership");
  check(one.maximumAdvanced == 1u && eight.maximumAdvanced <= 8u && many.maximumAdvanced <= 128u && one.frames > eight.frames && eight.frames >= many.frames, "chunk caps change only throughput and latency");
  check(many.firstFrameDispatches == 16u, "multi-job runtime submits all 16 fitting chunks without serializing on one page");
  check(many.acceptedPublications == 16u, "all independently built chunks cross the generation-checked publication boundary");
  check(
      many.measuredP99Microseconds <= settings().scheduler.hardGenerationMicroseconds && many.nearestLOD0LatencyFrames <= 10u,
      "generation p99 stays below the hard ceiling and nearest LOD0 refines within the latency target");

  auto tightSettings = settings(128u);
  tightSettings.scheduler.maximumWorkgroupsPerFrame = 3u;
  svr::TerrainGPUV2Runtime workgroupBound;
  std::string error;
  check(workgroupBound.initialize(device(), tightSettings, &error), "tight runtime initializes: " + error);
  workgroupBound.setWorldBundleRevision(51u);
  svr::TerrainGPUV2RegionRequest tightRegion{};
  tightRegion.chunk = chunk(0, 0u);
  tightRegion.chunkCounts = {16u, 1u, 1u};
  tightRegion.terrainSlotBase = 300u;
  std::vector<uint64_t> tightIds;
  check(workgroupBound.requestRegion(tightRegion, view(), tightIds, &error), "tight workgroup region queues: " + error);
  const auto tightFrame = workgroupBound.beginFrame({1u, 0u, 0u, 16667u, view(), false});
  check(tightFrame.advancedChunks == 3u && tightFrame.advancedChunks < 128u && tightFrame.workgroups == 3u, "the workgroup budget advances fewer than the configured 128-chunk cap");

  svr::TerrainGPUV2Runtime foregroundBound;
  check(foregroundBound.initialize(device(), settings(128u), &error), "foreground-bound runtime initializes: " + error);
  foregroundBound.setWorldBundleRevision(51u);
  std::vector<uint64_t> foregroundIds;
  check(foregroundBound.requestRegion(tightRegion, view(), foregroundIds, &error), "foreground-bound region queues: " + error);
  const auto foregroundFrame = foregroundBound.beginFrame({1u, 0u, 16000u, 16667u, view(), false});
  check(
      foregroundFrame.effectiveTargetMicroseconds ==
              settings().scheduler.minimumUrgentGenerationMicroseconds &&
          !foregroundFrame.dispatches.empty() &&
          foregroundFrame.predictedMicroseconds <=
              settings().scheduler.minimumUrgentGenerationMicroseconds,
      "foreground pressure retains a hard-bounded visible-generation floor");

  svr::TerrainGPUV2Runtime conservative;
  initialize(conservative, 128u, false);
  check(
      conservative.effectiveBudget().maximumChunksPerFrame == 8u && conservative.effectiveBudget().targetGenerationMicroseconds == 500u && conservative.effectiveBudget().hardGenerationMicroseconds == 1000u &&
          conservative.telemetry().conservativeTiming,
      "devices without timestamps use the conservative 8-chunk/0.5 ms profile");
  uint64_t id = 0u;
  check(conservative.requestChunk(chunk(0, 1u), view(), id, &error), "conservative request is accepted: " + error);
  const auto scheduled = conservative.beginFrame({1u, 0u, 0u, 16667u, view(), false});
  check(!scheduled.dispatches.empty(), "conservative profile still advances bounded GPU work");
  for (const auto &dispatch : scheduled.dispatches)
    check(conservative.completeDispatch({dispatch.id, 1u, svr::TerrainGPUV2Failure::None, std::nullopt}, &error), "no-timestamp completion retires via its conservative estimate: " + error);
  check(conservative.telemetry().scheduler.delayedTimingSamples == 0u, "no-timestamp mode leaves no delayed timestamp records behind");
}

void testDelayedTimestampsTelemetryAndQualification()
{
  std::cout << "timestamps, telemetry and qualification matrix\n";
  svr::TerrainGPUV2Runtime runtime;
  initialize(runtime);
  uint64_t id = 0u;
  std::string error;
  check(runtime.requestChunk(chunk(0, 1u), view(), id, &error), "timestamp request is accepted: " + error);
  const auto scheduled = runtime.beginFrame({1u, 0u, 0u, 16667u, view(), false});
  check(scheduled.dispatches.size() == 1u, "one timestamped dispatch is submitted");
  const auto dispatch = scheduled.dispatches[0];
  check(runtime.completeDispatch({dispatch.id, 1u, svr::TerrainGPUV2Failure::None, std::nullopt}, &error), "dispatch completion does not wait for its delayed timestamp: " + error);
  check(runtime.reportTimestamp(dispatch.id, 100u, &error), "delayed timestamp updates the stage cost model asynchronously: " + error);
  svr::TerrainGPUV2WorldMapAtlasDiagnostics atlas{};
  atlas.readyTiles = 7u;
  atlas.waitingTiles = 2u;
  atlas.uploadedBytes = 4096u;
  atlas.reportedFaults = 3u;
  runtime.setAtlasDiagnostics(atlas);
  runtime.recordFramePerformance(10500u, 10000u);
  for (uint32_t moving = 0u; moving < 2u; ++moving)
    for (uint32_t upscale = 0u; upscale < 2u; ++upscale)
      for (uint32_t lighting = 0u; lighting < 2u; ++lighting)
        runtime.recordCapture({moving != 0u, upscale != 0u, lighting != 0u, true, true});
  const auto telemetry = runtime.telemetry();
  check(
      telemetry.measuredP99Microseconds == 100u && telemetry.sourceFrameP95PermilleOfReference == 1050u && telemetry.captureMatrixMask == 0xffu && telemetry.rawRasterStable && telemetry.lightingStable &&
          telemetry.atlas.readyTiles == 7u && telemetry.atlas.reportedFaults == 3u,
      "p99 generation, atlas state, <10% frame regression, and the raw-before-lighting capture matrix are recorded");
  check(!runtime.qualificationReady(), "timing and capture presence cannot bypass Phase 9 visual qualification");
  svr::TerrainGPUV2StabilityReport visual{};
  visual.qualified = true;
  runtime.recordVisualQualification(visual);
  check(runtime.qualificationReady(), "runtime qualification passes only after timing, performance, and all eight capture combinations");
  const auto overlay = runtime.overlayText();
  check(
      overlay.find("Terrain GPU V2: gpu") != std::string::npos && overlay.find("budget_us=") != std::string::npos && overlay.find("faults=3") != std::string::npos && overlay.find("visual=pass") != std::string::npos &&
          overlay.find("drains=0/0") != std::string::npos,
      "overlay exposes mode, budgets, priority, residency, atlas, and synchronization state");
}

void testFallbacks()
{
  std::cout << "qualification, failure and device-loss fallback\n";
  auto unsupported = device();
  unsupported.compute = false;
  svr::TerrainGPUV2Runtime CPU;
  std::string error;
  check(
      CPU.initialize(unsupported, settings(), &error) && CPU.mode() == svr::TerrainGPUV2RuntimeMode::CPUFallback && CPU.fallbackReason() == svr::TerrainGPUV2RuntimeFallback::MissingCompute,
      "missing compute capability fails directly to the CPU column/build path: " + error);
  CPU.setWorldBundleRevision(51u);
  uint64_t id = 0u;
  check(CPU.requestChunk(chunk(0, 1u), view(), id, &error) && id == 0u && CPU.takeCPUFallbackRequests().size() == 1u, "unqualified work queues only a compact CPU rebuild request");

  auto memoryConstrained = device();
  memoryConstrained.deviceLocalMemoryBytes = 2u << 20u;
  svr::TerrainGPUV2Runtime constrained;
  check(
      constrained.initialize(memoryConstrained, settings(), &error) && constrained.mode() == svr::TerrainGPUV2RuntimeMode::CPUFallback &&
          constrained.fallbackReason() == svr::TerrainGPUV2RuntimeFallback::InsufficientLimits,
      "device qualification budgets residency, scratch, and output arenas together: " + error);

  svr::TerrainGPUV2Runtime lost;
  initialize(lost);
  check(lost.requestChunk(chunk(0, 1u), view(), id, &error), "device-loss fixture queues GPU work: " + error);
  lost.recordDeviceLoss();
  const auto fallback = lost.takeCPUFallbackRequests();
  check(
      lost.mode() == svr::TerrainGPUV2RuntimeMode::CPUFallback && lost.fallbackReason() == svr::TerrainGPUV2RuntimeFallback::DeviceLost && fallback.size() == 1u && fallback[0].key == chunk(0, 1u).key,
      "device loss cancels GPU admission and preserves the exact generation key for CPU rebuild");

  svr::TerrainGPUV2Runtime failuresRuntime;
  initialize(failuresRuntime);
  uint64_t fence = 0u;
  for (uint32_t failureIndex = 0u; failureIndex < 2u; ++failureIndex)
  {
    check(failuresRuntime.requestChunk(chunk(static_cast<int32_t>(failureIndex), 10u + failureIndex), view(), id, &error), "failure fixture queues a chunk: " + error);
    const auto frame = failuresRuntime.beginFrame({failureIndex + 1u, fence, 0u, 16667u, view(), false});
    check(!frame.dispatches.empty(), "failure fixture submits a dispatch");
    const auto dispatch = frame.dispatches.front();
    check(failuresRuntime.completeDispatch({dispatch.id, ++fence, svr::TerrainGPUV2Failure::DispatchFailure, dispatch.predictedMicroseconds}, &error), "failed dispatch completes and releases asynchronously: " + error);
  }
  check(
      failuresRuntime.mode() == svr::TerrainGPUV2RuntimeMode::CPUFallback && failuresRuntime.fallbackReason() == svr::TerrainGPUV2RuntimeFallback::RepeatedBuildFailure &&
          failuresRuntime.takeCPUFallbackRequests().size() == 2u,
      "repeated build failures switch the session to compact CPU rebuild requests");
}
} // namespace

int main()
{
  std::cout << "Sparse voxel terrain GPU V2 Phase 8 tests\n";
  testPriorityAndRegionDecomposition();
  testIncrementalSchedulerPriorityQueueBounds();
  testChunkCapsAndAdaptiveBudgets();
  testDelayedTimestampsTelemetryAndQualification();
  testFallbacks();
  if (failures != 0)
    std::cerr << failures << " Phase 8 checks failed\n";
  return failures == 0 ? 0 : 1;
}
