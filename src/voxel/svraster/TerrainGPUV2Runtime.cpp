#include "voxel/svraster/TerrainGPUV2Runtime.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

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

template <typename T> T percentile(std::vector<T> values, const double fraction)
{
  if (values.empty())
    return T{};
  std::sort(values.begin(), values.end());
  const size_t index = std::min(values.size() - 1u, static_cast<size_t>(std::ceil(values.size() * fraction)) - 1u);
  return values[index];
}

bool terminal(const TerrainGPUJobStage stage)
{
  return stage == TerrainGPUJobStage::Complete || stage == TerrainGPUJobStage::Cancelled || stage == TerrainGPUJobStage::Failed;
}

bool checkedAdd(const uint64_t left, const uint64_t right, uint64_t &result)
{
  if (left > std::numeric_limits<uint64_t>::max() - right)
    return false;
  result = left + right;
  return true;
}
} // namespace

struct TerrainGPUV2Runtime::JobRecord
{
  uint64_t id = 0u;
  TerrainGPUV2SpatialRequest request{};
  uint64_t queuedFrame = 0u;
  float errorPixels = 0.0f;
  float distanceMetres = 0.0f;
  bool fallbackQueued = false;
};

struct TerrainGPUV2Runtime::DispatchRecord
{
  uint64_t frameIndex = 0u;
  uint64_t jobId = 0u;
  uint32_t predictedMicroseconds = 0u;
};

TerrainGPUV2Runtime::TerrainGPUV2Runtime() = default;
TerrainGPUV2Runtime::~TerrainGPUV2Runtime() = default;

bool TerrainGPUV2Runtime::initialize(const TerrainGPUV2DeviceCapabilities &device, const TerrainGPUV2RuntimeSettings &settings, std::string *error)
{
  if (initialized_ || settings.maximumConsecutiveBuildFailures == 0u || settings.maximumConsecutiveBudgetViolations == 0u || settings.conservativeNoTimestampTargetMicroseconds == 0u ||
      settings.conservativeNoTimestampHardMicroseconds < settings.conservativeNoTimestampTargetMicroseconds || settings.conservativeNoTimestampChunksPerFrame == 0u)
    return fail(error, "terrain GPU V2 runtime settings are invalid");
  settings_ = settings;
  device_ = device;
  effectiveBudget_ = settings.scheduler;
  if (!device.timestampQueries)
  {
    effectiveBudget_.targetGenerationMicroseconds = std::min(effectiveBudget_.targetGenerationMicroseconds, settings.conservativeNoTimestampTargetMicroseconds);
    effectiveBudget_.hardGenerationMicroseconds = std::min(effectiveBudget_.hardGenerationMicroseconds, settings.conservativeNoTimestampHardMicroseconds);
    effectiveBudget_.minimumUrgentGenerationMicroseconds = std::min(
        effectiveBudget_.minimumUrgentGenerationMicroseconds,
        effectiveBudget_.targetGenerationMicroseconds);
    effectiveBudget_.maximumSingleDispatchMicroseconds = std::min(effectiveBudget_.maximumSingleDispatchMicroseconds, effectiveBudget_.hardGenerationMicroseconds);
    effectiveBudget_.maximumChunksPerFrame = std::min(effectiveBudget_.maximumChunksPerFrame, settings.conservativeNoTimestampChunksPerFrame);
    effectiveBudget_.maximumNewChunksPerFrame = std::min(effectiveBudget_.maximumNewChunksPerFrame, effectiveBudget_.maximumChunksPerFrame);
  }
  if (!scheduler_.configure(effectiveBudget_, error) || !residency_.configure(settings.residency, error))
    return false;
  initialized_ = true;
  mode_ = TerrainGPUV2RuntimeMode::GPU;
  fallback_ = TerrainGPUV2RuntimeFallback::None;
  if (!settings.requestGPU)
  {
    mode_ = TerrainGPUV2RuntimeMode::CPUFallback;
    fallback_ = TerrainGPUV2RuntimeFallback::ExplicitCPU;
  }
  else if (!device.compute)
  {
    mode_ = TerrainGPUV2RuntimeMode::CPUFallback;
    fallback_ = TerrainGPUV2RuntimeFallback::MissingCompute;
  }
  else if (!device.qualified)
  {
    mode_ = TerrainGPUV2RuntimeMode::CPUFallback;
    fallback_ = TerrainGPUV2RuntimeFallback::UnqualifiedDevice;
  }
  uint64_t requiredDeviceBytes = 0u;
  uint64_t requiredWithScratch = 0u;
  const bool validDeviceBudget =
      checkedAdd(settings.residency.hardBudgetBytes, effectiveBudget_.scratchCapacityBytes, requiredWithScratch) && checkedAdd(requiredWithScratch, effectiveBudget_.outputCapacityBytes, requiredDeviceBytes);
  if (mode_ == TerrainGPUV2RuntimeMode::GPU && (device.maximumComputeWorkGroupInvocations < 256u || device.maximumStorageBuffersPerStage < 8u || device.maximumStorageBufferBytes < effectiveBudget_.outputCapacityBytes ||
                                                !validDeviceBudget || device.deviceLocalMemoryBytes < requiredDeviceBytes))
  {
    mode_ = TerrainGPUV2RuntimeMode::CPUFallback;
    fallback_ = TerrainGPUV2RuntimeFallback::InsufficientLimits;
  }
  refreshTelemetry();
  if (error != nullptr)
    error->clear();
  return true;
}

void TerrainGPUV2Runtime::setWorldBundleRevision(const uint64_t revision)
{
  worldBundleRevision_ = revision;
  scheduler_.setWorldBundleRevision(revision);
}

TerrainGPUV2PriorityClass TerrainGPUV2Runtime::priority(const TerrainGPUV2Visibility visibility)
{
  switch (visibility)
  {
  case TerrainGPUV2Visibility::VisibleMissing:
    return TerrainGPUV2PriorityClass::VisibleMissing;
  case TerrainGPUV2Visibility::VisibleRefinement:
    return TerrainGPUV2PriorityClass::VisibleRefinement;
  case TerrainGPUV2Visibility::NearFrustumPrefetch:
    return TerrainGPUV2PriorityClass::NearFrustumPrefetch;
  case TerrainGPUV2Visibility::Offscreen:
    return TerrainGPUV2PriorityClass::Background;
  }
  return TerrainGPUV2PriorityClass::Background;
}

bool TerrainGPUV2Runtime::projectedError(const TerrainGPUV2SpatialRequest &request, const TerrainGPUV2View &view, float &errorPixels, float &distanceMetres)
{
  TerrainGPUChunkShapeCounts counts{};
  if (!terrainGPUChunkShapeCounts(request.shape, counts, nullptr) || request.voxelSizeMetres <= 0.0 || request.geometricErrorMetres <= 0.0 || view.viewportHeightPixels == 0u || !std::isfinite(view.verticalFovRadians) ||
      view.verticalFovRadians <= 0.0 || view.verticalFovRadians >= 3.141592653589793)
    return false;
  double squared = 0.0;
  const std::array<int32_t, 3u> chunk{request.key.chunkX, request.key.chunkY, request.key.chunkZ};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double extent = static_cast<double>(counts.voxelDimensions[axis]) * request.voxelSizeMetres;
    const double minimum = static_cast<double>(chunk[axis]) * extent;
    const double maximum = minimum + extent;
    const double delta = view.cameraMetres[axis] < minimum ? minimum - view.cameraMetres[axis] : view.cameraMetres[axis] > maximum ? view.cameraMetres[axis] - maximum : 0.0;
    squared += delta * delta;
  }
  const double distance = std::max(std::sqrt(squared), request.voxelSizeMetres * 0.01);
  const double pixelsPerRadian = static_cast<double>(view.viewportHeightPixels) / (2.0 * std::tan(view.verticalFovRadians * 0.5));
  const double projected = request.geometricErrorMetres * pixelsPerRadian / distance;
  if (!std::isfinite(projected) || projected < 0.0 || distance > std::numeric_limits<float>::max())
    return false;
  errorPixels = static_cast<float>(std::min(projected, static_cast<double>(std::numeric_limits<float>::max())));
  distanceMetres = static_cast<float>(distance);
  return true;
}

bool TerrainGPUV2Runtime::requestChunk(const TerrainGPUV2SpatialRequest &request, const TerrainGPUV2View &view, uint64_t &jobId, std::string *error)
{
  jobId = 0u;
  float errorPixels = 0.0f;
  float distanceMetres = 0.0f;
  if (!initialized_ || request.key.worldBundleRevision == 0u || request.key.worldBundleRevision != worldBundleRevision_ || !request.required || !projectedError(request, view, errorPixels, distanceMetres))
    return fail(error, "terrain GPU V2 runtime chunk request is invalid");
  const auto identity = TerrainGPUV2ChunkIdentity::from(request.key);
  const auto previous = jobsByIdentity_.find(identity);
  if (previous != jobsByIdentity_.end())
  {
    auto found = jobs_.find(previous->second);
    if (found != jobs_.end() && found->second.request.key.expectedGeneration == request.key.expectedGeneration)
    {
      found->second.request = request;
      found->second.errorPixels = errorPixels;
      found->second.distanceMetres = distanceMetres;
      scheduler_.updatePriority(found->second.id, priority(request.visibility), errorPixels, distanceMetres, true);
      jobId = found->second.id;
      return true;
    }
    if (found != jobs_.end())
      scheduler_.cancel(found->second.id);
    jobsByIdentity_.erase(previous);
  }
  JobRecord record{};
  record.request = request;
  record.queuedFrame = currentFrame_;
  record.errorPixels = errorPixels;
  record.distanceMetres = distanceMetres;
  scheduler_.setTerrainGeneration(request.key.terrainSlot, request.key.expectedGeneration);
  if (mode_ == TerrainGPUV2RuntimeMode::CPUFallback)
  {
    record.id = 0u;
    enqueueFallback(record, TerrainGPUV2Failure::None, fallback_);
    return true;
  }
  TerrainGPUV2Request scheduled{};
  scheduled.key = request.key;
  scheduled.shape = request.shape;
  scheduled.priorityClass = priority(request.visibility);
  scheduled.projectedErrorPixels = errorPixels;
  scheduled.distanceMetres = distanceMetres;
  scheduled.scratchBytes = request.scratchBytes;
  scheduled.outputReservationBytes = request.outputReservationBytes;
  scheduled.atlasUploadBytes = request.atlasUploadBytes;
  scheduled.atlasResident = request.atlasResident;
  scheduled.buildRasterParents = request.buildRasterParents;
  scheduled.required = request.required;
  if (!scheduler_.request(scheduled, record.id, error))
    return false;
  jobId = record.id;
  telemetry_.selectedShape = request.shape;
  jobs_.emplace(record.id, record);
  jobsByIdentity_[identity] = record.id;
  refreshTelemetry();
  return true;
}

bool TerrainGPUV2Runtime::requestRegion(const TerrainGPUV2RegionRequest &request, const TerrainGPUV2View &view, std::vector<uint64_t> &jobIds, std::string *error)
{
  jobIds.clear();
  const uint64_t horizontalCount = uint64_t{request.chunkCounts[0]} * request.chunkCounts[1];
  uint64_t count = 0u;
  const bool validCount = request.chunkCounts[2] == 0u || horizontalCount <= std::numeric_limits<uint64_t>::max() / request.chunkCounts[2];
  if (validCount)
    count = horizontalCount * request.chunkCounts[2];
  if (request.chunkCounts[0] == 0u || request.chunkCounts[1] == 0u || request.chunkCounts[2] == 0u || !validCount || count > std::numeric_limits<uint32_t>::max() ||
      request.terrainSlotBase > std::numeric_limits<uint32_t>::max() - count)
    return fail(error, "terrain GPU V2 runtime region is invalid");
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t maximum = int64_t{request.minimumChunk[axis]} + request.chunkCounts[axis] - 1u;
    if (maximum > std::numeric_limits<int32_t>::max())
      return fail(error, "terrain GPU V2 runtime region coordinates overflow");
  }
  maximumVerticalStack_ = std::max(maximumVerticalStack_, request.chunkCounts[1]);
  jobIds.reserve(static_cast<size_t>(count));
  for (uint32_t z = 0u; z < request.chunkCounts[2]; ++z)
    for (uint32_t y = 0u; y < request.chunkCounts[1]; ++y)
      for (uint32_t x = 0u; x < request.chunkCounts[0]; ++x)
      {
        TerrainGPUV2SpatialRequest chunk = request.chunk;
        chunk.key.chunkX = request.minimumChunk[0] + static_cast<int32_t>(x);
        chunk.key.chunkY = request.minimumChunk[1] + static_cast<int32_t>(y);
        chunk.key.chunkZ = request.minimumChunk[2] + static_cast<int32_t>(z);
        const uint64_t linear = (uint64_t{z} * request.chunkCounts[1] + y) * request.chunkCounts[0] + x;
        chunk.key.terrainSlot = request.terrainSlotBase + static_cast<uint32_t>(linear);
        uint64_t id = 0u;
        if (!requestChunk(chunk, view, id, error))
          return false;
        jobIds.push_back(id);
      }
  refreshTelemetry();
  return true;
}

void TerrainGPUV2Runtime::refreshPriorities(const TerrainGPUV2View &view)
{
  for (auto &[id, job] : jobs_)
  {
    const auto state = scheduler_.job(id);
    if (!state || terminal(state->stage))
      continue;
    float errorPixels = 0.0f;
    float distanceMetres = 0.0f;
    if (!projectedError(job.request, view, errorPixels, distanceMetres))
      continue;
    job.errorPixels = errorPixels;
    job.distanceMetres = distanceMetres;
    scheduler_.updatePriority(id, priority(job.request.visibility), errorPixels, distanceMetres, job.request.required);
  }
}

TerrainGPUV2FrameSchedule TerrainGPUV2Runtime::beginFrame(const TerrainGPUV2RuntimeFrameInput &input)
{
  currentFrame_ = std::max(currentFrame_, input.frameIndex);
  refreshPriorities(input.view);
  TerrainGPUV2FrameSchedule schedule{};
  schedule.frameIndex = input.frameIndex;
  if (mode_ == TerrainGPUV2RuntimeMode::GPU)
    schedule = scheduler_.scheduleFrame({input.frameIndex, input.completedFence, device_.profile, input.foregroundMicroseconds, input.frameTargetMicroseconds, input.publicationEnabled});
  else
    scheduler_.scheduleFrame({input.frameIndex, input.completedFence, device_.profile, input.frameTargetMicroseconds, input.frameTargetMicroseconds, false});
  for (const auto &dispatch : schedule.dispatches)
  {
    dispatches_[dispatch.id] = {input.frameIndex, dispatch.jobId, dispatch.predictedMicroseconds};
    const auto found = jobs_.find(dispatch.jobId);
    if (found != jobs_.end() && found->second.request.key.clipmapLOD == 0u && !found->second.fallbackQueued)
    {
      const uint32_t latency = static_cast<uint32_t>(std::min<uint64_t>(currentFrame_ - std::min(currentFrame_, found->second.queuedFrame), std::numeric_limits<uint32_t>::max()));
      if (!observedLOD0Refinement_ || latency < nearestLOD0LatencyFrames_)
        nearestLOD0LatencyFrames_ = latency;
      observedLOD0Refinement_ = true;
    }
  }
  refreshTelemetry(&schedule);
  return schedule;
}

bool TerrainGPUV2Runtime::completeAtlasUpload(const uint64_t jobId, const bool success, std::string *error)
{
  if (!scheduler_.completeAtlasUpload(jobId, success, error))
    return false;
  if (!success)
    recordFailure(jobId, TerrainGPUV2Failure::AtlasFailure);
  refreshTelemetry();
  return true;
}

void TerrainGPUV2Runtime::recordMeasured(const uint64_t frameIndex, const uint32_t microseconds)
{
  auto &total = measuredFrames_[frameIndex];
  total = total > std::numeric_limits<uint32_t>::max() - microseconds ? std::numeric_limits<uint32_t>::max() : total + microseconds;
  while (measuredFrames_.size() > 256u)
    measuredFrames_.erase(measuredFrames_.begin());
  consecutiveBudgetViolations_ = 0u;
  for (auto iterator = measuredFrames_.rbegin(); iterator != measuredFrames_.rend(); ++iterator)
  {
    if (iterator->second <= effectiveBudget_.hardGenerationMicroseconds)
      break;
    ++consecutiveBudgetViolations_;
  }
  if (consecutiveBudgetViolations_ >= settings_.maximumConsecutiveBudgetViolations)
    fallbackSession(TerrainGPUV2RuntimeFallback::SustainedBudgetViolation);
}

bool TerrainGPUV2Runtime::completeDispatch(const TerrainGPUV2DispatchCompletion &completion, std::string *error)
{
  const auto found = dispatches_.find(completion.dispatchId);
  if (found == dispatches_.end())
    return fail(error, "terrain GPU V2 runtime dispatch completion is unknown");
  const DispatchRecord record = found->second;
  auto submitted = completion;
  if (!device_.timestampQueries && !submitted.elapsedMicroseconds)
    submitted.elapsedMicroseconds = record.predictedMicroseconds;
  if (!scheduler_.completeDispatch(submitted, error))
    return false;
  if (submitted.elapsedMicroseconds)
  {
    recordMeasured(record.frameIndex, *submitted.elapsedMicroseconds);
    dispatches_.erase(found);
  }
  if (completion.failure != TerrainGPUV2Failure::None)
    recordFailure(record.jobId, completion.failure);
  refreshTelemetry();
  return true;
}

bool TerrainGPUV2Runtime::reportTimestamp(const uint64_t dispatchId, const uint32_t elapsedMicroseconds, std::string *error)
{
  const auto found = dispatches_.find(dispatchId);
  if (found == dispatches_.end() || !scheduler_.reportTiming(dispatchId, elapsedMicroseconds, error))
    return false;
  recordMeasured(found->second.frameIndex, elapsedMicroseconds);
  dispatches_.erase(found);
  refreshTelemetry();
  return true;
}

bool TerrainGPUV2Runtime::completePublication(const uint64_t jobId, const uint64_t completionFence, const bool accepted, std::string *error)
{
  const auto found = jobs_.find(jobId);
  if (found == jobs_.end() || !scheduler_.completePublication(jobId, completionFence, accepted, error))
    return false;
  const auto identity = TerrainGPUV2ChunkIdentity::from(found->second.request.key);
  const auto state = scheduler_.job(jobId);
  const bool committed = accepted && state && state->stage == TerrainGPUJobStage::Complete;
  if (committed)
  {
    ++acceptedPublications_;
    completedKeys_.push_back(found->second.request.key);
    consecutiveFailures_ = 0u;
  }
  else if (!accepted)
  {
    ++rejectedPublications_;
    recordFailure(jobId, TerrainGPUV2Failure::PublicationRejected);
  }
  jobsByIdentity_.erase(identity);
  jobs_.erase(found);
  refreshTelemetry();
  return true;
}

void TerrainGPUV2Runtime::enqueueFallback(const JobRecord &job, const TerrainGPUV2Failure failure, const TerrainGPUV2RuntimeFallback reason)
{
  const auto identity = TerrainGPUV2ChunkIdentity::from(job.request.key);
  TerrainGPUV2CPUFallbackRequest request{job.id, job.request.key, job.request.shape, failure, reason};
  const auto found = fallbackRequests_.find(identity);
  if (found == fallbackRequests_.end() || found->second.key.expectedGeneration <= request.key.expectedGeneration)
    fallbackRequests_[identity] = request;
  ++fallbackRequestCount_;
}

void TerrainGPUV2Runtime::recordFailure(const uint64_t jobId, const TerrainGPUV2Failure failure)
{
  if (failure == TerrainGPUV2Failure::None || failure == TerrainGPUV2Failure::Cancelled || failure == TerrainGPUV2Failure::StaleGeneration)
    return;
  const auto found = jobs_.find(jobId);
  if (found != jobs_.end() && !found->second.fallbackQueued)
  {
    enqueueFallback(found->second, failure, TerrainGPUV2RuntimeFallback::RepeatedBuildFailure);
    found->second.fallbackQueued = true;
  }
  ++consecutiveFailures_;
  if (consecutiveFailures_ >= settings_.maximumConsecutiveBuildFailures)
    fallbackSession(TerrainGPUV2RuntimeFallback::RepeatedBuildFailure);
}

void TerrainGPUV2Runtime::fallbackSession(const TerrainGPUV2RuntimeFallback reason)
{
  if (mode_ == TerrainGPUV2RuntimeMode::CPUFallback)
    return;
  mode_ = TerrainGPUV2RuntimeMode::CPUFallback;
  fallback_ = reason;
  for (auto &[id, job] : jobs_)
  {
    const auto state = scheduler_.job(id);
    if (state && !terminal(state->stage))
      scheduler_.cancel(id);
    if (!job.fallbackQueued)
    {
      enqueueFallback(job, state ? state->failure : TerrainGPUV2Failure::Cancelled, reason);
      job.fallbackQueued = true;
    }
  }
  refreshTelemetry();
}

void TerrainGPUV2Runtime::recordDeviceLoss()
{
  fallbackSession(TerrainGPUV2RuntimeFallback::DeviceLost);
}

void TerrainGPUV2Runtime::recordCapture(const TerrainGPUV2CaptureSample &sample)
{
  const uint32_t index = (sample.cameraMoving ? 1u : 0u) | (sample.temporalUpscaling ? 2u : 0u) | (sample.lighting ? 4u : 0u);
  captureMask_ |= 1u << index;
  rawRasterStable_ &= sample.rawRasterStable;
  lightingStable_ &= !sample.lighting || sample.lightingStable;
  refreshTelemetry();
}

void TerrainGPUV2Runtime::recordVisualQualification(const TerrainGPUV2StabilityReport &report)
{
  visualQualificationRecorded_ = true;
  visualQualificationPassed_ = report.qualified;
  rawRasterStable_ &= report.nonFinitePixels == 0u && report.blackCrackPixels == 0u && report.overlapPixels == 0u && report.tJunctionPixels == 0u && report.identityChanges == 0u && report.depthChanges == 0u &&
                      report.upscalerGeometryMismatches == 0u && report.upscalerHiddenDefects == 0u && report.rayOriginOffsetViolations == 0u && report.occupancyMismatches == 0u && report.closeRangeUnstablePixels == 0u;
  lightingStable_ &= report.shadowChanges == 0u;
  refreshTelemetry();
}

void TerrainGPUV2Runtime::recordFramePerformance(const uint32_t sourceFrameMicroseconds, const uint32_t referenceFrameMicroseconds)
{
  if (referenceFrameMicroseconds == 0u)
    return;
  framePerformancePermille_.push_back(static_cast<uint32_t>(std::min<uint64_t>(uint64_t{sourceFrameMicroseconds} * 1000u / referenceFrameMicroseconds, std::numeric_limits<uint32_t>::max())));
  if (framePerformancePermille_.size() > 256u)
    framePerformancePermille_.erase(framePerformancePermille_.begin());
  refreshTelemetry();
}

void TerrainGPUV2Runtime::setAtlasDiagnostics(const TerrainGPUV2WorldMapAtlasDiagnostics &diagnostics)
{
  atlasDiagnostics_ = diagnostics;
  refreshTelemetry();
}

void TerrainGPUV2Runtime::refreshTelemetry(const TerrainGPUV2FrameSchedule *schedule)
{
  telemetry_.mode = mode_;
  telemetry_.fallback = fallback_;
  telemetry_.timestampQueries = device_.timestampQueries;
  telemetry_.conservativeTiming = !device_.timestampQueries;
  telemetry_.frameIndex = currentFrame_;
  telemetry_.maximumVerticalStack = maximumVerticalStack_;
  telemetry_.nearestLOD0LatencyFrames = nearestLOD0LatencyFrames_;
  telemetry_.captureMatrixMask = captureMask_;
  telemetry_.rawRasterStable = rawRasterStable_;
  telemetry_.lightingStable = lightingStable_;
  telemetry_.visualQualificationRecorded = visualQualificationRecorded_;
  telemetry_.visualQualificationPassed = visualQualificationPassed_;
  telemetry_.CPUFallbackRequests = fallbackRequestCount_;
  telemetry_.acceptedPublications = acceptedPublications_;
  telemetry_.rejectedPublications = rejectedPublications_;
  telemetry_.jobsByStage.fill(0u);
  telemetry_.scheduler = scheduler_.diagnostics();
  telemetry_.residency = residency_.diagnostics();
  telemetry_.atlas = atlasDiagnostics_;
  telemetry_.rendererDrainCalls = telemetry_.scheduler.rendererDrainCalls;
  telemetry_.waitIdleCalls = telemetry_.scheduler.waitIdleCalls;
  std::vector<uint32_t> measured;
  for (const auto &[frame, value] : measuredFrames_)
    measured.push_back(value);
  telemetry_.measuredP99Microseconds = percentile(std::move(measured), 0.99);
  telemetry_.sourceFrameP95PermilleOfReference = percentile(framePerformancePermille_, 0.95);
  if (schedule != nullptr)
  {
    telemetry_.submittedDispatches = static_cast<uint32_t>(schedule->dispatches.size());
    telemetry_.submittedWorkgroups = schedule->workgroups;
    telemetry_.advancedChunks = schedule->advancedChunks;
    telemetry_.effectiveBudgetMicroseconds = schedule->effectiveTargetMicroseconds;
    telemetry_.predictedMicroseconds = schedule->predictedMicroseconds;
    telemetry_.remainingBudgetMicroseconds = schedule->predictedMicroseconds < schedule->effectiveTargetMicroseconds ? schedule->effectiveTargetMicroseconds - schedule->predictedMicroseconds : 0u;
  }
  struct Ranked
  {
    TerrainGPUV2PriorityClass priority{};
    float error = 0.0f;
    float distance = 0.0f;
  };
  auto rankedBefore = [](const Ranked &left, const Ranked &right)
  {
    if (left.priority != right.priority)
      return left.priority < right.priority;
    if (left.error != right.error)
      return left.error > right.error;
    return left.distance < right.distance;
  };
  std::optional<Ranked> nearest;
  for (const auto &[id, job] : jobs_)
  {
    const auto state = scheduler_.job(id);
    if (!state)
      continue;
    const uint32_t stageIndex = static_cast<uint32_t>(state->stage);
    if (stageIndex < telemetry_.jobsByStage.size())
      ++telemetry_.jobsByStage[stageIndex];
    if (terminal(state->stage))
      continue;
    const Ranked value{priority(job.request.visibility), job.errorPixels, job.distanceMetres};
    if (job.request.visibility != TerrainGPUV2Visibility::Offscreen && (!nearest || value.distance < nearest->distance))
      nearest = value;
  }
  telemetry_.nearestVisibleErrorPixels = nearest ? nearest->error : 0.0f;
  telemetry_.nearestVisibleDistanceMetres = nearest ? nearest->distance : 0.0f;
  telemetry_.nearestVisibleQueueRank = 0u;
  if (nearest)
  {
    uint32_t rank = 1u;
    for (const auto &[id, job] : jobs_)
    {
      const auto state = scheduler_.job(id);
      if (!state || terminal(state->stage))
        continue;
      const Ranked value{priority(job.request.visibility), job.errorPixels, job.distanceMetres};
      if (rankedBefore(value, *nearest))
        ++rank;
    }
    telemetry_.nearestVisibleQueueRank = rank;
  }
}

TerrainGPUV2RuntimeTelemetry TerrainGPUV2Runtime::telemetry() const
{
  return telemetry_;
}

std::string TerrainGPUV2Runtime::overlayText() const
{
  std::ostringstream stream;
  stream << "Terrain GPU V2: " << terrainGPUV2RuntimeModeName(telemetry_.mode) << " fallback=" << terrainGPUV2RuntimeFallbackName(telemetry_.fallback)
         << " timing=" << (telemetry_.conservativeTiming ? "conservative" : "timestamps") << " jobs=" << telemetry_.scheduler.queuedJobs << '/' << telemetry_.scheduler.activeJobs << '/'
         << telemetry_.scheduler.readyToPublish << " dispatches=" << telemetry_.submittedDispatches << " workgroups=" << telemetry_.submittedWorkgroups << " budget_us=" << telemetry_.predictedMicroseconds << '/'
         << telemetry_.effectiveBudgetMicroseconds << " p99_us=" << telemetry_.measuredP99Microseconds << " nearest=" << telemetry_.nearestVisibleErrorPixels << "px@" << telemetry_.nearestVisibleDistanceMetres
         << "m rank=" << telemetry_.nearestVisibleQueueRank << " resident_mib=" << static_cast<double>(telemetry_.residency.accountedBytes) / (1024.0 * 1024.0) << " hidden=" << telemetry_.residency.hiddenChunks
         << " retired=" << telemetry_.residency.retiredChunks << " atlas=" << telemetry_.atlas.readyTiles << '/' << telemetry_.atlas.waitingTiles << " faults=" << telemetry_.atlas.reportedFaults
         << " uploads=" << telemetry_.atlas.uploadedBytes << " publish=" << telemetry_.acceptedPublications << '/' << telemetry_.rejectedPublications << " visual="
         << (!telemetry_.visualQualificationRecorded ? "pending"
             : telemetry_.visualQualificationPassed  ? "pass"
                                                     : "fail")
         << " drains=" << telemetry_.rendererDrainCalls << '/' << telemetry_.waitIdleCalls;
  return stream.str();
}

bool TerrainGPUV2Runtime::qualificationReady() const
{
  return mode_ == TerrainGPUV2RuntimeMode::GPU && !measuredFrames_.empty() && !framePerformancePermille_.empty() && captureMask_ == 0xffu && rawRasterStable_ && lightingStable_ && visualQualificationRecorded_ &&
         visualQualificationPassed_ && telemetry_.measuredP99Microseconds <= effectiveBudget_.hardGenerationMicroseconds &&
         (telemetry_.sourceFrameP95PermilleOfReference == 0u || telemetry_.sourceFrameP95PermilleOfReference <= 1100u) && telemetry_.rendererDrainCalls == 0u && telemetry_.waitIdleCalls == 0u;
}

std::vector<TerrainGPUV2CPUFallbackRequest> TerrainGPUV2Runtime::takeCPUFallbackRequests()
{
  std::vector<TerrainGPUV2CPUFallbackRequest> result;
  for (const auto &[identity, request] : fallbackRequests_)
    result.push_back(request);
  fallbackRequests_.clear();
  return result;
}

const char *terrainGPUV2RuntimeModeName(const TerrainGPUV2RuntimeMode mode)
{
  return mode == TerrainGPUV2RuntimeMode::GPU ? "gpu" : "cpu-fallback";
}

const char *terrainGPUV2RuntimeFallbackName(const TerrainGPUV2RuntimeFallback reason)
{
  switch (reason)
  {
  case TerrainGPUV2RuntimeFallback::None:
    return "none";
  case TerrainGPUV2RuntimeFallback::ExplicitCPU:
    return "explicit-cpu";
  case TerrainGPUV2RuntimeFallback::UnqualifiedDevice:
    return "unqualified-device";
  case TerrainGPUV2RuntimeFallback::MissingCompute:
    return "missing-compute";
  case TerrainGPUV2RuntimeFallback::InsufficientLimits:
    return "insufficient-limits";
  case TerrainGPUV2RuntimeFallback::DeviceLost:
    return "device-lost";
  case TerrainGPUV2RuntimeFallback::RepeatedBuildFailure:
    return "repeated-build-failure";
  case TerrainGPUV2RuntimeFallback::SustainedBudgetViolation:
    return "sustained-budget-violation";
  }
  return "unknown";
}

} // namespace voxel::svraster
