#include "voxel/svraster/TerrainGPUQualification.hpp"

#include <cstdint>
#include <iostream>
#include <string>

#undef NDEBUG
#include <cassert>

namespace
{

using namespace voxel::svraster;

constexpr uint64_t kBundle = 0x504841534538ull;

TerrainGPUGenerationKey key(const uint32_t slot = 7u)
{
  return {kBundle, -11, 19, 2u, 41u, slot, 3u};
}

TerrainGPUDeviceQualificationInput qualifiedDevice(const uint32_t suites = kTerrainGPUFullQualificationSuites, const TerrainGPUDeviceTier tier = TerrainGPUDeviceTier::Discrete)
{
  TerrainGPUDeviceQualificationInput result{};
  result.backend = TerrainGPUBackend::Vulkan;
  result.tier = tier;
  result.vendorId = 0x10deu;
  result.deviceId = 0x1234u;
  result.driverVersion = 9u;
  result.deviceLocalMemoryBytes = 8ull << 30u;
  result.maximumStorageBufferBytes = 1ull << 30u;
  result.maximumStorageBuffersPerStage = 32u;
  result.maximumComputeWorkGroupInvocations = 1024u;
  result.compute = true;
  result.timestampQueries = true;
  result.deviceProfileCertified = true;
  result.passedSuites = suites;
  return result;
}

TerrainGPUBuildRequest request(const uint32_t slot = 7u)
{
  TerrainGPUBuildRequest result{};
  result.key = key(slot);
  result.jobClass = TerrainGPUJobClass::VisibleMissing;
  result.projectedError = 4.0f;
  result.distance = 8.0f;
  result.scratchBytes = 256u;
  result.semanticRevision = 1u;
  result.mapsReady = true;
  return result;
}

void testModeParsingAndFailClosedQualification()
{
  static_assert(
      kDefaultTerrainGenerationMode == TerrainGenerationMode::GPUFull,
      "generated terrain must remain GPU-first");
  TerrainGenerationMode mode = TerrainGenerationMode::Auto;
  assert(parseTerrainGenerationMode("cpu", mode));
  assert(mode == TerrainGenerationMode::CPU);
  assert(parseTerrainGenerationMode("gpu-hybrid", mode));
  assert(mode == TerrainGenerationMode::GPUHybrid);
  assert(parseTerrainGenerationMode("gpu-full", mode));
  assert(mode == TerrainGenerationMode::GPUFull);
  assert(parseTerrainGenerationMode("auto", mode));
  assert(mode == TerrainGenerationMode::Auto);
  assert(!parseTerrainGenerationMode("gpu", mode));

  auto device = qualifiedDevice();
  device.deviceProfileCertified = false;
  auto qualification = qualifyTerrainGPUDevice(
      device, TerrainGenerationMode::Auto);
  assert(qualification.selectedMode == TerrainGenerationMode::CPU);
  assert(!qualification.hybridQualified);
  assert(!qualification.fullQualified);

  device = qualifiedDevice(kTerrainGPUHybridQualificationSuites);
  qualification = qualifyTerrainGPUDevice(
      device, TerrainGenerationMode::Auto);
  assert(qualification.hybridQualified);
  assert(!qualification.fullQualified);
  assert(qualification.selectedMode == TerrainGenerationMode::GPUHybrid);
  assert(qualifyTerrainGPUDevice(device).selectedMode ==
      TerrainGenerationMode::GPUHybrid);
  assert(qualifyTerrainGPUDevice(device, TerrainGenerationMode::GPUFull).selectedMode == TerrainGenerationMode::GPUHybrid);

  qualification = qualifyTerrainGPUDevice(qualifiedDevice());
  assert(qualification.fullQualified);
  assert(qualification.selectedMode == TerrainGenerationMode::GPUFull);
  assert(qualifyTerrainGPUDevice(qualifiedDevice(), TerrainGenerationMode::CPU).selectedMode == TerrainGenerationMode::CPU);
}

void testBackendBudgetsAndSchedulerTelemetry()
{
  const auto minimum = terrainGPUBackendBudget(TerrainGPUBackend::Vulkan, TerrainGPUDeviceTier::Minimum);
  const auto discrete = terrainGPUBackendBudget(TerrainGPUBackend::Vulkan, TerrainGPUDeviceTier::Discrete);
  const auto metal = terrainGPUBackendBudget(TerrainGPUBackend::Metal, TerrainGPUDeviceTier::Discrete);
  assert(minimum.targetGPUTimeMicroseconds == 750u);
  assert(discrete.admissionCeilingMicroseconds == 1250u);
  assert(minimum.sampleColumnQuantum == 8u);
  assert(discrete.sampleColumnQuantum == 32u);
  assert(discrete.maximumDispatchesPerFrame > minimum.maximumDispatchesPerFrame);
  assert(metal.maximumDispatchesPerFrame <= 6u);
  assert(metal.sampleColumnQuantum <= 16u);

  TerrainGPUBuildScheduler scheduler(minimum);
  scheduler.setBundleSignature(kBundle);
  scheduler.setTerrainGeneration(7u, 3u);
  uint64_t jobId = 0u;
  assert(scheduler.request(request(), jobId));
  const auto scheduled = scheduler.scheduleFrame({1u, 0u, 0u, 16667u, false});
  assert(scheduled.dispatches.size() == 1u);
  assert(scheduled.dispatches.front().stage == TerrainGPUBuildStage::SampleColumns);
  assert(scheduled.dispatches.front().workUnits <= minimum.sampleColumnQuantum);
  assert(scheduler.completeDispatch({scheduled.dispatches.front().dispatchId, 1u, TerrainGPUFailure::None, 300u}));
  const auto timing = scheduler.stageTiming(TerrainGPUBuildStage::SampleColumns);
  assert(timing.sampleCount == 1u);
  assert(timing.p95Microseconds == 300u);
  assert(scheduler.diagnostics().quantumBudgetViolations == 1u);

  TerrainGPUBuildScheduler ceilingScheduler(minimum);
  ceilingScheduler.setBundleSignature(kBundle);
  ceilingScheduler.setTerrainGeneration(7u, 3u);
  assert(ceilingScheduler.request(request(), jobId));
  const auto ceilingDispatch = ceilingScheduler.scheduleFrame({1u, 0u, 0u, 16667u, false});
  assert(ceilingDispatch.dispatches.size() == 1u);
  assert(ceilingScheduler.completeDispatch({ceilingDispatch.dispatches.front().dispatchId, 1u, TerrainGPUFailure::None, 1300u}));
  assert(ceilingScheduler.diagnostics().dispatchCeilingViolations == 1u);
}

void testSessionFallbackPreservesGenerationKey()
{
  const auto qualification = qualifyTerrainGPUDevice(qualifiedDevice());
  TerrainGPUSessionController controller;
  std::string error;
  assert(controller.initialize(qualification, {}, &error));
  assert(controller.GPUEnabled());

  const auto failedKey = key();
  controller.recordJobCompletion(10u, failedKey, TerrainGPUFailure::CounterOverflow);
  controller.recordJobCompletion(10u, failedKey, TerrainGPUFailure::CounterOverflow);
  assert(controller.GPUEnabled());
  controller.recordJobCompletion(10u, failedKey, TerrainGPUFailure::CounterOverflow);
  assert(controller.mode() == TerrainGenerationMode::CPU);
  assert(controller.diagnostics().fallbackReason == TerrainGPUFallbackReason::RepeatedBuildFailure);
  const auto fallback = controller.takeFallbackRequests();
  assert(fallback.size() == 1u);
  assert(fallback.front().key == failedKey);
  assert(fallback.front().failedJobId == 10u);

  TerrainGPUBuildScheduler scheduler(qualification.budget);
  scheduler.setBundleSignature(kBundle);
  scheduler.setTerrainGeneration(7u, 3u);
  uint64_t jobId = 0u;
  assert(scheduler.request(request(), jobId));
  assert(controller.disableGPU(scheduler) == 1u);
  assert(scheduler.job(jobId)->stage == TerrainGPUBuildStage::Cancelled);
  const auto cancelledFallback = controller.takeFallbackRequests();
  assert(cancelledFallback.size() == 1u);
  assert(cancelledFallback.front().key == failedKey);

  TerrainGPUSessionController stale;
  assert(stale.initialize(qualification));
  stale.recordJobCompletion(1u, failedKey, TerrainGPUFailure::StaleGeneration);
  assert(stale.GPUEnabled());
  assert(stale.takeFallbackRequests().empty());
}

void testHealthFallbacks()
{
  const auto qualification = qualifyTerrainGPUDevice(qualifiedDevice());
  TerrainGPUSessionController budget;
  assert(budget.initialize(qualification));
  TerrainGPUFrameHealth unhealthy{};
  unhealthy.maximumQuantumMicroseconds = 251u;
  for (uint32_t frame = 0u; frame < 4u; ++frame)
    budget.recordFrame(unhealthy);
  assert(budget.GPUEnabled());
  budget.recordFrame(unhealthy);
  assert(budget.mode() == TerrainGenerationMode::CPU);
  assert(budget.diagnostics().fallbackReason == TerrainGPUFallbackReason::SustainedBudgetViolation);

  TerrainGPUSessionController correctness;
  assert(correctness.initialize(qualification));
  TerrainGPUFrameHealth invalid{};
  invalid.unrelatedLightHistoryInvalidated = true;
  correctness.recordFrame(invalid);
  assert(correctness.mode() == TerrainGenerationMode::CPU);
  assert(correctness.diagnostics().correctnessViolations == 1u);

  TerrainGPUSessionController memory;
  assert(memory.initialize(qualification));
  TerrainGPUFrameHealth oversized{};
  oversized.mapUploadBytes = qualification.budget.maximumMapUploadBytesPerFrame + 1u;
  for (uint32_t frame = 0u; frame < 5u; ++frame)
    memory.recordFrame(oversized);
  assert(memory.mode() == TerrainGenerationMode::CPU);

  TerrainGPUSessionController deviceLost;
  assert(deviceLost.initialize(qualification));
  deviceLost.recordDeviceLoss();
  assert(deviceLost.mode() == TerrainGenerationMode::CPU);
  assert(deviceLost.diagnostics().fallbackReason == TerrainGPUFallbackReason::DeviceLost);
}

void testPerformanceQualification()
{
  TerrainGPUPerformanceQualification qualification;
  for (uint32_t scenario = 0u; scenario < 4u; ++scenario)
    for (uint32_t frame = 0u; frame < 100u; ++frame)
    {
      TerrainGPUPerformanceSample sample{};
      sample.workload = static_cast<TerrainGPUWorkload>(scenario);
      sample.sourceFrameMicroseconds = scenario == 0u ? 10000u : 9800u;
      sample.CPUReferenceSourceFrameMicroseconds = 10000u;
      sample.requestToVisibleMicroseconds = 700u;
      sample.CPUReferenceRequestToVisibleMicroseconds = 1000u;
      sample.terrainGPUTimeMicroseconds = 700u;
      sample.maximumQuantumMicroseconds = 200u;
      qualification.record(sample);
    }
  const auto report = qualification.evaluate();
  assert(report.qualified);
  assert(report.totalFrames == 400u);
  assert(report.framesAtOrBelowCeiling == 400u);
  assert(report.terrainFrameP99Microseconds == 700u);
  assert(report.quantumP95Microseconds == 200u);
  for (const auto &scenario : report.scenarios)
    assert(scenario.sampleCount == 100u);

  TerrainGPUPerformanceQualification bad;
  TerrainGPUPerformanceSample sample{};
  sample.workload = TerrainGPUWorkload::Walk;
  sample.sourceFrameMicroseconds = 11000u;
  sample.CPUReferenceSourceFrameMicroseconds = 10000u;
  sample.requestToVisibleMicroseconds = 900u;
  sample.CPUReferenceRequestToVisibleMicroseconds = 1000u;
  sample.terrainGPUTimeMicroseconds = 1600u;
  sample.maximumQuantumMicroseconds = 400u;
  sample.mapBudgetBounded = false;
  sample.publicationCorrect = false;
  bad.record(sample);
  assert(!bad.evaluate().qualified);
}

} // namespace

int main()
{
  testModeParsingAndFailClosedQualification();
  testBackendBudgetsAndSchedulerTelemetry();
  testSessionFallbackPreservesGenerationKey();
  testHealthFallbacks();
  testPerformanceQualification();
  std::cout << "Sparse voxel terrain Phase 8 qualification tests passed\n";
  return 0;
}
