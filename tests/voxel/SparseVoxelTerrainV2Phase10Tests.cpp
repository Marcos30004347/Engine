#include "voxel/svraster/TerrainGPUV2Rollout.hpp"

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
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message
            << '\n';
  failures += condition ? 0 : 1;
}

svr::TerrainGPUV2DeviceCapabilities qualifiedDevice()
{
  svr::TerrainGPUV2DeviceCapabilities value{};
  value.profile = 0xaabbccddu;
  value.deviceLocalMemoryBytes = 8ull << 30u;
  value.maximumStorageBufferBytes = 1ull << 30u;
  value.maximumStorageBuffersPerStage = 32u;
  value.maximumComputeWorkGroupInvocations = 1024u;
  value.compute = true;
  value.timestampQueries = true;
  value.qualified = true;
  return value;
}

svr::TerrainGPUV2RolloutSettings rolloutSettings(
    const svr::TerrainGPUV2RequestedMode mode =
        svr::kDefaultTerrainGPUV2RequestedMode)
{
  svr::TerrainGPUV2RolloutSettings value{};
  value.requestedMode = mode;
  value.pipelinesCreated = true;
  value.visualQualificationAvailable = true;
  value.visualQualification.qualified = true;
  value.runtime.scheduler.targetGenerationMicroseconds = 12000u;
  value.runtime.scheduler.hardGenerationMicroseconds = 16000u;
  value.runtime.scheduler.maximumSingleDispatchMicroseconds = 2000u;
  value.runtime.scheduler.maximumChunksPerFrame = 128u;
  value.runtime.scheduler.maximumNewChunksPerFrame = 128u;
  value.runtime.scheduler.maximumWorkgroupsPerFrame = 65536u;
  value.runtime.scheduler.scratchCapacityBytes = 8u << 20u;
  value.runtime.scheduler.outputCapacityBytes = 16u << 20u;
  value.runtime.scheduler.maximumScratchReservationBytesPerFrame = 8u << 20u;
  value.runtime.scheduler.maximumOutputReservationBytesPerFrame = 16u << 20u;
  value.runtime.scheduler.parameterLaneCount = 512u;
  value.runtime.residency = {
      64u << 20u, 48u << 20u, 56u << 20u, 256u, 4096u};
  value.runtime.maximumConsecutiveBuildFailures = 3u;
  value.runtime.maximumConsecutiveBudgetViolations = 4u;
  return value;
}

svr::TerrainGPUV2View view(const double x = 0.0)
{
  svr::TerrainGPUV2View result{};
  result.cameraMetres = {x, 1.0, 0.0};
  result.viewportHeightPixels = 1080u;
  return result;
}

svr::TerrainGPUV2SpatialRequest request(
    const int32_t x, const uint32_t slot, const uint32_t generation = 1u,
    const bool atlasResident = true)
{
  svr::TerrainGPUV2SpatialRequest result{};
  result.key = {9001u, x, 0, 0, 0u, 17u, slot, generation};
  result.shape = {1u, 1u, 1u, 1u};
  result.visibility = svr::TerrainGPUV2Visibility::VisibleMissing;
  result.voxelSizeMetres = 1.0;
  result.geometricErrorMetres = 1.0;
  result.scratchBytes = 256u;
  result.outputReservationBytes = 256u;
  result.atlasUploadBytes = atlasResident ? 0u : 4096u;
  result.atlasResident = atlasResident;
  return result;
}

bool drive(
    svr::TerrainGPUV2Runtime &runtime, const uint32_t expected,
    uint64_t &fence, const uint32_t maximumFrames = 1024u)
{
  std::string error;
  for (uint64_t frame = 1u;
       frame <= maximumFrames && runtime.completedKeys().size() < expected;
       ++frame)
  {
    const auto schedule = runtime.beginFrame(
        {frame, fence, 0u, 16667u, view(), true});
    for (const auto &upload : schedule.atlasUploads)
      if (!runtime.completeAtlasUpload(upload.jobId, true, &error))
        return false;
    for (const auto &dispatch : schedule.dispatches)
      if (!runtime.completeDispatch(
              {dispatch.id, ++fence, svr::TerrainGPUV2Failure::None,
               dispatch.predictedMicroseconds},
              &error))
        return false;
    for (const auto &publication : schedule.publications)
      if (!runtime.completePublication(
              publication.jobId, ++fence, true, &error))
        return false;
  }
  return runtime.completedKeys().size() == expected;
}

void testModeSelectionAndCLI()
{
  std::cout << "qualified-device rollout and command controls\n";
  check(
      svr::kDefaultTerrainGPUV2RequestedMode ==
          svr::TerrainGPUV2RequestedMode::GPUV2,
      "gpu-v2 is the shipping rollout default");
  svr::TerrainGPUV2RequestedMode parsed{};
  check(
      svr::parseTerrainGPUV2RequestedMode("cpu", parsed) &&
          parsed == svr::TerrainGPUV2RequestedMode::CPU &&
          svr::parseTerrainGPUV2RequestedMode("gpu-v2", parsed) &&
          parsed == svr::TerrainGPUV2RequestedMode::GPUV2 &&
          svr::parseTerrainGPUV2RequestedMode("auto", parsed) &&
          !svr::parseTerrainGPUV2RequestedMode("gpu-full", parsed),
      "public mode parser accepts only cpu, gpu-v2, and auto");

  svr::TerrainGPUChunkShape shape{};
  std::string error;
  check(
      svr::parseTerrainGPUV2ChunkShape("8,4,8", shape, &error) &&
          shape.bricksX == 8u && shape.bricksY == 4u &&
          shape.bricksZ == 8u && shape.brickSide == 8u,
      "3D chunk shape keeps fixed occupancy-only 8-cubed bricks: " + error);
  check(
      !svr::parseTerrainGPUV2ChunkShape("8x8", shape, &error),
      "2D chunk shapes are rejected");

  auto configured = rolloutSettings();
  svr::TerrainGPUV2BudgetOverrides overrides{};
  overrides.targetGenerationMicroseconds = 800u;
  overrides.hardGenerationMicroseconds = 1400u;
  overrides.maximumChunksPerFrame = 128u;
  overrides.maximumNewChunksPerFrame = 32u;
  overrides.maximumWorkgroupsPerFrame = 4096u;
  check(
      svr::applyTerrainGPUV2BudgetOverrides(
          overrides, configured.runtime, &error) &&
          configured.runtime.scheduler.targetGenerationMicroseconds == 800u &&
          configured.runtime.scheduler.maximumNewChunksPerFrame == 32u,
      "CLI budget overrides preserve independent time/chunk/workgroup caps: " +
          error);

  svr::TerrainGPUV2Rollout qualified;
  check(
      qualified.start(qualifiedDevice(), rolloutSettings(), &error) &&
          qualified.GPUActive() &&
          qualified.fallbackReason() ==
              svr::TerrainGPUV2RolloutFallback::None,
      "auto selects V2 only with device, pipeline, and visual qualification: " +
          error);

  auto unqualifiedDevice = qualifiedDevice();
  unqualifiedDevice.qualified = false;
  svr::TerrainGPUV2Rollout unqualified;
  check(
      !unqualified.start(unqualifiedDevice, rolloutSettings(), &error) &&
          unqualified.fallbackReason() ==
              svr::TerrainGPUV2RolloutFallback::UnqualifiedDevice,
      "default gpu-v2 fails closed on unqualified hardware instead of generating CPU payloads");
  svr::TerrainGPUV2Rollout unqualifiedAuto;
  check(
      unqualifiedAuto.start(
          unqualifiedDevice,
          rolloutSettings(svr::TerrainGPUV2RequestedMode::Auto), &error) &&
          !unqualifiedAuto.GPUActive() &&
          unqualifiedAuto.fallbackReason() ==
              svr::TerrainGPUV2RolloutFallback::UnqualifiedDevice,
      "auto remains the compact compatibility fallback: " + error);
  auto missingPipelines = rolloutSettings();
  missingPipelines.pipelinesCreated = false;
  svr::TerrainGPUV2Rollout pipelineFailure;
  check(
      !pipelineFailure.start(qualifiedDevice(), missingPipelines, &error) &&
          pipelineFailure.fallbackReason() ==
              svr::TerrainGPUV2RolloutFallback::PipelineCreationFailure,
      "default gpu-v2 fails closed until pipeline creation finishes");
  auto missingPipelinesAuto = rolloutSettings(svr::TerrainGPUV2RequestedMode::Auto);
  missingPipelinesAuto.pipelinesCreated = false;
  svr::TerrainGPUV2Rollout pipelineFallback;
  check(
      pipelineFallback.start(qualifiedDevice(), missingPipelinesAuto, &error) &&
          pipelineFallback.fallbackReason() ==
              svr::TerrainGPUV2RolloutFallback::PipelineCreationFailure,
      "auto can still use the compatibility fallback before pipeline creation: " +
          error);
  svr::TerrainGPUV2Rollout cpu;
  check(
      cpu.start(
          qualifiedDevice(),
          rolloutSettings(svr::TerrainGPUV2RequestedMode::CPU), &error) &&
          cpu.fallbackReason() ==
              svr::TerrainGPUV2RolloutFallback::ExplicitCPU &&
          !cpu.circuitBreakerOpen(),
      "explicit CPU remains a diagnostic mode, not a device failure: " + error);
}

void testColdStartAndSixteenKilometres()
{
  std::cout << "cold start and 16 km streaming\n";
  svr::TerrainGPUV2Rollout rollout;
  std::string error;
  check(
      rollout.start(qualifiedDevice(), rolloutSettings(), &error),
      "cold runtime starts: " + error);
  auto *runtime = rollout.runtime();
  runtime->setWorldBundleRevision(9001u);
  uint32_t accepted = 0u;
  for (uint32_t index = 0u; index < 128u; ++index)
  {
    // Samples coverage from zero through 16 km without turning that entire
    // extent into one giant scheduling job.
    const int32_t coordinate = static_cast<int32_t>(index * 126u);
    uint64_t id = 0u;
    accepted += runtime->requestChunk(
        request(coordinate, 1000u + index), view(), id, &error) ? 1u : 0u;
  }
  uint64_t fence = 0u;
  check(
      accepted == 128u && drive(*runtime, 128u, fence) &&
          runtime->telemetry().rendererDrainCalls == 0u &&
          runtime->telemetry().waitIdleCalls == 0u,
      "128 independent chunks span 16 km and publish without a queue drain: " +
          error);
}

void testTeleportLODOscillationAndAtlasThrash()
{
  std::cout << "teleport, rapid LOD oscillation, and atlas thrash\n";
  svr::TerrainGPUV2Rollout rollout;
  std::string error;
  check(
      rollout.start(qualifiedDevice(), rolloutSettings(), &error),
      "stress runtime starts: " + error);
  auto *runtime = rollout.runtime();
  runtime->setWorldBundleRevision(9001u);

  uint64_t farId = 0u;
  uint64_t nearId = 0u;
  runtime->requestChunk(request(12000, 1u), view(), farId, &error);
  runtime->requestChunk(request(0, 2u), view(), nearId, &error);
  const auto beforeTeleport = runtime->beginFrame(
      {1u, 0u, 0u, 16667u, view(), false});
  const bool nearWon = !beforeTeleport.dispatches.empty() &&
      beforeTeleport.dispatches.front().jobId == nearId;
  for (const auto &dispatch : beforeTeleport.dispatches)
    runtime->completeDispatch(
        {dispatch.id, dispatch.id, svr::TerrainGPUV2Failure::None,
         dispatch.predictedMicroseconds},
        &error);

  // Re-requesting one identity with changing generations cancels stale work
  // instead of allowing two LOD owners to publish over each other.
  uint64_t latest = 0u;
  for (uint32_t generation = 2u; generation <= 32u; ++generation)
    runtime->requestChunk(
        request(400, 30u, generation, false), view(12000.0), latest, &error);

  uint32_t atlasRequests = 0u;
  uint64_t fence = 100u;
  for (uint64_t frame = 2u; frame < 64u; ++frame)
  {
    const auto scheduled = runtime->beginFrame(
        {frame, fence, 0u, 16667u, view(12000.0), true});
    atlasRequests += static_cast<uint32_t>(scheduled.atlasUploads.size());
    for (const auto &upload : scheduled.atlasUploads)
      runtime->completeAtlasUpload(upload.jobId, true, &error);
    for (const auto &dispatch : scheduled.dispatches)
      runtime->completeDispatch(
          {dispatch.id, ++fence, svr::TerrainGPUV2Failure::None,
           dispatch.predictedMicroseconds},
          &error);
    for (const auto &publication : scheduled.publications)
      runtime->completePublication(publication.jobId, ++fence, true, &error);
  }
  const auto telemetry = runtime->telemetry();
  check(
      nearWon && atlasRequests != 0u &&
          telemetry.scheduler.cancelledJobs != 0u &&
          telemetry.rendererDrainCalls == 0u,
      "teleport reprioritizes by screen error while stale LOD and atlas work stay bounded "
      "(near=" + std::to_string(nearWon) +
          " atlas=" + std::to_string(atlasRequests) +
          " cancelled=" + std::to_string(telemetry.scheduler.cancelledJobs) +
          ")");
}

void testMemoryPressureAndCircuitBreaker()
{
  std::cout << "memory pressure and sticky circuit breaker\n";
  auto settings = rolloutSettings();
  settings.runtime.maximumConsecutiveBuildFailures = 2u;
  svr::TerrainGPUV2Rollout rollout;
  std::string error;
  check(
      rollout.start(qualifiedDevice(), settings, &error),
      "pressure runtime starts: " + error);
  auto *runtime = rollout.runtime();
  runtime->setWorldBundleRevision(9001u);
  for (uint32_t index = 0u; index < 4u; ++index)
  {
    uint64_t id = 0u;
    runtime->requestChunk(
        request(static_cast<int32_t>(index), 1u + index), view(), id,
        &error);
  }
  for (uint64_t frame = 1u; frame <= 4u && rollout.GPUActive(); ++frame)
  {
    const auto scheduled = runtime->beginFrame(
        {frame, frame - 1u, 0u, 16667u, view(), false});
    for (const auto &dispatch : scheduled.dispatches)
      runtime->completeDispatch(
          {dispatch.id, frame, svr::TerrainGPUV2Failure::AllocationFailure,
           dispatch.predictedMicroseconds},
          &error);
    rollout.synchronizeRuntimeState();
  }
  check(
      rollout.circuitBreakerOpen() && !rollout.GPUActive() &&
          rollout.fallbackReason() ==
              svr::TerrainGPUV2RolloutFallback::RepeatedBuildFailure &&
          rollout.diagnostics().cpuGeneratedPayloadBytes == 0u,
      "repeated allocation failures fall back before publication without a CPU payload cache");
  rollout.stop();
  check(
      !rollout.start(qualifiedDevice(), settings, &error) &&
          !rollout.GPUActive() && rollout.circuitBreakerOpen(),
      "default gpu-v2 disable/enable fails closed after the same-session circuit breaker");
  svr::TerrainGPUV2Rollout compatibility;
  check(
      compatibility.start(
          qualifiedDevice(),
          rolloutSettings(svr::TerrainGPUV2RequestedMode::Auto), &error),
      "fresh auto compatibility session can still start independently: " +
          error);
}

void testLifecycleAndDeviceResetSoak()
{
  std::cout << "lifecycle and device-reset soak\n";
  const uint32_t baseline = svr::TerrainGPUV2Rollout::liveRuntimeObjects();
  bool clean = true;
  std::string error;
  for (uint32_t cycle = 0u; cycle < 128u; ++cycle)
  {
    svr::TerrainGPUV2Rollout rollout;
    clean &= rollout.start(qualifiedDevice(), rolloutSettings(), &error);
    if ((cycle % 8u) == 0u)
      rollout.recordDeviceLoss();
    rollout.stop();
    clean &= svr::TerrainGPUV2Rollout::liveRuntimeObjects() == baseline;
  }
  check(
      clean && svr::TerrainGPUV2Rollout::liveRuntimeObjects() == baseline,
      "128 enable/disable cycles, including device loss, leave no runtime owner orphan: " +
          error);
}

void testPublicFormatCompatibility()
{
  std::cout << "public data format compatibility\n";
  check(
      sizeof(svr::TerrainGPUV2PayloadResultGPU) == 64u &&
          sizeof(svr::TerrainGPUV2PublishedOwnerGPU) == 96u &&
          sizeof(svr::TerrainGPUV2OwnershipGroupStateGPU) == 64u &&
          sizeof(svr::TerrainGPUV2PublicationResultGPU) == 32u,
      "rollout changes no frozen V2 publication ABI");
}
} // namespace

int main()
{
  std::cout << "Sparse voxel terrain GPU V2 Phase 10 tests\n";
  testModeSelectionAndCLI();
  testColdStartAndSixteenKilometres();
  testTeleportLODOscillationAndAtlasThrash();
  testMemoryPressureAndCircuitBreaker();
  testLifecycleAndDeviceResetSoak();
  testPublicFormatCompatibility();
  if (failures != 0)
    std::cerr << failures << " Phase 10 checks failed\n";
  return failures == 0 ? 0 : 1;
}
