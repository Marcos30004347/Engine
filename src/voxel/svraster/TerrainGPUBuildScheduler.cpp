#include "voxel/svraster/TerrainGPUBuildScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

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

uint64_t alignUp(const uint64_t value, const uint64_t alignment)
{
  if (alignment == 0u)
    return value;
  const uint64_t remainder = value % alignment;
  return remainder == 0u ? value : value + alignment - remainder;
}

bool terminalStage(const TerrainGPUBuildStage stage)
{
  return stage == TerrainGPUBuildStage::Published ||
      stage == TerrainGPUBuildStage::Cancelled ||
      stage == TerrainGPUBuildStage::Failed;
}

uint32_t stageTotalUnits(const TerrainGPUBuildStage stage)
{
  switch (stage)
  {
  case TerrainGPUBuildStage::SampleColumns: return 64u;
  case TerrainGPUBuildStage::FinalizeBoundaryStitches: return 1u;
  case TerrainGPUBuildStage::CountFineBricks: return 1024u;
  case TerrainGPUBuildStage::CompactFineBricks: return 4u;
  case TerrainGPUBuildStage::EmitFineBricks: return 1024u;
  case TerrainGPUBuildStage::ReduceBrickLevel: return 4u;
  case TerrainGPUBuildStage::EmitBrickLevel: return 4u;
  case TerrainGPUBuildStage::BuildTree64Level: return 4u;
  case TerrainGPUBuildStage::FinalizeSidecars: return 64u;
  case TerrainGPUBuildStage::Validate: return 1u;
  default: return 0u;
  }
}

uint32_t stageQuantum(
    const TerrainGPUBuildStage stage, const TerrainGPUBuildBudget &budget)
{
  switch (stage)
  {
  case TerrainGPUBuildStage::SampleColumns:
    return budget.sampleColumnQuantum;
  case TerrainGPUBuildStage::FinalizeSidecars:
    return budget.sidecarQuantum;
  case TerrainGPUBuildStage::CountFineBricks:
  case TerrainGPUBuildStage::EmitFineBricks:
    return budget.fineBrickQuantum;
  default: return 1u;
  }
}

TerrainGPUBuildStage nextStage(const TerrainGPUBuildStage stage)
{
  switch (stage)
  {
  case TerrainGPUBuildStage::SampleColumns:
    return TerrainGPUBuildStage::FinalizeBoundaryStitches;
  case TerrainGPUBuildStage::FinalizeBoundaryStitches:
    return TerrainGPUBuildStage::CountFineBricks;
  case TerrainGPUBuildStage::CountFineBricks:
    return TerrainGPUBuildStage::CompactFineBricks;
  case TerrainGPUBuildStage::CompactFineBricks:
    return TerrainGPUBuildStage::EmitFineBricks;
  case TerrainGPUBuildStage::EmitFineBricks:
    return TerrainGPUBuildStage::ReduceBrickLevel;
  case TerrainGPUBuildStage::ReduceBrickLevel:
    return TerrainGPUBuildStage::EmitBrickLevel;
  case TerrainGPUBuildStage::EmitBrickLevel:
    return TerrainGPUBuildStage::BuildTree64Level;
  case TerrainGPUBuildStage::BuildTree64Level:
    return TerrainGPUBuildStage::FinalizeSidecars;
  case TerrainGPUBuildStage::FinalizeSidecars:
    return TerrainGPUBuildStage::Validate;
  case TerrainGPUBuildStage::Validate:
    return TerrainGPUBuildStage::ReadyToPublish;
  default: return stage;
  }
}

uint32_t coldEstimate(const TerrainGPUBuildStage stage)
{
  switch (stage)
  {
  case TerrainGPUBuildStage::FinalizeBoundaryStitches:
  case TerrainGPUBuildStage::Validate: return 30u;
  case TerrainGPUBuildStage::CompactFineBricks: return 45u;
  case TerrainGPUBuildStage::ReduceBrickLevel:
  case TerrainGPUBuildStage::EmitBrickLevel:
  case TerrainGPUBuildStage::BuildTree64Level: return 70u;
  default: return 55u;
  }
}

} // namespace

TerrainGPUFixedScratchAllocator::TerrainGPUFixedScratchAllocator(
    const uint64_t capacityBytes)
{
  reset(capacityBytes);
}

void TerrainGPUFixedScratchAllocator::reset(const uint64_t capacityBytes)
{
  capacityBytes_ = capacityBytes;
  nextId_ = 1u;
  allocatedBytes_ = 0u;
  highWatermarkBytes_ = 0u;
  allocationFailures_ = 0u;
  invalidReleaseAttempts_ = 0u;
  allocations_.clear();
  free_.clear();
  if (capacityBytes != 0u)
    free_.push_back({0u, capacityBytes});
}

std::optional<TerrainGPUScratchHandle>
TerrainGPUFixedScratchAllocator::allocate(
    const uint64_t bytes, const uint64_t alignment)
{
  if (bytes == 0u || alignment == 0u ||
      (alignment & (alignment - 1u)) != 0u)
  {
    ++allocationFailures_;
    return std::nullopt;
  }
  for (size_t index = 0u; index < free_.size(); ++index)
  {
    const Block block = free_[index];
    const uint64_t start = alignUp(block.offset, alignment);
    if (start < block.offset || start - block.offset > block.size ||
        bytes > block.size - (start - block.offset))
      continue;
    const uint64_t end = start + bytes;
    free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(index));
    if (start != block.offset)
      free_.push_back({block.offset, start - block.offset});
    const uint64_t blockEnd = block.offset + block.size;
    if (end != blockEnd)
      free_.push_back({end, blockEnd - end});
    std::sort(free_.begin(), free_.end(), [](const Block &a, const Block &b)
    {
      return a.offset < b.offset;
    });
    TerrainGPUScratchHandle handle{nextId_++, start, bytes};
    allocations_.emplace(handle.id, Allocation{handle, false, 0u});
    allocatedBytes_ += bytes;
    highWatermarkBytes_ = std::max(highWatermarkBytes_, allocatedBytes_);
    return handle;
  }
  ++allocationFailures_;
  return std::nullopt;
}

bool TerrainGPUFixedScratchAllocator::releaseDeferred(
    const TerrainGPUScratchHandle handle, const uint64_t fenceValue)
{
  const auto found = allocations_.find(handle.id);
  if (!handle.valid() || found == allocations_.end() ||
      found->second.handle.offset != handle.offset ||
      found->second.handle.size != handle.size || found->second.deferred)
  {
    ++invalidReleaseAttempts_;
    return false;
  }
  found->second.deferred = true;
  found->second.fenceValue = fenceValue;
  return true;
}

uint32_t TerrainGPUFixedScratchAllocator::reclaim(
    const uint64_t completedFenceValue)
{
  uint32_t count = 0u;
  for (auto iterator = allocations_.begin(); iterator != allocations_.end();)
  {
    if (!iterator->second.deferred ||
        iterator->second.fenceValue > completedFenceValue)
    {
      ++iterator;
      continue;
    }
    insertFree({iterator->second.handle.offset, iterator->second.handle.size});
    allocatedBytes_ -= iterator->second.handle.size;
    iterator = allocations_.erase(iterator);
    ++count;
  }
  return count;
}

void TerrainGPUFixedScratchAllocator::insertFree(Block block)
{
  free_.push_back(block);
  std::sort(free_.begin(), free_.end(), [](const Block &a, const Block &b)
  {
    return a.offset < b.offset;
  });
  std::vector<Block> merged;
  for (const Block candidate : free_)
  {
    if (!merged.empty() &&
        merged.back().offset + merged.back().size == candidate.offset)
      merged.back().size += candidate.size;
    else
      merged.push_back(candidate);
  }
  free_ = std::move(merged);
}

TerrainGPUScratchStats TerrainGPUFixedScratchAllocator::stats() const
{
  TerrainGPUScratchStats result{};
  result.capacityBytes = capacityBytes_;
  result.allocatedBytes = allocatedBytes_;
  result.highWatermarkBytes = highWatermarkBytes_;
  result.liveAllocations = static_cast<uint32_t>(allocations_.size());
  result.deferredAllocations = static_cast<uint32_t>(std::count_if(
      allocations_.begin(), allocations_.end(), [](const auto &entry)
      { return entry.second.deferred; }));
  result.allocationFailures = allocationFailures_;
  result.invalidReleaseAttempts = invalidReleaseAttempts_;
  return result;
}

struct TerrainGPUBuildScheduler::JobRecord
{
  TerrainGPUBuildJob publicJob{};
  TerrainGPUBuildRequest request{};
  TerrainGPUBuildResult result{};
  uint64_t sequence = 0u;
  uint64_t queuedFrame = 0u;
  uint64_t uploadedBytes = 0u;
  uint64_t lastFence = 0u;
  uint64_t activeDispatch = 0u;
  bool publicationPending = false;
  bool cancelRequested = false;
  bool releaseQueued = false;
};

struct TerrainGPUBuildScheduler::DispatchRecord
{
  TerrainGPUDispatch dispatch{};
  uint64_t completionFence = 0u;
  bool completed = false;
  bool timingReported = false;
};

struct TerrainGPUBuildScheduler::TimingEstimator
{
  uint32_t sampleCount = 0u;
  double mean = 0.0;
  uint32_t highWater = 0u;
  std::vector<uint32_t> recent{};

  void push(const uint32_t value)
  {
    mean = sampleCount == 0u ? static_cast<double>(value)
        : mean * 0.8 + static_cast<double>(value) * 0.2;
    ++sampleCount;
    highWater = std::max(value, static_cast<uint32_t>(highWater * 0.98));
    recent.push_back(value);
    if (recent.size() > 32u)
      recent.erase(recent.begin());
  }

  uint32_t estimate(const uint32_t cold) const
  {
    if (sampleCount == 0u)
      return cold;
    auto ordered = recent;
    std::sort(ordered.begin(), ordered.end());
    const size_t p90 = std::min(ordered.size() - 1u,
        static_cast<size_t>(std::ceil(ordered.size() * 0.9)) - 1u);
    return std::max({1u, static_cast<uint32_t>(std::ceil(mean)),
        ordered[p90], highWater});
  }

  uint32_t p95() const
  {
    if (recent.empty()) return 0u;
    auto ordered = recent;
    std::sort(ordered.begin(), ordered.end());
    const size_t index = std::min(ordered.size() - 1u,
        static_cast<size_t>(std::ceil(ordered.size() * 0.95)) - 1u);
    return ordered[index];
  }
};

TerrainGPUBuildScheduler::TerrainGPUBuildScheduler(
    const TerrainGPUBuildBudget budget)
{
  configure(budget);
}

TerrainGPUBuildScheduler::~TerrainGPUBuildScheduler() = default;

bool TerrainGPUBuildScheduler::configure(
    const TerrainGPUBuildBudget &budget, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (budget.admissionCeilingMicroseconds == 0u ||
      budget.maximumDispatchesPerFrame == 0u ||
      budget.maximumPagesStartedPerFrame == 0u ||
      budget.maximumPagesPublishedPerFrame == 0u ||
      budget.maximumScratchBytes == 0u ||
      budget.maximumInFlightJobs == 0u ||
      budget.sampleColumnQuantum == 0u ||
      budget.fineBrickQuantum == 0u ||
      budget.sidecarQuantum == 0u ||
      budget.coldEstimateScalePermille == 0u ||
      budget.maximumQuantumMicroseconds == 0u ||
      budget.targetGPUTimeMicroseconds >
          budget.admissionCeilingMicroseconds)
    return fail(error, "GPU terrain scheduler limits are invalid");
  budget_ = budget;
  scratch_.reset(budget.maximumScratchBytes);
  jobs_.clear();
  dispatches_.clear();
  timings_.clear();
  terrainGenerations_.clear();
  diagnostics_ = {};
  bundleSignature_ = 0u;
  nextJobId_ = nextSequence_ = nextDispatchId_ = 1u;
  lastFrameIndex_ = lastCompletedFence_ = 0u;
  gpuDebtMicroseconds_ = 0u;
  return true;
}

void TerrainGPUBuildScheduler::setBundleSignature(
    const uint64_t bundleSignature)
{
  bundleSignature_ = bundleSignature;
}

void TerrainGPUBuildScheduler::setTerrainGeneration(
    const uint32_t terrainSlot, const uint32_t generation)
{
  terrainGenerations_[terrainSlot] = generation;
}

bool TerrainGPUBuildScheduler::request(
    const TerrainGPUBuildRequest &request, uint64_t &jobId,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  jobId = 0u;
  if (request.key.bundleSignature == 0u ||
      request.key.generatorRevision == 0u ||
      request.key.expectedTerrainGeneration == 0u ||
      request.scratchBytes == 0u ||
      request.scratchBytes > budget_.maximumScratchBytes ||
      request.semanticRevision == 0u ||
      !std::isfinite(request.projectedError) ||
      !std::isfinite(request.distance) || request.distance < 0.0f)
    return fail(error, "GPU terrain request is invalid");
  for (auto &[id, existing] : jobs_)
  {
    if (!terminalStage(existing.publicJob.stage) &&
        existing.publicJob.key == request.key)
    {
      existing.request.jobClass = std::min(
          existing.request.jobClass, request.jobClass);
      existing.request.projectedError = std::max(
          existing.request.projectedError, request.projectedError);
      existing.request.distance = std::min(
          existing.request.distance, request.distance);
      if (request.mapsReady)
      {
        existing.request.mapsReady = true;
        existing.request.columnUploadBytes = request.columnUploadBytes;
      }
      jobId = id;
      return true;
    }
  }
  JobRecord record{};
  record.publicJob.id = nextJobId_++;
  record.publicJob.key = request.key;
  record.publicJob.stage = request.mapsReady
      ? TerrainGPUBuildStage::UploadColumns
      : TerrainGPUBuildStage::WaitingForMaps;
  record.request = request;
  record.sequence = nextSequence_++;
  record.queuedFrame = lastFrameIndex_;
  record.result.terrainGeneration = request.key.expectedTerrainGeneration;
  record.result.semanticRevision = request.semanticRevision;
  jobs_.emplace(record.publicJob.id, record);
  jobId = record.publicJob.id;
  refreshDiagnostics();
  return true;
}

bool TerrainGPUBuildScheduler::markMapsReady(
    const uint64_t jobId, const uint64_t columnUploadBytes,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto found = jobs_.find(jobId);
  if (found == jobs_.end() ||
      found->second.publicJob.stage != TerrainGPUBuildStage::WaitingForMaps)
    return fail(error, "GPU terrain job is not waiting for maps");
  found->second.request.mapsReady = true;
  found->second.request.columnUploadBytes = columnUploadBytes;
  found->second.publicJob.stage = TerrainGPUBuildStage::UploadColumns;
  found->second.publicJob.stageCursor = 0u;
  return true;
}

bool TerrainGPUBuildScheduler::current(const JobRecord &job) const
{
  const auto generation = terrainGenerations_.find(
      job.publicJob.key.terrainSlot);
  return bundleSignature_ != 0u &&
      job.publicJob.key.bundleSignature == bundleSignature_ &&
      generation != terrainGenerations_.end() &&
      generation->second == job.publicJob.key.expectedTerrainGeneration;
}

void TerrainGPUBuildScheduler::terminal(
    JobRecord &job, const TerrainGPUBuildStage stage,
    const TerrainGPUFailure failure, const uint64_t releaseFence)
{
  if (terminalStage(job.publicJob.stage))
    return;
  const TerrainGPUBuildStage failedAt = job.publicJob.stage;
  job.publicJob.stage = stage;
  job.result.status = failure;
  if (failure != TerrainGPUFailure::None)
    job.result.overflowStage = failedAt;
  if (job.publicJob.scratch.valid() && !job.releaseQueued)
  {
    scratch_.releaseDeferred(job.publicJob.scratch,
        std::max(releaseFence, job.lastFence));
    job.releaseQueued = true;
  }
  if (stage == TerrainGPUBuildStage::Cancelled)
    ++diagnostics_.cancelledJobsTotal;
  else if (stage == TerrainGPUBuildStage::Failed)
    ++diagnostics_.failedJobsTotal;
}

bool TerrainGPUBuildScheduler::cancel(const uint64_t jobId)
{
  const auto found = jobs_.find(jobId);
  if (found == jobs_.end() || terminalStage(found->second.publicJob.stage))
    return false;
  if (found->second.activeDispatch != 0u ||
      found->second.publicationPending)
    found->second.cancelRequested = true;
  else
    terminal(found->second, TerrainGPUBuildStage::Cancelled,
        TerrainGPUFailure::None, lastCompletedFence_);
  refreshDiagnostics();
  return true;
}

uint32_t TerrainGPUBuildScheduler::cancelTerrainSlot(
    const uint32_t terrainSlot)
{
  uint32_t count = 0u;
  for (auto &[id, record] : jobs_)
    if (record.publicJob.key.terrainSlot == terrainSlot && cancel(id))
      ++count;
  return count;
}

uint32_t TerrainGPUBuildScheduler::cancelAll()
{
  uint32_t count = 0u;
  for (auto &[id, record] : jobs_)
  {
    if (terminalStage(record.publicJob.stage) || record.cancelRequested)
      continue;
    record.cancelRequested = true;
    ++count;
    if (record.activeDispatch == 0u && !record.publicationPending)
      terminal(record, TerrainGPUBuildStage::Cancelled,
          TerrainGPUFailure::None, lastCompletedFence_);
  }
  refreshDiagnostics();
  return count;
}

void TerrainGPUBuildScheduler::cancelStale(const uint64_t releaseFence)
{
  for (auto &[id, record] : jobs_)
  {
    if (terminalStage(record.publicJob.stage) || current(record))
      continue;
    if (record.activeDispatch != 0u || record.publicationPending)
    {
      if (!record.cancelRequested)
        ++diagnostics_.staleInFlightCompletions;
      record.cancelRequested = true;
      record.result.status = TerrainGPUFailure::StaleGeneration;
    }
    else
    {
      ++diagnostics_.staleQueuedJobs;
      terminal(record, TerrainGPUBuildStage::Cancelled,
          TerrainGPUFailure::StaleGeneration, releaseFence);
    }
  }
}

uint32_t TerrainGPUBuildScheduler::effectiveTarget(
    const TerrainGPUFrameInput &input) const
{
  if (budget_.targetGPUTimeMicroseconds == 0u ||
      input.frameTargetMicroseconds == 0u)
    return 0u;
  const double load = static_cast<double>(input.foregroundGPUTimeMicroseconds) /
      input.frameTargetMicroseconds;
  if (load >= 0.95)
    return 0u;
  if (load <= 0.85)
    return budget_.targetGPUTimeMicroseconds;
  const double scale = (0.95 - load) / 0.10;
  return static_cast<uint32_t>(std::floor(
      budget_.targetGPUTimeMicroseconds * std::clamp(scale, 0.0, 1.0)));
}

uint32_t TerrainGPUBuildScheduler::estimate(
    const TerrainGPUBuildStage stage) const
{
  const uint32_t cold = std::max(1u, static_cast<uint32_t>(
      uint64_t{coldEstimate(stage)} * budget_.coldEstimateScalePermille /
      1000u));
  const auto found = timings_.find(stage);
  return found == timings_.end() ? cold : found->second.estimate(cold);
}

TerrainGPUFrameSchedule TerrainGPUBuildScheduler::scheduleFrame(
    const TerrainGPUFrameInput &input)
{
  lastFrameIndex_ = input.frameIndex;
  lastCompletedFence_ = std::max(lastCompletedFence_,
      input.completedFenceValue);
  scratch_.reclaim(lastCompletedFence_);
  cancelStale(lastCompletedFence_);
  const size_t timingCapacity = std::max<size_t>(
      32u, size_t{budget_.maximumInFlightJobs} * 8u);
  while (dispatches_.size() > timingCapacity)
  {
    const auto expired = std::find_if(dispatches_.begin(), dispatches_.end(),
        [](const auto &entry)
        { return entry.second.completed && !entry.second.timingReported; });
    if (expired == dispatches_.end())
      break;
    dispatches_.erase(expired);
    ++diagnostics_.droppedTimingSamples;
  }

  TerrainGPUFrameSchedule schedule{};
  schedule.frameIndex = input.frameIndex;
  schedule.effectiveTargetMicroseconds = effectiveTarget(input);
  if (schedule.effectiveTargetMicroseconds == 0u)
    ++diagnostics_.pressureZeroFrames;
  else
    gpuDebtMicroseconds_ = gpuDebtMicroseconds_ >
        schedule.effectiveTargetMicroseconds
      ? gpuDebtMicroseconds_ - schedule.effectiveTargetMicroseconds : 0u;

  uint32_t pagesStarted = 0u;
  uint64_t uploadBytes = 0u;

  std::vector<JobRecord *> ordered;
  ordered.reserve(jobs_.size());
  for (auto &[id, record] : jobs_)
    if (!terminalStage(record.publicJob.stage) &&
        !record.cancelRequested && current(record))
      ordered.push_back(&record);
  std::sort(ordered.begin(), ordered.end(), [&](const JobRecord *a,
      const JobRecord *b)
  {
    const auto ageA = input.frameIndex >= a->queuedFrame
        ? input.frameIndex - a->queuedFrame : 0u;
    const auto ageB = input.frameIndex >= b->queuedFrame
        ? input.frameIndex - b->queuedFrame : 0u;
    if (a->request.jobClass != b->request.jobClass)
      return a->request.jobClass < b->request.jobClass;
    const bool visibleClass =
        a->request.jobClass == TerrainGPUJobClass::VisibleMissing ||
        a->request.jobClass == TerrainGPUJobClass::VisibleRefinement;
    const bool starvedA = visibleClass &&
        ageA >= budget_.starvationAgeFrames;
    const bool starvedB = visibleClass &&
        ageB >= budget_.starvationAgeFrames;
    if (starvedA != starvedB)
      return starvedA;
    if (a->request.jobClass == TerrainGPUJobClass::VisibleMissing &&
        a->publicJob.key.internalLOD != b->publicJob.key.internalLOD)
      return a->publicJob.key.internalLOD > b->publicJob.key.internalLOD;
    if (a->request.jobClass == TerrainGPUJobClass::VisibleRefinement &&
        a->request.projectedError != b->request.projectedError)
      return a->request.projectedError > b->request.projectedError;
    if (a->request.distance != b->request.distance)
      return a->request.distance < b->request.distance;
    if (ageA != ageB)
      return ageA > ageB;
    if (!(a->publicJob.key == b->publicJob.key))
      return a->publicJob.key < b->publicJob.key;
    return a->sequence < b->sequence;
  });

  uint32_t residentJobs = static_cast<uint32_t>(std::count_if(
      jobs_.begin(), jobs_.end(), [](const auto &entry)
      { return entry.second.publicJob.scratch.valid() &&
          !entry.second.releaseQueued; }));

  for (JobRecord *record : ordered)
  {
    if (record->publicJob.stage == TerrainGPUBuildStage::WaitingForMaps)
      continue;
    if (!record->publicJob.scratch.valid())
    {
      if (pagesStarted >= budget_.maximumPagesStartedPerFrame ||
          residentJobs >= budget_.maximumInFlightJobs)
        continue;
      auto allocation = scratch_.allocate(record->request.scratchBytes);
      if (!allocation)
      {
        ++diagnostics_.scratchLimitedStarts;
        continue;
      }
      record->publicJob.scratch = *allocation;
      ++pagesStarted;
      ++residentJobs;
    }

    if (record->publicJob.stage == TerrainGPUBuildStage::UploadColumns)
    {
      const uint64_t remaining = record->request.columnUploadBytes -
          std::min(record->uploadedBytes, record->request.columnUploadBytes);
      const uint64_t capacity = budget_.maximumMapUploadBytesPerFrame -
          std::min(uploadBytes, budget_.maximumMapUploadBytesPerFrame);
      const uint64_t bytes = std::min(remaining, capacity);
      if (bytes != 0u)
      {
        schedule.uploads.push_back({record->publicJob.id,
            record->uploadedBytes, bytes});
        record->uploadedBytes += bytes;
        uploadBytes += bytes;
        ++diagnostics_.admittedUploads;
        diagnostics_.admittedUploadBytes += bytes;
      }
      if (record->uploadedBytes == record->request.columnUploadBytes)
      {
        record->publicJob.stage = TerrainGPUBuildStage::SampleColumns;
        record->publicJob.stageCursor = 0u;
      }
    }
  }

  const uint32_t debtLimitedTarget = schedule.effectiveTargetMicroseconds >
      gpuDebtMicroseconds_ ? schedule.effectiveTargetMicroseconds -
          gpuDebtMicroseconds_ : 0u;
  bool admittedColdStart = false;
  for (JobRecord *record : ordered)
  {
    if (schedule.dispatches.size() >= budget_.maximumDispatchesPerFrame)
      break;
    if (record->activeDispatch != 0u ||
        record->publicJob.stage < TerrainGPUBuildStage::SampleColumns ||
        record->publicJob.stage > TerrainGPUBuildStage::Validate)
      continue;
    const uint32_t total = stageTotalUnits(record->publicJob.stage);
    const uint32_t baseUnits = std::min(
        stageQuantum(record->publicJob.stage, budget_),
        total - std::min(total, record->publicJob.stageCursor));
    const uint32_t fullQuantumEstimate = estimate(record->publicJob.stage);
    uint32_t units = baseUnits;
    uint32_t predicted = fullQuantumEstimate;
    const uint32_t quantumLimit = debtLimitedTarget == 0u
        ? budget_.admissionCeilingMicroseconds
        : std::min(budget_.admissionCeilingMicroseconds,
              debtLimitedTarget);
    if (predicted > quantumLimit && quantumLimit != 0u && units > 1u)
    {
      units = std::max(1u, static_cast<uint32_t>(
          uint64_t{units} * quantumLimit /
          predicted));
      predicted = std::max(1u, static_cast<uint32_t>(
          uint64_t{predicted} * units / baseUnits));
    }
    if (schedule.admittedEstimateMicroseconds + predicted >
        budget_.admissionCeilingMicroseconds)
    {
      ++diagnostics_.ceilingLimitedFrames;
      continue;
    }
    const bool withinTarget = schedule.admittedEstimateMicroseconds +
        predicted <= debtLimitedTarget;
    const bool coldStart = schedule.dispatches.empty() &&
        schedule.effectiveTargetMicroseconds != 0u &&
        !admittedColdStart &&
        timings_.find(record->publicJob.stage) == timings_.end();
    if (!withinTarget && !coldStart)
    {
      ++diagnostics_.targetLimitedFrames;
      continue;
    }
    TerrainGPUDispatch dispatch{};
    dispatch.dispatchId = nextDispatchId_++;
    dispatch.jobId = record->publicJob.id;
    dispatch.key = record->publicJob.key;
    dispatch.stage = record->publicJob.stage;
    dispatch.cursor = record->publicJob.stageCursor;
    dispatch.workUnits = units;
    dispatch.estimatedMicroseconds = predicted;
    dispatches_.emplace(dispatch.dispatchId,
        DispatchRecord{dispatch, 0u, false, false});
    record->activeDispatch = dispatch.dispatchId;
    schedule.dispatches.push_back(dispatch);
    schedule.admittedEstimateMicroseconds += predicted;
    admittedColdStart = admittedColdStart || coldStart;
    ++diagnostics_.admittedDispatches;
  }

  if (input.publicationEnabled)
  {
    for (JobRecord *record : ordered)
    {
      if (schedule.publications.size() >=
          budget_.maximumPagesPublishedPerFrame)
        break;
      if (record->publicJob.stage == TerrainGPUBuildStage::ReadyToPublish &&
          !record->publicationPending && current(*record))
      {
        schedule.publications.push_back(
            {record->publicJob.id, record->publicJob.key});
        record->publicationPending = true;
      }
    }
  }

  refreshDiagnostics();
  return schedule;
}

bool TerrainGPUBuildScheduler::completeDispatch(
    const TerrainGPUDispatchCompletion &completion, std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto dispatchFound = dispatches_.find(completion.dispatchId);
  if (dispatchFound == dispatches_.end() || dispatchFound->second.completed)
    return fail(error, "GPU terrain dispatch completion is invalid");
  DispatchRecord &dispatch = dispatchFound->second;
  const auto jobFound = jobs_.find(dispatch.dispatch.jobId);
  if (jobFound == jobs_.end())
    return fail(error, "GPU terrain dispatch lost its job");
  JobRecord &job = jobFound->second;
  dispatch.completed = true;
  dispatch.completionFence = completion.completionFenceValue;
  job.lastFence = std::max(job.lastFence, completion.completionFenceValue);
  job.activeDispatch = 0u;

  if (completion.elapsedMicroseconds)
  {
    timings_[dispatch.dispatch.stage].push(*completion.elapsedMicroseconds);
    if (*completion.elapsedMicroseconds > budget_.maximumQuantumMicroseconds)
      ++diagnostics_.quantumBudgetViolations;
    if (*completion.elapsedMicroseconds >
        budget_.admissionCeilingMicroseconds)
      ++diagnostics_.dispatchCeilingViolations;
    dispatch.timingReported = true;
    gpuDebtMicroseconds_ = *completion.elapsedMicroseconds >
        budget_.targetGPUTimeMicroseconds
      ? *completion.elapsedMicroseconds - budget_.targetGPUTimeMicroseconds
      : 0u;
  }
  else
    ++diagnostics_.delayedTimingSamples;

  if (job.cancelRequested || !current(job))
  {
    terminal(job, TerrainGPUBuildStage::Cancelled,
        current(job) ? TerrainGPUFailure::None
                     : TerrainGPUFailure::StaleGeneration,
        completion.completionFenceValue);
    if (dispatch.timingReported)
      dispatches_.erase(dispatchFound);
    refreshDiagnostics();
    return true;
  }
  if (completion.failure != TerrainGPUFailure::None)
  {
    job.result.overflowStage = dispatch.dispatch.stage;
    terminal(job, TerrainGPUBuildStage::Failed, completion.failure,
        completion.completionFenceValue);
    if (dispatch.timingReported)
      dispatches_.erase(dispatchFound);
    refreshDiagnostics();
    return true;
  }
  if (job.publicJob.stage != dispatch.dispatch.stage ||
      job.publicJob.stageCursor != dispatch.dispatch.cursor)
    return fail(error, "GPU terrain dispatch state does not match its job");
  job.publicJob.stageCursor += dispatch.dispatch.workUnits;
  if (job.publicJob.stageCursor >= stageTotalUnits(job.publicJob.stage))
  {
    if (job.publicJob.stage == TerrainGPUBuildStage::ReduceBrickLevel)
      ++job.publicJob.brickLevel;
    if (job.publicJob.stage == TerrainGPUBuildStage::BuildTree64Level)
      ++job.publicJob.tree64Level;
    job.publicJob.stage = nextStage(job.publicJob.stage);
    job.publicJob.stageCursor = 0u;
  }
  if (dispatch.timingReported)
    dispatches_.erase(dispatchFound);
  refreshDiagnostics();
  return true;
}

bool TerrainGPUBuildScheduler::reportTiming(
    const uint64_t dispatchId, const uint32_t elapsedMicroseconds,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto found = dispatches_.find(dispatchId);
  if (found == dispatches_.end() || !found->second.completed ||
      found->second.timingReported)
    return fail(error, "GPU terrain timing sample is invalid");
  found->second.timingReported = true;
  timings_[found->second.dispatch.stage].push(elapsedMicroseconds);
  if (elapsedMicroseconds > budget_.maximumQuantumMicroseconds)
    ++diagnostics_.quantumBudgetViolations;
  if (elapsedMicroseconds > budget_.admissionCeilingMicroseconds)
    ++diagnostics_.dispatchCeilingViolations;
  gpuDebtMicroseconds_ = elapsedMicroseconds >
      budget_.targetGPUTimeMicroseconds
    ? elapsedMicroseconds - budget_.targetGPUTimeMicroseconds : 0u;
  dispatches_.erase(found);
  refreshDiagnostics();
  return true;
}

bool TerrainGPUBuildScheduler::completePublication(
    const TerrainGPUPublicationCompletion &completion, std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto found = jobs_.find(completion.jobId);
  if (found == jobs_.end() ||
      found->second.publicJob.stage != TerrainGPUBuildStage::ReadyToPublish ||
      !found->second.publicationPending)
    return fail(error, "GPU terrain publication completion is invalid");
  JobRecord &job = found->second;
  job.publicationPending = false;
  job.lastFence = std::max(job.lastFence, completion.completionFenceValue);
  if (job.cancelRequested || !current(job))
    terminal(job, TerrainGPUBuildStage::Cancelled,
        current(job) ? TerrainGPUFailure::None
                     : TerrainGPUFailure::StaleGeneration,
        completion.completionFenceValue);
  else if (!completion.accepted)
    terminal(job, TerrainGPUBuildStage::Failed,
        TerrainGPUFailure::InvalidResult, completion.completionFenceValue);
  else
  {
    terminal(job, TerrainGPUBuildStage::Published,
        TerrainGPUFailure::None, completion.completionFenceValue);
    ++diagnostics_.publishedPages;
  }
  refreshDiagnostics();
  return true;
}

bool TerrainGPUBuildScheduler::finishPublication(
    const uint64_t jobId, std::string *error)
{
  const auto found = jobs_.find(jobId);
  if (found == jobs_.end())
    return fail(error, "GPU terrain publication is stale or not ready");
  return completePublication({jobId,
      std::max(lastCompletedFence_, found->second.lastFence), true}, error);
}

std::optional<TerrainGPUBuildJob> TerrainGPUBuildScheduler::job(
    const uint64_t jobId) const
{
  const auto found = jobs_.find(jobId);
  return found == jobs_.end() ? std::nullopt
      : std::optional<TerrainGPUBuildJob>{found->second.publicJob};
}

std::vector<TerrainGPUBuildJob>
TerrainGPUBuildScheduler::unfinishedJobs() const
{
  std::vector<TerrainGPUBuildJob> result;
  for (const auto &[id, record] : jobs_)
    if (!terminalStage(record.publicJob.stage))
      result.push_back(record.publicJob);
  return result;
}

std::optional<TerrainGPUBuildResult> TerrainGPUBuildScheduler::result(
    const uint64_t jobId) const
{
  const auto found = jobs_.find(jobId);
  return found == jobs_.end() ? std::nullopt
      : std::optional<TerrainGPUBuildResult>{found->second.result};
}

void TerrainGPUBuildScheduler::refreshDiagnostics()
{
  diagnostics_.queuedJobs = diagnostics_.inFlightJobs =
      diagnostics_.readyJobs = diagnostics_.failedJobs =
      diagnostics_.cancelledJobs = 0u;
  for (const auto &[id, record] : jobs_)
  {
    if (record.activeDispatch != 0u || record.publicationPending)
      ++diagnostics_.inFlightJobs;
    else if (record.publicJob.stage == TerrainGPUBuildStage::ReadyToPublish)
      ++diagnostics_.readyJobs;
    else if (record.publicJob.stage == TerrainGPUBuildStage::Failed)
      ++diagnostics_.failedJobs;
    else if (record.publicJob.stage == TerrainGPUBuildStage::Cancelled)
      ++diagnostics_.cancelledJobs;
    else if (!terminalStage(record.publicJob.stage))
      ++diagnostics_.queuedJobs;
  }
  diagnostics_.gpuDebtMicroseconds = gpuDebtMicroseconds_;
  diagnostics_.scratch = scratch_.stats();
}

TerrainGPUBuildDiagnostics TerrainGPUBuildScheduler::diagnostics() const
{
  TerrainGPUBuildDiagnostics result = diagnostics_;
  result.scratch = scratch_.stats();
  return result;
}

TerrainGPUStageTiming TerrainGPUBuildScheduler::stageTiming(
    const TerrainGPUBuildStage stage) const
{
  TerrainGPUStageTiming result{};
  const auto found = timings_.find(stage);
  if (found == timings_.end())
  {
    result.estimateMicroseconds = std::max(1u, static_cast<uint32_t>(
        uint64_t{coldEstimate(stage)} *
        budget_.coldEstimateScalePermille / 1000u));
    return result;
  }
  result.sampleCount = found->second.sampleCount;
  result.exponentialMeanMicroseconds = found->second.mean;
  result.highWaterMicroseconds = found->second.highWater;
  result.p95Microseconds = found->second.p95();
  result.estimateMicroseconds = found->second.estimate(
      std::max(1u, static_cast<uint32_t>(uint64_t{coldEstimate(stage)} *
          budget_.coldEstimateScalePermille / 1000u)));
  return result;
}

bool TerrainGPUBuildScheduler::idle() const
{
  return std::none_of(jobs_.begin(), jobs_.end(), [](const auto &entry)
  {
    return !terminalStage(entry.second.publicJob.stage);
  });
}

const char *terrainGPUBuildStageName(const TerrainGPUBuildStage stage)
{
  switch (stage)
  {
  case TerrainGPUBuildStage::WaitingForMaps: return "waiting-for-maps";
  case TerrainGPUBuildStage::UploadColumns: return "upload-columns";
  case TerrainGPUBuildStage::SampleColumns: return "sample-columns";
  case TerrainGPUBuildStage::FinalizeBoundaryStitches: return "boundary-stitches";
  case TerrainGPUBuildStage::CountFineBricks: return "count-fine-bricks";
  case TerrainGPUBuildStage::CompactFineBricks: return "compact-fine-bricks";
  case TerrainGPUBuildStage::EmitFineBricks: return "emit-fine-bricks";
  case TerrainGPUBuildStage::ReduceBrickLevel: return "reduce-bricks";
  case TerrainGPUBuildStage::EmitBrickLevel: return "emit-brick-level";
  case TerrainGPUBuildStage::BuildTree64Level: return "build-tree64-level";
  case TerrainGPUBuildStage::FinalizeSidecars: return "finalize-sidecars";
  case TerrainGPUBuildStage::Validate: return "validate";
  case TerrainGPUBuildStage::ReadyToPublish: return "ready-to-publish";
  case TerrainGPUBuildStage::Published: return "published";
  case TerrainGPUBuildStage::Cancelled: return "cancelled";
  case TerrainGPUBuildStage::Failed: return "failed";
  }
  return "unknown";
}

} // namespace voxel::svraster
