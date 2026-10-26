#include "voxel/svraster/TerrainGPUQualification.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool suites(const uint32_t passed, const uint32_t required)
{
  return (passed & required) == required;
}
} // namespace

TerrainGPUBuildBudget terrainGPUBackendBudget(const TerrainGPUBackend backend, const TerrainGPUDeviceTier tier)
{
  TerrainGPUBuildBudget budget{};
  // The interactive target and hard ceiling are invariant qualification
  // gates. Backends tune concurrency and quantum size, not permission to steal
  // more frame time.
  budget.targetGPUTimeMicroseconds = 750u;
  budget.admissionCeilingMicroseconds = 1250u;
  budget.maximumQuantumMicroseconds = 250u;
  if (tier == TerrainGPUDeviceTier::Minimum)
  {
    budget.maximumDispatchesPerFrame = 4u;
    budget.maximumPagesStartedPerFrame = 1u;
    budget.maximumPagesPublishedPerFrame = 2u;
    budget.maximumMapUploadBytesPerFrame = 2ull << 20u;
    budget.maximumScratchBytes = 32ull << 20u;
    budget.maximumInFlightJobs = 4u;
    budget.sampleColumnQuantum = 8u;
    budget.fineBrickQuantum = 128u;
    budget.sidecarQuantum = 8u;
    budget.coldEstimateScalePermille = 1250u;
  }
  else if (tier == TerrainGPUDeviceTier::Discrete)
  {
    budget.maximumDispatchesPerFrame = 12u;
    budget.maximumPagesStartedPerFrame = 4u;
    budget.maximumPagesPublishedPerFrame = 6u;
    budget.maximumMapUploadBytesPerFrame = 8ull << 20u;
    budget.maximumScratchBytes = 128ull << 20u;
    budget.maximumInFlightJobs = 16u;
    budget.sampleColumnQuantum = 32u;
    budget.fineBrickQuantum = 512u;
    budget.sidecarQuantum = 32u;
    budget.coldEstimateScalePermille = 800u;
  }
  // Metal's storage-buffer emulation and timestamp completion are more
  // conservative than native Vulkan/D3D12 on the same nominal tier.
  if (backend == TerrainGPUBackend::Metal)
  {
    budget.maximumDispatchesPerFrame = std::min(budget.maximumDispatchesPerFrame, 6u);
    budget.maximumPagesStartedPerFrame = std::min(budget.maximumPagesStartedPerFrame, 2u);
    budget.sampleColumnQuantum = std::min(budget.sampleColumnQuantum, 16u);
    budget.fineBrickQuantum = std::min(budget.fineBrickQuantum, 256u);
    budget.sidecarQuantum = std::min(budget.sidecarQuantum, 16u);
    budget.coldEstimateScalePermille = std::max(budget.coldEstimateScalePermille, 1100u);
  }
  return budget;
}

TerrainGPUDeviceQualification qualifyTerrainGPUDevice(const TerrainGPUDeviceQualificationInput &device, const TerrainGenerationMode requestedMode)
{
  TerrainGPUDeviceQualification result{};
  result.budget = terrainGPUBackendBudget(device.backend, device.tier);
  const bool baseCapabilities = device.backend != TerrainGPUBackend::Unknown && device.compute && device.timestampQueries && device.maximumComputeWorkGroupInvocations >= 64u &&
                                device.maximumStorageBuffersPerStage >= 13u && device.maximumStorageBufferBytes >= (16ull << 20u) && device.deviceLocalMemoryBytes >= (512ull << 20u);
  const bool fullCapabilities = baseCapabilities && device.maximumStorageBuffersPerStage >= 15u && device.maximumStorageBufferBytes >= (64ull << 20u);
  result.hybridQualified = baseCapabilities && device.deviceProfileCertified && suites(device.passedSuites, kTerrainGPUHybridQualificationSuites);
  result.fullQualified = fullCapabilities && device.deviceProfileCertified && suites(device.passedSuites, kTerrainGPUFullQualificationSuites);

  switch (requestedMode)
  {
  case TerrainGenerationMode::CPU:
    result.selectedMode = TerrainGenerationMode::CPU;
    result.reason = "CPU reference path requested";
    break;
  case TerrainGenerationMode::GPUHybrid:
    result.selectedMode = result.hybridQualified ? TerrainGenerationMode::GPUHybrid : TerrainGenerationMode::CPU;
    result.reason = result.hybridQualified ? "qualified hybrid GPU terrain profile" : "hybrid GPU terrain qualification failed";
    break;
  case TerrainGenerationMode::GPUFull:
    if (result.fullQualified)
    {
      result.selectedMode = TerrainGenerationMode::GPUFull;
      result.reason = "qualified full GPU terrain profile";
    }
    else if (result.hybridQualified)
    {
      // GPUFull is the preference, not permission to throw away a qualified
      // GPU hierarchy builder merely because atlas/column sampling is not yet
      // available on this profile.
      result.selectedMode = TerrainGenerationMode::GPUHybrid;
      result.reason = "full GPU terrain unavailable; using qualified hybrid GPU terrain";
    }
    else
    {
      result.selectedMode = TerrainGenerationMode::CPU;
      result.reason = "GPU terrain qualification failed";
    }
    break;
  case TerrainGenerationMode::Auto:
    if (result.fullQualified)
    {
      result.selectedMode = TerrainGenerationMode::GPUFull;
      result.reason = "default full GPU terrain on qualified device";
    }
    else if (result.hybridQualified)
    {
      result.selectedMode = TerrainGenerationMode::GPUHybrid;
      result.reason = "default hybrid GPU terrain on qualified device";
    }
    else
    {
      result.selectedMode = TerrainGenerationMode::CPU;
      result.reason = device.deviceProfileCertified ? "device failed required GPU terrain suites" : "device has no certified GPU terrain profile";
    }
    break;
  }
  return result;
}

bool parseTerrainGenerationMode(const std::string &text, TerrainGenerationMode &mode)
{
  if (text == "auto")
    mode = TerrainGenerationMode::Auto;
  else if (text == "cpu")
    mode = TerrainGenerationMode::CPU;
  else if (text == "gpu-hybrid")
    mode = TerrainGenerationMode::GPUHybrid;
  else if (text == "gpu-full")
    mode = TerrainGenerationMode::GPUFull;
  else
    return false;
  return true;
}

const char *terrainGenerationModeName(const TerrainGenerationMode mode)
{
  switch (mode)
  {
  case TerrainGenerationMode::Auto:
    return "auto";
  case TerrainGenerationMode::CPU:
    return "cpu";
  case TerrainGenerationMode::GPUHybrid:
    return "gpu-hybrid";
  case TerrainGenerationMode::GPUFull:
    return "gpu-full";
  }
  return "unknown";
}

const char *terrainGPUBackendName(const TerrainGPUBackend backend)
{
  switch (backend)
  {
  case TerrainGPUBackend::Vulkan:
    return "vulkan";
  case TerrainGPUBackend::Metal:
    return "metal";
  case TerrainGPUBackend::D3D12:
    return "d3d12";
  case TerrainGPUBackend::Unknown:
    break;
  }
  return "unknown";
}

bool TerrainGPUSessionController::initialize(const TerrainGPUDeviceQualification &qualification, const TerrainGPUSessionPolicy &policy, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (policy.maximumConsecutiveBuildFailures == 0u || policy.maximumConsecutiveBudgetViolations == 0u || policy.maximumQuantumMicroseconds == 0u || policy.admissionCeilingMicroseconds == 0u ||
      policy.maximumQuantumMicroseconds > policy.admissionCeilingMicroseconds)
    return fail(error, "GPU terrain session policy is invalid");
  policy_ = policy;
  budget_ = qualification.budget;
  diagnostics_ = {};
  diagnostics_.initialMode = qualification.selectedMode;
  diagnostics_.activeMode = qualification.selectedMode;
  if (qualification.selectedMode == TerrainGenerationMode::CPU)
    diagnostics_.fallbackReason = qualification.reason == "CPU reference path requested" ? TerrainGPUFallbackReason::ExplicitCPU : TerrainGPUFallbackReason::UnsupportedDevice;
  fallback_.clear();
  return true;
}

void TerrainGPUSessionController::enqueueFallback(const uint64_t jobId, const TerrainGPUGenerationKey &key, const TerrainGPUFailure failure, const TerrainGPUFallbackReason reason)
{
  const auto inserted = fallback_.emplace(key, TerrainGPUFallbackRequest{jobId, key, failure, reason});
  if (inserted.second)
    ++diagnostics_.CPUFallbackJobs;
}

void TerrainGPUSessionController::fallbackSession(const TerrainGPUFallbackReason reason)
{
  if (!GPUEnabled())
    return;
  diagnostics_.activeMode = TerrainGenerationMode::CPU;
  diagnostics_.fallbackReason = reason;
}

void TerrainGPUSessionController::recordJobCompletion(const uint64_t jobId, const TerrainGPUGenerationKey &key, const TerrainGPUFailure failure)
{
  if (failure == TerrainGPUFailure::None)
  {
    ++diagnostics_.completedGPUJobs;
    diagnostics_.consecutiveBuildFailures = 0u;
    return;
  }
  if (failure == TerrainGPUFailure::StaleGeneration)
    return;
  ++diagnostics_.failedGPUJobs;
  ++diagnostics_.consecutiveBuildFailures;
  enqueueFallback(jobId, key, failure, TerrainGPUFallbackReason::RepeatedBuildFailure);
  if (diagnostics_.consecutiveBuildFailures >= policy_.maximumConsecutiveBuildFailures)
    fallbackSession(TerrainGPUFallbackReason::RepeatedBuildFailure);
}

void TerrainGPUSessionController::recordFrame(const TerrainGPUFrameHealth &health)
{
  diagnostics_.maximumObservedQuantumMicroseconds = std::max(diagnostics_.maximumObservedQuantumMicroseconds, health.maximumQuantumMicroseconds);
  const bool ceiling = health.terrainGPUTimeMicroseconds > policy_.admissionCeilingMicroseconds;
  const bool budget = ceiling || health.maximumQuantumMicroseconds > policy_.maximumQuantumMicroseconds || health.mapUploadBytes > budget_.maximumMapUploadBytesPerFrame ||
                      health.scratchBytes > budget_.maximumScratchBytes || health.mapBudgetExceeded || health.scratchBudgetExceeded;
  if (ceiling)
    ++diagnostics_.ceilingViolationFrames;
  if (budget)
  {
    ++diagnostics_.budgetViolationFrames;
    ++diagnostics_.consecutiveBudgetViolations;
  }
  else
    diagnostics_.consecutiveBudgetViolations = 0u;
  const bool correctness = health.stalePublication || health.rasterTreeMismatch || health.coverageOrCrackFailure || health.unrelatedLightHistoryInvalidated;
  if (correctness)
  {
    ++diagnostics_.correctnessViolations;
    fallbackSession(TerrainGPUFallbackReason::CorrectnessViolation);
  }
  else if (health.deviceLost)
    recordDeviceLoss();
  else if (diagnostics_.consecutiveBudgetViolations >= policy_.maximumConsecutiveBudgetViolations)
    fallbackSession(TerrainGPUFallbackReason::SustainedBudgetViolation);
}

void TerrainGPUSessionController::recordDeviceLoss()
{
  fallbackSession(TerrainGPUFallbackReason::DeviceLost);
}

uint32_t TerrainGPUSessionController::disableGPU(TerrainGPUBuildScheduler &scheduler)
{
  if (GPUEnabled())
    return 0u;
  for (const TerrainGPUBuildJob &job : scheduler.unfinishedJobs())
    enqueueFallback(job.id, job.key, TerrainGPUFailure::None, diagnostics_.fallbackReason);
  return scheduler.cancelAll();
}

std::vector<TerrainGPUFallbackRequest> TerrainGPUSessionController::takeFallbackRequests()
{
  std::vector<TerrainGPUFallbackRequest> result;
  result.reserve(fallback_.size());
  for (const auto &[key, request] : fallback_)
    result.push_back(request);
  fallback_.clear();
  return result;
}

const char *terrainGPUFallbackReasonName(const TerrainGPUFallbackReason reason)
{
  switch (reason)
  {
  case TerrainGPUFallbackReason::None:
    return "none";
  case TerrainGPUFallbackReason::UnsupportedDevice:
    return "unsupported-device";
  case TerrainGPUFallbackReason::MissingAtlasCapability:
    return "missing-atlas-capability";
  case TerrainGPUFallbackReason::RepeatedBuildFailure:
    return "repeated-build-failure";
  case TerrainGPUFallbackReason::SustainedBudgetViolation:
    return "sustained-budget-violation";
  case TerrainGPUFallbackReason::CorrectnessViolation:
    return "correctness-violation";
  case TerrainGPUFallbackReason::DeviceLost:
    return "device-lost";
  case TerrainGPUFallbackReason::ExplicitCPU:
    return "explicit-cpu";
  }
  return "unknown";
}

void TerrainGPUPerformanceQualification::clear()
{
  samples_.clear();
}

void TerrainGPUPerformanceQualification::record(const TerrainGPUPerformanceSample &sample)
{
  samples_.push_back(sample);
}

TerrainGPUPerformanceReport TerrainGPUPerformanceQualification::evaluate(
    const uint32_t targetMicroseconds,
    const uint32_t ceilingMicroseconds,
    const uint32_t maximumQuantumP95Microseconds,
    const uint32_t maximumStaticRegressionMicroseconds,
    const uint32_t minimumWalkLatencyImprovementPercent) const
{
  TerrainGPUPerformanceReport report{};
  report.totalFrames = static_cast<uint32_t>(samples_.size());
  if (targetMicroseconds == 0u || ceilingMicroseconds < targetMicroseconds || maximumQuantumP95Microseconds == 0u || minimumWalkLatencyImprovementPercent > 100u)
  {
    report.failures.push_back("invalid qualification thresholds");
    return report;
  }
  const auto percentile = [](std::vector<uint32_t> values, const double fraction)
  {
    if (values.empty())
      return 0u;
    std::sort(values.begin(), values.end());
    const size_t index = std::min(values.size() - 1u, static_cast<size_t>(std::ceil(values.size() * fraction)) - 1u);
    return values[index];
  };
  std::vector<uint32_t> terrainTimes;
  std::vector<uint32_t> quantums;
  std::array<std::vector<TerrainGPUPerformanceSample>, 4u> scenarios;
  bool bounded = true;
  bool correct = true;
  bool overshootBounded = true;
  for (const auto &sample : samples_)
  {
    terrainTimes.push_back(sample.terrainGPUTimeMicroseconds);
    quantums.push_back(sample.maximumQuantumMicroseconds);
    scenarios[static_cast<uint32_t>(sample.workload)].push_back(sample);
    if (sample.terrainGPUTimeMicroseconds <= ceilingMicroseconds)
      ++report.framesAtOrBelowCeiling;
    if (sample.terrainGPUTimeMicroseconds > ceilingMicroseconds + maximumQuantumP95Microseconds)
      overshootBounded = false;
    bounded = bounded && sample.mapBudgetBounded && sample.scratchBudgetBounded;
    correct = correct && sample.publicationCorrect && sample.lightingStable;
  }
  report.terrainFrameP99Microseconds = percentile(terrainTimes, 0.99);
  report.quantumP95Microseconds = percentile(quantums, 0.95);
  for (uint32_t scenario = 0u; scenario < scenarios.size(); ++scenario)
  {
    auto &out = report.scenarios[scenario];
    out.sampleCount = static_cast<uint32_t>(scenarios[scenario].size());
    std::vector<uint32_t> source;
    std::vector<uint32_t> CPUFrame;
    std::vector<uint32_t> latency;
    std::vector<uint32_t> CPULatency;
    for (const auto &sample : scenarios[scenario])
    {
      source.push_back(sample.sourceFrameMicroseconds);
      CPUFrame.push_back(sample.CPUReferenceSourceFrameMicroseconds);
      latency.push_back(sample.requestToVisibleMicroseconds);
      CPULatency.push_back(sample.CPUReferenceRequestToVisibleMicroseconds);
    }
    out.sourceFrameP95Microseconds = percentile(source, 0.95);
    out.CPUReferenceSourceFrameP95Microseconds = percentile(CPUFrame, 0.95);
    out.requestToVisibleP50Microseconds = percentile(latency, 0.50);
    out.requestToVisibleP95Microseconds = percentile(latency, 0.95);
    out.CPUReferenceRequestToVisibleP50Microseconds = percentile(CPULatency, 0.50);
    out.CPUReferenceRequestToVisibleP95Microseconds = percentile(CPULatency, 0.95);
  }
  if (samples_.empty())
    report.failures.push_back("no performance samples");
  for (uint32_t scenario = 0u; scenario < report.scenarios.size(); ++scenario)
    if (report.scenarios[scenario].sampleCount == 0u)
      report.failures.push_back("missing workload scenario " + std::to_string(scenario));
  if (report.totalFrames != 0u && uint64_t{report.framesAtOrBelowCeiling} * 100u < uint64_t{report.totalFrames} * 99u)
    report.failures.push_back("less than 99 percent of frames meet ceiling");
  if (!overshootBounded)
    report.failures.push_back("terrain frame overshoot exceeds one quantum");
  if (report.quantumP95Microseconds > maximumQuantumP95Microseconds)
    report.failures.push_back("quantum p95 exceeds limit");
  const auto &settled = report.scenarios[static_cast<uint32_t>(TerrainGPUWorkload::Static)];
  if (settled.sourceFrameP95Microseconds > settled.CPUReferenceSourceFrameP95Microseconds + maximumStaticRegressionMicroseconds)
    report.failures.push_back("settled source-frame p95 regressed");
  const auto &walk = report.scenarios[static_cast<uint32_t>(TerrainGPUWorkload::Walk)];
  if (walk.sourceFrameP95Microseconds > walk.CPUReferenceSourceFrameP95Microseconds)
    report.failures.push_back("walk source-frame p95 regressed");
  const auto improves = [&](const uint32_t GPU, const uint32_t CPU)
  {
    return CPU != 0u && uint64_t{GPU} * 100u <= uint64_t{CPU} * (100u - minimumWalkLatencyImprovementPercent);
  };
  if (!improves(walk.requestToVisibleP50Microseconds, walk.CPUReferenceRequestToVisibleP50Microseconds) || !improves(walk.requestToVisibleP95Microseconds, walk.CPUReferenceRequestToVisibleP95Microseconds))
    report.failures.push_back("walk request latency improvement is insufficient");
  if (!bounded)
    report.failures.push_back("streaming or scratch budget exceeded");
  if (!correct)
    report.failures.push_back("publication or lighting correctness failed");
  report.qualified = report.failures.empty();
  return report;
}

} // namespace voxel::svraster
