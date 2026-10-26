#pragma once

#include "voxel/svraster/TerrainGPUV2Publication.hpp"
#include "voxel/svraster/TerrainGPUV2Scheduling.hpp"
#include "voxel/svraster/TerrainGPUV2Stability.hpp"
#include "voxel/svraster/TerrainGPUV2WorldMapAtlas.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace voxel::svraster
{

inline constexpr uint32_t kTerrainGPUV2RuntimeStageCount = static_cast<uint32_t>(TerrainGPUJobStage::Failed) + 1u;

enum class TerrainGPUV2RuntimeMode : uint32_t
{
  CPUFallback,
  GPU,
};

enum class TerrainGPUV2RuntimeFallback : uint32_t
{
  None,
  ExplicitCPU,
  UnqualifiedDevice,
  MissingCompute,
  InsufficientLimits,
  DeviceLost,
  RepeatedBuildFailure,
  SustainedBudgetViolation,
};

enum class TerrainGPUV2Visibility : uint32_t
{
  VisibleMissing,
  VisibleRefinement,
  NearFrustumPrefetch,
  Offscreen,
};

struct TerrainGPUV2DeviceCapabilities
{
  uint64_t profile = 0u;
  uint64_t deviceLocalMemoryBytes = 0u;
  uint64_t maximumStorageBufferBytes = 0u;
  uint32_t maximumStorageBuffersPerStage = 0u;
  uint32_t maximumComputeWorkGroupInvocations = 0u;
  bool compute = false;
  bool timestampQueries = false;
  bool qualified = false;
};

struct TerrainGPUV2RuntimeSettings
{
  TerrainGPUV2Budget scheduler{};
  TerrainGPUV2ResidencySettings residency{};
  bool requestGPU = true;
  uint32_t conservativeNoTimestampTargetMicroseconds = 500u;
  uint32_t conservativeNoTimestampHardMicroseconds = 1000u;
  uint32_t conservativeNoTimestampChunksPerFrame = 8u;
  uint32_t maximumConsecutiveBuildFailures = 3u;
  uint32_t maximumConsecutiveBudgetViolations = 5u;
};

struct TerrainGPUV2View
{
  std::array<double, 3u> cameraMetres{};
  uint32_t viewportHeightPixels = 1080u;
  double verticalFovRadians = 1.0471975511965976;
};

struct TerrainGPUV2SpatialRequest
{
  TerrainGPUChunkKey key{};
  TerrainGPUChunkShape shape{8u, 4u, 8u, 8u};
  TerrainGPUV2Visibility visibility = TerrainGPUV2Visibility::Offscreen;
  double voxelSizeMetres = 1.0;
  double geometricErrorMetres = 1.0;
  uint64_t scratchBytes = 0u;
  uint64_t outputReservationBytes = 0u;
  uint64_t atlasUploadBytes = 0u;
  bool atlasResident = false;
  bool buildRasterParents = false;
  bool required = true;
};

struct TerrainGPUV2RegionRequest
{
  TerrainGPUV2SpatialRequest chunk{};
  std::array<int32_t, 3u> minimumChunk{};
  std::array<uint32_t, 3u> chunkCounts{1u, 1u, 1u};
  uint32_t terrainSlotBase = 0u;
};

struct TerrainGPUV2RuntimeFrameInput
{
  uint64_t frameIndex = 0u;
  uint64_t completedFence = 0u;
  uint32_t foregroundMicroseconds = 0u;
  uint32_t frameTargetMicroseconds = 16667u;
  TerrainGPUV2View view{};
  bool publicationEnabled = true;
};

struct TerrainGPUV2CPUFallbackRequest
{
  uint64_t failedJobId = 0u;
  TerrainGPUChunkKey key{};
  TerrainGPUChunkShape shape{};
  TerrainGPUV2Failure failure = TerrainGPUV2Failure::None;
  TerrainGPUV2RuntimeFallback reason = TerrainGPUV2RuntimeFallback::None;
};

struct TerrainGPUV2CaptureSample
{
  bool cameraMoving = false;
  bool temporalUpscaling = false;
  bool lighting = false;
  bool rawRasterStable = true;
  bool lightingStable = true;
};

struct TerrainGPUV2RuntimeTelemetry
{
  TerrainGPUV2RuntimeMode mode = TerrainGPUV2RuntimeMode::CPUFallback;
  TerrainGPUV2RuntimeFallback fallback = TerrainGPUV2RuntimeFallback::None;
  bool timestampQueries = false;
  bool conservativeTiming = false;
  uint64_t frameIndex = 0u;
  TerrainGPUChunkShape selectedShape{};
  uint32_t maximumVerticalStack = 0u;
  uint32_t submittedDispatches = 0u;
  uint32_t submittedWorkgroups = 0u;
  uint32_t advancedChunks = 0u;
  uint32_t effectiveBudgetMicroseconds = 0u;
  uint32_t predictedMicroseconds = 0u;
  uint32_t remainingBudgetMicroseconds = 0u;
  uint32_t measuredP99Microseconds = 0u;
  uint32_t sourceFrameP95PermilleOfReference = 0u;
  float nearestVisibleErrorPixels = 0.0f;
  float nearestVisibleDistanceMetres = 0.0f;
  uint32_t nearestVisibleQueueRank = 0u;
  uint32_t nearestLOD0LatencyFrames = 0u;
  uint32_t captureMatrixMask = 0u;
  bool rawRasterStable = true;
  bool lightingStable = true;
  bool visualQualificationRecorded = false;
  bool visualQualificationPassed = false;
  uint64_t rendererDrainCalls = 0u;
  uint64_t waitIdleCalls = 0u;
  uint64_t CPUFallbackRequests = 0u;
  uint64_t acceptedPublications = 0u;
  uint64_t rejectedPublications = 0u;
  std::array<uint32_t, kTerrainGPUV2RuntimeStageCount> jobsByStage{};
  TerrainGPUV2Diagnostics scheduler{};
  TerrainGPUV2ResidencyDiagnostics residency{};
  TerrainGPUV2WorldMapAtlasDiagnostics atlas{};
};

class TerrainGPUV2Runtime
{
public:
  TerrainGPUV2Runtime();
  ~TerrainGPUV2Runtime();
  TerrainGPUV2Runtime(const TerrainGPUV2Runtime &) = delete;
  TerrainGPUV2Runtime &operator=(const TerrainGPUV2Runtime &) = delete;

  bool initialize(const TerrainGPUV2DeviceCapabilities &device, const TerrainGPUV2RuntimeSettings &settings = {}, std::string *error = nullptr);
  void setWorldBundleRevision(uint64_t revision);
  bool requestChunk(const TerrainGPUV2SpatialRequest &request, const TerrainGPUV2View &view, uint64_t &jobId, std::string *error = nullptr);
  bool requestRegion(const TerrainGPUV2RegionRequest &request, const TerrainGPUV2View &view, std::vector<uint64_t> &jobIds, std::string *error = nullptr);
  TerrainGPUV2FrameSchedule beginFrame(const TerrainGPUV2RuntimeFrameInput &input);
  bool completeAtlasUpload(uint64_t jobId, bool success, std::string *error = nullptr);
  bool completeDispatch(const TerrainGPUV2DispatchCompletion &completion, std::string *error = nullptr);
  bool reportTimestamp(uint64_t dispatchId, uint32_t elapsedMicroseconds, std::string *error = nullptr);
  bool completePublication(uint64_t jobId, uint64_t completionFence, bool accepted, std::string *error = nullptr);
  void recordDeviceLoss();
  void recordCapture(const TerrainGPUV2CaptureSample &sample);
  void recordVisualQualification(const TerrainGPUV2StabilityReport &report);
  void recordFramePerformance(uint32_t sourceFrameMicroseconds, uint32_t referenceFrameMicroseconds);
  void setAtlasDiagnostics(const TerrainGPUV2WorldMapAtlasDiagnostics &diagnostics);

  TerrainGPUV2RuntimeMode mode() const
  {
    return mode_;
  }
  TerrainGPUV2RuntimeFallback fallbackReason() const
  {
    return fallback_;
  }
  const TerrainGPUV2Budget &effectiveBudget() const
  {
    return effectiveBudget_;
  }
  TerrainGPUV2RuntimeTelemetry telemetry() const;
  std::string overlayText() const;
  bool qualificationReady() const;
  std::vector<TerrainGPUV2CPUFallbackRequest> takeCPUFallbackRequests();
  std::vector<TerrainGPUChunkKey> completedKeys() const
  {
    return completedKeys_;
  }
  TerrainGPUV2ResidencyManager &residency()
  {
    return residency_;
  }
  const TerrainGPUV2ResidencyManager &residency() const
  {
    return residency_;
  }

private:
  struct JobRecord;
  struct DispatchRecord;
  static TerrainGPUV2PriorityClass priority(TerrainGPUV2Visibility visibility);
  static bool projectedError(const TerrainGPUV2SpatialRequest &request, const TerrainGPUV2View &view, float &errorPixels, float &distanceMetres);
  void refreshPriorities(const TerrainGPUV2View &view);
  void refreshTelemetry(const TerrainGPUV2FrameSchedule *schedule = nullptr);
  void enqueueFallback(const JobRecord &job, TerrainGPUV2Failure failure, TerrainGPUV2RuntimeFallback reason);
  void fallbackSession(TerrainGPUV2RuntimeFallback reason);
  void recordFailure(uint64_t jobId, TerrainGPUV2Failure failure);
  void recordMeasured(uint64_t frameIndex, uint32_t microseconds);

  TerrainGPUV2RuntimeSettings settings_{};
  TerrainGPUV2Budget effectiveBudget_{};
  TerrainGPUV2DeviceCapabilities device_{};
  TerrainGPUV2Scheduler scheduler_{};
  TerrainGPUV2ResidencyManager residency_{};
  TerrainGPUV2RuntimeMode mode_ = TerrainGPUV2RuntimeMode::CPUFallback;
  TerrainGPUV2RuntimeFallback fallback_ = TerrainGPUV2RuntimeFallback::None;
  uint64_t worldBundleRevision_ = 0u;
  uint64_t currentFrame_ = 0u;
  uint32_t consecutiveFailures_ = 0u;
  uint32_t consecutiveBudgetViolations_ = 0u;
  uint32_t maximumVerticalStack_ = 0u;
  uint32_t nearestLOD0LatencyFrames_ = 0u;
  bool observedLOD0Refinement_ = false;
  uint32_t captureMask_ = 0u;
  bool rawRasterStable_ = true;
  bool lightingStable_ = true;
  bool visualQualificationRecorded_ = false;
  bool visualQualificationPassed_ = false;
  uint64_t fallbackRequestCount_ = 0u;
  uint64_t acceptedPublications_ = 0u;
  uint64_t rejectedPublications_ = 0u;
  TerrainGPUV2WorldMapAtlasDiagnostics atlasDiagnostics_{};
  std::map<uint64_t, JobRecord> jobs_{};
  std::map<TerrainGPUV2ChunkIdentity, uint64_t> jobsByIdentity_{};
  std::map<uint64_t, DispatchRecord> dispatches_{};
  std::map<TerrainGPUV2ChunkIdentity, TerrainGPUV2CPUFallbackRequest> fallbackRequests_{};
  std::map<uint64_t, uint32_t> measuredFrames_{};
  std::vector<uint32_t> framePerformancePermille_{};
  std::vector<TerrainGPUChunkKey> completedKeys_{};
  TerrainGPUV2RuntimeTelemetry telemetry_{};
  bool initialized_ = false;
};

const char *terrainGPUV2RuntimeModeName(TerrainGPUV2RuntimeMode mode);
const char *terrainGPUV2RuntimeFallbackName(TerrainGPUV2RuntimeFallback reason);

} // namespace voxel::svraster
