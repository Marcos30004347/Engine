#pragma once

#include "voxel/svraster/TerrainGPUV2Runtime.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace voxel::svraster
{

enum class TerrainGPUV2RequestedMode : uint32_t
{
  Auto,
  CPU,
  GPUV2,
};

inline constexpr TerrainGPUV2RequestedMode kDefaultTerrainGPUV2RequestedMode =
    TerrainGPUV2RequestedMode::GPUV2;

enum class TerrainGPUV2RolloutFallback : uint32_t
{
  None,
  ExplicitCPU,
  UnqualifiedDevice,
  PipelineCreationFailure,
  VisualQualificationMissing,
  VisualQualificationFailed,
  MissingCompute,
  InsufficientLimits,
  DeviceLost,
  RepeatedBuildFailure,
  SustainedBudgetViolation,
  SessionCircuitBreaker,
};

enum class TerrainGPUV2RasterHierarchyMode : uint32_t
{
  Off,
  On,
  Auto,
};

struct TerrainGPUV2BudgetOverrides
{
  std::optional<uint32_t> targetGenerationMicroseconds{};
  std::optional<uint32_t> hardGenerationMicroseconds{};
  std::optional<uint32_t> maximumSingleDispatchMicroseconds{};
  std::optional<uint32_t> maximumChunksPerFrame{};
  std::optional<uint32_t> maximumNewChunksPerFrame{};
  std::optional<uint32_t> maximumWorkgroupsPerFrame{};
};

struct TerrainGPUV2RolloutSettings
{
  TerrainGPUV2RequestedMode requestedMode = kDefaultTerrainGPUV2RequestedMode;
  TerrainGPUV2RuntimeSettings runtime{};
  TerrainGPUChunkShape chunkShape{8u, 4u, 8u, 8u};
  TerrainGPUV2RasterHierarchyMode rasterHierarchy =
      TerrainGPUV2RasterHierarchyMode::Auto;
  bool pipelinesCreated = false;
  bool visualQualificationAvailable = false;
  TerrainGPUV2StabilityReport visualQualification{};
};

struct TerrainGPUV2RolloutDiagnostics
{
  TerrainGPUV2RequestedMode requestedMode = kDefaultTerrainGPUV2RequestedMode;
  TerrainGPUV2RuntimeMode activeMode = TerrainGPUV2RuntimeMode::CPUFallback;
  TerrainGPUV2RolloutFallback fallback = TerrainGPUV2RolloutFallback::None;
  bool started = false;
  bool pipelinesCreated = false;
  bool deviceQualified = false;
  bool visualQualified = false;
  bool circuitBreakerOpen = false;
  uint64_t enableCount = 0u;
  uint64_t disableCount = 0u;
  uint64_t cpuGeneratedPayloadBytes = 0u;
  uint32_t liveRuntimeObjects = 0u;
};

// Owns the renderer-independent V2 runtime for one application session. A
// runtime failure opens a sticky circuit breaker: later enable attempts in the
// same session remain on compact CPU rebuild requests. stop() releases all V2
// control-plane state, but deliberately does not clear the breaker.
class TerrainGPUV2Rollout
{
public:
  TerrainGPUV2Rollout() = default;
  ~TerrainGPUV2Rollout();
  TerrainGPUV2Rollout(const TerrainGPUV2Rollout &) = delete;
  TerrainGPUV2Rollout &operator=(const TerrainGPUV2Rollout &) = delete;

  bool start(
      const TerrainGPUV2DeviceCapabilities &device,
      const TerrainGPUV2RolloutSettings &settings = {},
      std::string *error = nullptr);
  void stop();
  void synchronizeRuntimeState();
  void recordDeviceLoss();

  TerrainGPUV2Runtime *runtime()
  {
    return runtime_.get();
  }
  const TerrainGPUV2Runtime *runtime() const
  {
    return runtime_.get();
  }
  bool GPUActive() const;
  bool circuitBreakerOpen() const
  {
    return circuitBreakerOpen_;
  }
  TerrainGPUV2RolloutFallback fallbackReason() const
  {
    return fallback_;
  }
  TerrainGPUV2RolloutDiagnostics diagnostics() const;
  static uint32_t liveRuntimeObjects();

private:
  static TerrainGPUV2RolloutFallback mapRuntimeFallback(
      TerrainGPUV2RuntimeFallback reason);
  void openCircuitBreaker(TerrainGPUV2RolloutFallback reason);

  std::unique_ptr<TerrainGPUV2Runtime> runtime_{};
  TerrainGPUV2RolloutSettings settings_{};
  TerrainGPUV2DeviceCapabilities device_{};
  TerrainGPUV2RolloutFallback fallback_ = TerrainGPUV2RolloutFallback::None;
  TerrainGPUV2RolloutFallback circuitBreakerReason_ =
      TerrainGPUV2RolloutFallback::None;
  bool started_ = false;
  bool circuitBreakerOpen_ = false;
  uint64_t enableCount_ = 0u;
  uint64_t disableCount_ = 0u;
  static std::atomic<uint32_t> liveRuntimeObjects_;
};

bool parseTerrainGPUV2RequestedMode(
    const std::string &value, TerrainGPUV2RequestedMode &mode);
const char *terrainGPUV2RequestedModeName(TerrainGPUV2RequestedMode mode);
const char *terrainGPUV2RolloutFallbackName(TerrainGPUV2RolloutFallback reason);
bool parseTerrainGPUV2ChunkShape(
    const std::string &value, TerrainGPUChunkShape &shape,
    std::string *error = nullptr);
bool parseTerrainGPUV2RasterHierarchyMode(
    const std::string &value, TerrainGPUV2RasterHierarchyMode &mode);
bool applyTerrainGPUV2BudgetOverrides(
    const TerrainGPUV2BudgetOverrides &overrides,
    TerrainGPUV2RuntimeSettings &settings, std::string *error = nullptr);

} // namespace voxel::svraster
