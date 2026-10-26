#include "voxel/svraster/TerrainGPUV2Rollout.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <vector>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool parsePositive(const std::string &value, uint32_t &result)
{
  if (value.empty())
    return false;
  uint32_t parsed = 0u;
  const auto converted = std::from_chars(
      value.data(), value.data() + value.size(), parsed);
  if (converted.ec != std::errc{} ||
      converted.ptr != value.data() + value.size() || parsed == 0u)
    return false;
  result = parsed;
  return true;
}
} // namespace

std::atomic<uint32_t> TerrainGPUV2Rollout::liveRuntimeObjects_{0u};

TerrainGPUV2Rollout::~TerrainGPUV2Rollout()
{
  stop();
}

TerrainGPUV2RolloutFallback TerrainGPUV2Rollout::mapRuntimeFallback(
    const TerrainGPUV2RuntimeFallback reason)
{
  switch (reason)
  {
  case TerrainGPUV2RuntimeFallback::None:
    return TerrainGPUV2RolloutFallback::None;
  case TerrainGPUV2RuntimeFallback::ExplicitCPU:
    return TerrainGPUV2RolloutFallback::ExplicitCPU;
  case TerrainGPUV2RuntimeFallback::UnqualifiedDevice:
    return TerrainGPUV2RolloutFallback::UnqualifiedDevice;
  case TerrainGPUV2RuntimeFallback::MissingCompute:
    return TerrainGPUV2RolloutFallback::MissingCompute;
  case TerrainGPUV2RuntimeFallback::InsufficientLimits:
    return TerrainGPUV2RolloutFallback::InsufficientLimits;
  case TerrainGPUV2RuntimeFallback::DeviceLost:
    return TerrainGPUV2RolloutFallback::DeviceLost;
  case TerrainGPUV2RuntimeFallback::RepeatedBuildFailure:
    return TerrainGPUV2RolloutFallback::RepeatedBuildFailure;
  case TerrainGPUV2RuntimeFallback::SustainedBudgetViolation:
    return TerrainGPUV2RolloutFallback::SustainedBudgetViolation;
  }
  return TerrainGPUV2RolloutFallback::RepeatedBuildFailure;
}

void TerrainGPUV2Rollout::openCircuitBreaker(
    const TerrainGPUV2RolloutFallback reason)
{
  if (reason == TerrainGPUV2RolloutFallback::None ||
      reason == TerrainGPUV2RolloutFallback::ExplicitCPU)
    return;
  if (!circuitBreakerOpen_)
    circuitBreakerReason_ = reason;
  circuitBreakerOpen_ = true;
  fallback_ = circuitBreakerReason_;
}

bool TerrainGPUV2Rollout::start(
    const TerrainGPUV2DeviceCapabilities &device,
    const TerrainGPUV2RolloutSettings &settings, std::string *error)
{
  if (started_)
    return fail(error, "terrain GPU V2 rollout is already started");

  settings_ = settings;
  device_ = device;
  ++enableCount_;

  TerrainGPUV2RuntimeSettings runtimeSettings = settings.runtime;
  TerrainGPUV2DeviceCapabilities runtimeDevice = device;
  TerrainGPUV2RolloutFallback selectionFallback =
      TerrainGPUV2RolloutFallback::None;
  bool requestGPU = settings.requestedMode != TerrainGPUV2RequestedMode::CPU;

  if (settings.requestedMode == TerrainGPUV2RequestedMode::CPU)
    selectionFallback = TerrainGPUV2RolloutFallback::ExplicitCPU;
  else if (circuitBreakerOpen_)
    selectionFallback = circuitBreakerReason_;
  else if (!device.compute)
    selectionFallback = TerrainGPUV2RolloutFallback::MissingCompute;
  else if (!device.qualified)
    selectionFallback = TerrainGPUV2RolloutFallback::UnqualifiedDevice;
  else if (!settings.pipelinesCreated)
    selectionFallback = TerrainGPUV2RolloutFallback::PipelineCreationFailure;
  else if (!settings.visualQualificationAvailable)
    selectionFallback = TerrainGPUV2RolloutFallback::VisualQualificationMissing;
  else if (!settings.visualQualification.qualified)
    selectionFallback = TerrainGPUV2RolloutFallback::VisualQualificationFailed;

  if (selectionFallback != TerrainGPUV2RolloutFallback::None)
  {
    if (settings.requestedMode == TerrainGPUV2RequestedMode::GPUV2)
    {
      fallback_ = selectionFallback;
      return fail(
          error,
          std::string("terrain GPU V2 requested but unavailable: ") +
              terrainGPUV2RolloutFallbackName(selectionFallback));
    }
    requestGPU = false;
    runtimeDevice.qualified = false;
    if (settings.requestedMode != TerrainGPUV2RequestedMode::CPU)
      openCircuitBreaker(selectionFallback);
  }
  runtimeSettings.requestGPU = requestGPU;

  runtime_ = std::make_unique<TerrainGPUV2Runtime>();
  ++liveRuntimeObjects_;
  if (!runtime_->initialize(runtimeDevice, runtimeSettings, error))
  {
    runtime_.reset();
    --liveRuntimeObjects_;
    return false;
  }
  if (settings.visualQualificationAvailable)
    runtime_->recordVisualQualification(settings.visualQualification);
  started_ = true;
  fallback_ = selectionFallback;
  if (fallback_ == TerrainGPUV2RolloutFallback::None)
    fallback_ = mapRuntimeFallback(runtime_->fallbackReason());
  synchronizeRuntimeState();
  if (settings.requestedMode == TerrainGPUV2RequestedMode::GPUV2 &&
      runtime_->mode() == TerrainGPUV2RuntimeMode::CPUFallback)
  {
    const TerrainGPUV2RolloutFallback reason = fallback_;
    stop();
    fallback_ = reason;
    return fail(
        error,
        std::string("terrain GPU V2 requested but runtime fell back: ") +
            terrainGPUV2RolloutFallbackName(reason));
  }
  if (error != nullptr)
    error->clear();
  return true;
}

void TerrainGPUV2Rollout::stop()
{
  if (!runtime_)
    return;
  runtime_.reset();
  --liveRuntimeObjects_;
  started_ = false;
  ++disableCount_;
}

void TerrainGPUV2Rollout::synchronizeRuntimeState()
{
  if (!runtime_)
    return;
  const auto reason = mapRuntimeFallback(runtime_->fallbackReason());
  if (runtime_->mode() == TerrainGPUV2RuntimeMode::CPUFallback &&
      reason != TerrainGPUV2RolloutFallback::None &&
      reason != TerrainGPUV2RolloutFallback::ExplicitCPU)
    openCircuitBreaker(reason);
  else if (!circuitBreakerOpen_ && fallback_ == TerrainGPUV2RolloutFallback::None)
    fallback_ = reason;
}

void TerrainGPUV2Rollout::recordDeviceLoss()
{
  if (runtime_)
    runtime_->recordDeviceLoss();
  openCircuitBreaker(TerrainGPUV2RolloutFallback::DeviceLost);
}

bool TerrainGPUV2Rollout::GPUActive() const
{
  return runtime_ != nullptr &&
      runtime_->mode() == TerrainGPUV2RuntimeMode::GPU &&
      !circuitBreakerOpen_;
}

TerrainGPUV2RolloutDiagnostics TerrainGPUV2Rollout::diagnostics() const
{
  TerrainGPUV2RolloutDiagnostics result{};
  result.requestedMode = settings_.requestedMode;
  result.activeMode = runtime_ ? runtime_->mode() :
      TerrainGPUV2RuntimeMode::CPUFallback;
  result.fallback = fallback_;
  result.started = started_;
  result.pipelinesCreated = settings_.pipelinesCreated;
  result.deviceQualified = device_.qualified;
  result.visualQualified = settings_.visualQualificationAvailable &&
      settings_.visualQualification.qualified;
  result.circuitBreakerOpen = circuitBreakerOpen_;
  result.enableCount = enableCount_;
  result.disableCount = disableCount_;
  // This must stay zero. Generated columns, occupancy, materials, and Tree64
  // nodes are leases in GPU arenas; CPU fallback requests contain keys only.
  result.cpuGeneratedPayloadBytes = 0u;
  result.liveRuntimeObjects = liveRuntimeObjects();
  return result;
}

uint32_t TerrainGPUV2Rollout::liveRuntimeObjects()
{
  return liveRuntimeObjects_.load();
}

bool parseTerrainGPUV2RequestedMode(
    const std::string &value, TerrainGPUV2RequestedMode &mode)
{
  if (value == "auto")
    mode = TerrainGPUV2RequestedMode::Auto;
  else if (value == "cpu")
    mode = TerrainGPUV2RequestedMode::CPU;
  else if (value == "gpu-v2")
    mode = TerrainGPUV2RequestedMode::GPUV2;
  else
    return false;
  return true;
}

const char *terrainGPUV2RequestedModeName(const TerrainGPUV2RequestedMode mode)
{
  switch (mode)
  {
  case TerrainGPUV2RequestedMode::Auto:
    return "auto";
  case TerrainGPUV2RequestedMode::CPU:
    return "cpu";
  case TerrainGPUV2RequestedMode::GPUV2:
    return "gpu-v2";
  }
  return "unknown";
}

const char *terrainGPUV2RolloutFallbackName(
    const TerrainGPUV2RolloutFallback reason)
{
  switch (reason)
  {
  case TerrainGPUV2RolloutFallback::None:
    return "none";
  case TerrainGPUV2RolloutFallback::ExplicitCPU:
    return "explicit-cpu";
  case TerrainGPUV2RolloutFallback::UnqualifiedDevice:
    return "unqualified-device";
  case TerrainGPUV2RolloutFallback::PipelineCreationFailure:
    return "pipeline-creation-failure";
  case TerrainGPUV2RolloutFallback::VisualQualificationMissing:
    return "visual-qualification-missing";
  case TerrainGPUV2RolloutFallback::VisualQualificationFailed:
    return "visual-qualification-failed";
  case TerrainGPUV2RolloutFallback::MissingCompute:
    return "missing-compute";
  case TerrainGPUV2RolloutFallback::InsufficientLimits:
    return "insufficient-limits";
  case TerrainGPUV2RolloutFallback::DeviceLost:
    return "device-lost";
  case TerrainGPUV2RolloutFallback::RepeatedBuildFailure:
    return "repeated-build-failure";
  case TerrainGPUV2RolloutFallback::SustainedBudgetViolation:
    return "sustained-budget-violation";
  case TerrainGPUV2RolloutFallback::SessionCircuitBreaker:
    return "session-circuit-breaker";
  }
  return "unknown";
}

bool parseTerrainGPUV2ChunkShape(
    const std::string &value, TerrainGPUChunkShape &shape,
    std::string *error)
{
  std::array<uint32_t, 3u> dimensions{};
  size_t begin = 0u;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const size_t separator = value.find_first_of("xX,", begin);
    const size_t end = axis == 2u ? value.size() : separator;
    if (end == std::string::npos ||
        !parsePositive(value.substr(begin, end - begin), dimensions[axis]))
      return fail(error, "terrain GPU chunk bricks must be X,Y,Z positive integers");
    if (axis != 2u)
    {
      if (separator == std::string::npos)
        return fail(error, "terrain GPU chunk bricks require three dimensions");
      begin = separator + 1u;
    }
    else if (separator != std::string::npos)
      return fail(error, "terrain GPU chunk bricks require exactly three dimensions");
  }
  if (dimensions[0] > std::numeric_limits<uint16_t>::max() ||
      dimensions[1] > std::numeric_limits<uint16_t>::max() ||
      dimensions[2] > std::numeric_limits<uint16_t>::max())
    return fail(error, "terrain GPU chunk brick dimensions exceed 16-bit addressing");
  TerrainGPUChunkShape parsed{
      static_cast<uint16_t>(dimensions[0]),
      static_cast<uint16_t>(dimensions[1]),
      static_cast<uint16_t>(dimensions[2]), 8u};
  TerrainGPUChunkShapeCounts counts{};
  if (!terrainGPUChunkShapeCounts(parsed, counts, error))
    return false;
  shape = parsed;
  if (error != nullptr)
    error->clear();
  return true;
}

bool parseTerrainGPUV2RasterHierarchyMode(
    const std::string &value, TerrainGPUV2RasterHierarchyMode &mode)
{
  if (value == "off")
    mode = TerrainGPUV2RasterHierarchyMode::Off;
  else if (value == "on")
    mode = TerrainGPUV2RasterHierarchyMode::On;
  else if (value == "auto")
    mode = TerrainGPUV2RasterHierarchyMode::Auto;
  else
    return false;
  return true;
}

bool applyTerrainGPUV2BudgetOverrides(
    const TerrainGPUV2BudgetOverrides &overrides,
    TerrainGPUV2RuntimeSettings &settings, std::string *error)
{
  auto &budget = settings.scheduler;
  if (overrides.targetGenerationMicroseconds)
    budget.targetGenerationMicroseconds =
        *overrides.targetGenerationMicroseconds;
  if (overrides.hardGenerationMicroseconds)
    budget.hardGenerationMicroseconds =
        *overrides.hardGenerationMicroseconds;
  if (overrides.maximumSingleDispatchMicroseconds)
    budget.maximumSingleDispatchMicroseconds =
        *overrides.maximumSingleDispatchMicroseconds;
  else if (overrides.hardGenerationMicroseconds)
    budget.maximumSingleDispatchMicroseconds = std::min(
        budget.maximumSingleDispatchMicroseconds,
        budget.hardGenerationMicroseconds);
  if (overrides.maximumChunksPerFrame)
    budget.maximumChunksPerFrame = *overrides.maximumChunksPerFrame;
  if (overrides.maximumNewChunksPerFrame)
    budget.maximumNewChunksPerFrame =
        *overrides.maximumNewChunksPerFrame;
  if (overrides.maximumWorkgroupsPerFrame)
    budget.maximumWorkgroupsPerFrame =
        *overrides.maximumWorkgroupsPerFrame;
  if (budget.targetGenerationMicroseconds == 0u ||
      budget.hardGenerationMicroseconds <
          budget.targetGenerationMicroseconds ||
      budget.maximumSingleDispatchMicroseconds == 0u ||
      budget.maximumSingleDispatchMicroseconds >
          budget.hardGenerationMicroseconds ||
      budget.maximumChunksPerFrame == 0u ||
      budget.maximumNewChunksPerFrame == 0u ||
      budget.maximumNewChunksPerFrame > budget.maximumChunksPerFrame ||
      budget.maximumWorkgroupsPerFrame == 0u)
    return fail(error, "terrain GPU V2 budget overrides are inconsistent");
  if (error != nullptr)
    error->clear();
  return true;
}

} // namespace voxel::svraster
