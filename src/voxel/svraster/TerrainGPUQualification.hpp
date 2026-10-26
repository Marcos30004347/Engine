#pragma once

#include "voxel/svraster/TerrainGPUBuildScheduler.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace voxel::svraster
{

enum class TerrainGenerationMode : uint32_t
{
  Auto = 0u,
  CPU = 1u,
  GPUHybrid = 2u,
  GPUFull = 3u,
};

// Generated terrain is GPU-first. Runtime owners still qualify the active
// device and retain the CPU reference builder for unsupported devices and
// session fallback, but must not silently begin in CPU mode.
inline constexpr TerrainGenerationMode kDefaultTerrainGenerationMode =
    TerrainGenerationMode::GPUFull;

enum class TerrainGPUBackend : uint32_t
{
  Unknown = 0u,
  Vulkan = 1u,
  Metal = 2u,
  D3D12 = 3u,
};

enum class TerrainGPUDeviceTier : uint32_t
{
  Minimum = 0u,
  Integrated = 1u,
  Discrete = 2u,
};

enum TerrainGPUQualificationSuite : uint32_t
{
  TerrainGPUQualificationColumns = 1u << 0u,
  TerrainGPUQualificationRaster = 1u << 1u,
  TerrainGPUQualificationTree64 = 1u << 2u,
  TerrainGPUQualificationPublication = 1u << 3u,
  TerrainGPUQualificationAtlas = 1u << 4u,
  TerrainGPUQualificationStreaming = 1u << 5u,
  TerrainGPUQualificationLightingImages = 1u << 6u,
  TerrainGPUQualificationPerformance = 1u << 7u,
};

inline constexpr uint32_t kTerrainGPUHybridQualificationSuites =
    TerrainGPUQualificationRaster | TerrainGPUQualificationTree64 | TerrainGPUQualificationPublication | TerrainGPUQualificationStreaming | TerrainGPUQualificationLightingImages | TerrainGPUQualificationPerformance;
inline constexpr uint32_t kTerrainGPUFullQualificationSuites = kTerrainGPUHybridQualificationSuites | TerrainGPUQualificationColumns | TerrainGPUQualificationAtlas;

struct TerrainGPUDeviceQualificationInput
{
  TerrainGPUBackend backend = TerrainGPUBackend::Unknown;
  TerrainGPUDeviceTier tier = TerrainGPUDeviceTier::Minimum;
  uint32_t vendorId = 0u;
  uint32_t deviceId = 0u;
  uint64_t driverVersion = 0u;
  uint64_t deviceLocalMemoryBytes = 0u;
  uint64_t maximumStorageBufferBytes = 0u;
  uint32_t maximumStorageBuffersPerStage = 0u;
  uint32_t maximumComputeWorkGroupInvocations = 0u;
  bool compute = false;
  bool timestampQueries = false;
  bool deviceProfileCertified = false;
  uint32_t passedSuites = 0u;
};

struct TerrainGPUDeviceQualification
{
  bool hybridQualified = false;
  bool fullQualified = false;
  TerrainGenerationMode selectedMode = TerrainGenerationMode::CPU;
  TerrainGPUBuildBudget budget{};
  std::string reason;
};

TerrainGPUBuildBudget terrainGPUBackendBudget(TerrainGPUBackend backend, TerrainGPUDeviceTier tier);

TerrainGPUDeviceQualification qualifyTerrainGPUDevice(
    const TerrainGPUDeviceQualificationInput &device,
    TerrainGenerationMode requestedMode = kDefaultTerrainGenerationMode);

bool parseTerrainGenerationMode(const std::string &text, TerrainGenerationMode &mode);
const char *terrainGenerationModeName(TerrainGenerationMode mode);
const char *terrainGPUBackendName(TerrainGPUBackend backend);

enum class TerrainGPUFallbackReason : uint32_t
{
  None = 0u,
  UnsupportedDevice,
  MissingAtlasCapability,
  RepeatedBuildFailure,
  SustainedBudgetViolation,
  CorrectnessViolation,
  DeviceLost,
  ExplicitCPU,
};

struct TerrainGPUFallbackRequest
{
  uint64_t failedJobId = 0u;
  TerrainGPUGenerationKey key{};
  TerrainGPUFailure failure = TerrainGPUFailure::None;
  TerrainGPUFallbackReason reason = TerrainGPUFallbackReason::None;
};

struct TerrainGPUFrameHealth
{
  uint32_t terrainGPUTimeMicroseconds = 0u;
  uint32_t maximumQuantumMicroseconds = 0u;
  uint64_t mapUploadBytes = 0u;
  uint64_t scratchBytes = 0u;
  bool mapBudgetExceeded = false;
  bool scratchBudgetExceeded = false;
  bool stalePublication = false;
  bool rasterTreeMismatch = false;
  bool coverageOrCrackFailure = false;
  bool unrelatedLightHistoryInvalidated = false;
  bool deviceLost = false;
};

struct TerrainGPUSessionPolicy
{
  uint32_t maximumConsecutiveBuildFailures = 3u;
  uint32_t maximumConsecutiveBudgetViolations = 5u;
  uint32_t maximumQuantumMicroseconds = 250u;
  uint32_t admissionCeilingMicroseconds = 1250u;
};

struct TerrainGPUSessionDiagnostics
{
  TerrainGenerationMode initialMode = TerrainGenerationMode::CPU;
  TerrainGenerationMode activeMode = TerrainGenerationMode::CPU;
  TerrainGPUFallbackReason fallbackReason = TerrainGPUFallbackReason::None;
  uint64_t completedGPUJobs = 0u;
  uint64_t failedGPUJobs = 0u;
  uint64_t CPUFallbackJobs = 0u;
  uint64_t budgetViolationFrames = 0u;
  uint64_t ceilingViolationFrames = 0u;
  uint64_t correctnessViolations = 0u;
  uint32_t consecutiveBuildFailures = 0u;
  uint32_t consecutiveBudgetViolations = 0u;
  uint32_t maximumObservedQuantumMicroseconds = 0u;
};

// Session-scoped rollout authority. It never mutates residency or terrain-slot
// ownership. Failed GPU work is returned as the exact generation key the CPU
// reference builder must retry through the normal publication transaction.
class TerrainGPUSessionController
{
public:
  bool initialize(const TerrainGPUDeviceQualification &qualification, const TerrainGPUSessionPolicy &policy = {}, std::string *error = nullptr);

  TerrainGenerationMode mode() const
  {
    return diagnostics_.activeMode;
  }
  bool GPUEnabled() const
  {
    return mode() == TerrainGenerationMode::GPUHybrid || mode() == TerrainGenerationMode::GPUFull;
  }

  void recordJobCompletion(uint64_t jobId, const TerrainGPUGenerationKey &key, TerrainGPUFailure failure);
  void recordFrame(const TerrainGPUFrameHealth &health);
  void recordDeviceLoss();
  uint32_t disableGPU(TerrainGPUBuildScheduler &scheduler);

  std::vector<TerrainGPUFallbackRequest> takeFallbackRequests();
  TerrainGPUSessionDiagnostics diagnostics() const
  {
    return diagnostics_;
  }

private:
  void fallbackSession(TerrainGPUFallbackReason reason);
  void enqueueFallback(uint64_t jobId, const TerrainGPUGenerationKey &key, TerrainGPUFailure failure, TerrainGPUFallbackReason reason);

  TerrainGPUSessionPolicy policy_{};
  TerrainGPUBuildBudget budget_{};
  TerrainGPUSessionDiagnostics diagnostics_{};
  std::map<TerrainGPUGenerationKey, TerrainGPUFallbackRequest> fallback_{};
};

const char *terrainGPUFallbackReasonName(TerrainGPUFallbackReason reason);

enum class TerrainGPUWorkload : uint32_t
{
  Static = 0u,
  Walk,
  FastTravel,
  Teleport,
};

struct TerrainGPUPerformanceSample
{
  TerrainGPUWorkload workload = TerrainGPUWorkload::Static;
  uint32_t sourceFrameMicroseconds = 0u;
  uint32_t CPUReferenceSourceFrameMicroseconds = 0u;
  uint32_t requestToVisibleMicroseconds = 0u;
  uint32_t CPUReferenceRequestToVisibleMicroseconds = 0u;
  uint32_t terrainGPUTimeMicroseconds = 0u;
  uint32_t maximumQuantumMicroseconds = 0u;
  bool mapBudgetBounded = true;
  bool scratchBudgetBounded = true;
  bool publicationCorrect = true;
  bool lightingStable = true;
};

struct TerrainGPUPerformanceScenarioReport
{
  uint32_t sampleCount = 0u;
  uint32_t sourceFrameP95Microseconds = 0u;
  uint32_t CPUReferenceSourceFrameP95Microseconds = 0u;
  uint32_t requestToVisibleP50Microseconds = 0u;
  uint32_t requestToVisibleP95Microseconds = 0u;
  uint32_t CPUReferenceRequestToVisibleP50Microseconds = 0u;
  uint32_t CPUReferenceRequestToVisibleP95Microseconds = 0u;
};

struct TerrainGPUPerformanceReport
{
  bool qualified = false;
  uint32_t totalFrames = 0u;
  uint32_t framesAtOrBelowCeiling = 0u;
  uint32_t terrainFrameP99Microseconds = 0u;
  uint32_t quantumP95Microseconds = 0u;
  std::array<TerrainGPUPerformanceScenarioReport, 4u> scenarios{};
  std::vector<std::string> failures{};
};

class TerrainGPUPerformanceQualification
{
public:
  void clear();
  void record(const TerrainGPUPerformanceSample &sample);
  TerrainGPUPerformanceReport evaluate(
      uint32_t targetMicroseconds = 750u,
      uint32_t ceilingMicroseconds = 1250u,
      uint32_t maximumQuantumP95Microseconds = 250u,
      uint32_t maximumStaticRegressionMicroseconds = 300u,
      uint32_t minimumWalkLatencyImprovementPercent = 30u) const;

private:
  std::vector<TerrainGPUPerformanceSample> samples_{};
};

} // namespace voxel::svraster
