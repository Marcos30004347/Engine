#include "TerrainGPUV2Scheduling.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/TerrainGPUV2RasterHierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace voxel::svraster
{

uint32_t terrainGPUV2BatchLaneAdmission(
    const TerrainGPUV2BatchAdmission &admission)
{
  if (admission.maximumLanes == 0u ||
      admission.queuedUrgentPages == 0u ||
      admission.p95PageGenerationMicroseconds == 0u)
    return 0u;
  const uint32_t timeLanes =
      admission.generationTimeBudgetMicroseconds /
      admission.p95PageGenerationMicroseconds;
  const uint64_t perLaneBytes =
      admission.scratchBytesPerLane >
              std::numeric_limits<uint64_t>::max() -
                  admission.hiddenOutputBytesPerLane
          ? std::numeric_limits<uint64_t>::max()
          : admission.scratchBytesPerLane +
                admission.hiddenOutputBytesPerLane;
  const uint64_t memoryLanes = perLaneBytes == 0u
      ? admission.maximumLanes
      : admission.transientMemoryBudgetBytes / perLaneBytes;
  return std::min({
    admission.maximumLanes, admission.queuedUrgentPages, timeLanes,
    static_cast<uint32_t>(std::min<uint64_t>(
        memoryLanes, std::numeric_limits<uint32_t>::max()))});
}

namespace
{
bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool powerOfTwo(const uint64_t value)
{
  return value != 0u && (value & (value - 1u)) == 0u;
}

uint32_t integerLog2(uint64_t value)
{
  uint32_t result = 0u;
  while (value > 1u)
  {
    value >>= 1u;
    ++result;
  }
  return result;
}

bool terminal(const TerrainGPUJobStage stage)
{
  return stage == TerrainGPUJobStage::Complete || stage == TerrainGPUJobStage::Cancelled || stage == TerrainGPUJobStage::Failed;
}

uint32_t workgroupSize(const TerrainGPUJobStage stage)
{
  switch (stage)
  {
  case TerrainGPUJobStage::ScanFineBricks:
  case TerrainGPUJobStage::ScanTree64:
    return 256u;
  default:
    return 64u;
  }
}

struct Tree64Work
{
  uint32_t candidates = 0u;
  uint32_t upperCandidates = 0u;
};

Tree64Work tree64Work(const TerrainGPUChunkShapeCounts &counts)
{
  const uint32_t requiredSide = std::max({counts.voxelDimensions[0], counts.voxelDimensions[1], counts.voxelDimensions[2]});
  uint64_t side = 4u;
  while (side < requiredSide)
    side *= 4u;
  uint64_t level = (side / 4u) * (side / 4u) * (side / 4u);
  uint64_t candidates = level;
  uint64_t upper = 0u;
  while (level > 1u)
  {
    level /= 64u;
    candidates += level;
    upper += level;
  }
  return {
    static_cast<uint32_t>(std::min<uint64_t>(candidates, std::numeric_limits<uint32_t>::max())),
    static_cast<uint32_t>(std::min<uint64_t>(upper, std::numeric_limits<uint32_t>::max())),
  };
}

uint32_t rasterHierarchyWork(const TerrainGPUChunkShape &shape)
{
  uint64_t x = shape.bricksX;
  uint64_t y = shape.bricksY;
  uint64_t z = shape.bricksZ;
  const uint64_t voxelsX = x * shape.brickSide;
  const uint64_t voxelsY = y * shape.brickSide;
  const uint64_t voxelsZ = z * shape.brickSide;
  uint64_t work = 0u;
  for (uint32_t level = 1u; level < kTerrainGPUV2RasterHierarchyMaximumLevels && (x != 1u || y != 1u || z != 1u); ++level)
  {
    const uint64_t scale = uint64_t{1u} << level;
    if ((voxelsX % scale) != 0u || (voxelsY % scale) != 0u || (voxelsZ % scale) != 0u)
      break;
    x = (x + 1u) / 2u;
    y = (y + 1u) / 2u;
    z = (z + 1u) / 2u;
    const uint64_t candidates = x * y * z;
    const uint64_t scanBlocks = (candidates + 255u) / 256u;
    // Per candidate: 16 classifiers, reduce, level-0 scan/add, node emit,
    // 16 occupancy/material emitters, and link. Upper scans/adds plus reserve
    // are charged explicitly so hierarchy construction shares the same frame
    // work budget as fine raster and Tree64.
    work += candidates * 37u + scanBlocks * 4u + 4u;
  }
  return static_cast<uint32_t>(std::min<uint64_t>(work, std::numeric_limits<uint32_t>::max()));
}

uint32_t stageItems(const TerrainGPUJobStage stage, const TerrainGPUChunkShape &shape, const bool buildRasterParents)
{
  TerrainGPUChunkShapeCounts counts{};
  if (!terrainGPUChunkShapeCounts(shape, counts, nullptr))
    return 0u;
  switch (stage)
  {
  case TerrainGPUJobStage::SampleColumns:
    return counts.columnCount;
  case TerrainGPUJobStage::ScanFineBricks:
    return counts.denseBrickCount;
  case TerrainGPUJobStage::ClassifyFineBricks:
  case TerrainGPUJobStage::EmitFineBricks:
    // Fine raster owns one bounded invocation per 32-bit occupancy word.
    // Sixteen lanes replace the old single-invocation 512-cell walk.
    return counts.denseBrickCount <= std::numeric_limits<uint32_t>::max() / 16u ? counts.denseBrickCount * 16u : std::numeric_limits<uint32_t>::max();
  case TerrainGPUJobStage::BuildRasterParents:
    return buildRasterParents ? std::max(1u, rasterHierarchyWork(shape)) : 0u;
  case TerrainGPUJobStage::BuildTree64Masks:
  {
    const auto tree = tree64Work(counts);
    // Clear every candidate, accumulate one terminal contribution per fine
    // occupancy word, then reduce every upper candidate. These are separate
    // dispatches but all consume the mask-stage frame budget.
    const uint64_t fineWords = uint64_t{counts.denseBrickCount} * kSparseVoxelBrickOccupancyWords;
    return static_cast<uint32_t>(std::min<uint64_t>(uint64_t{tree.candidates} + tree.upperCandidates + fineWords, std::numeric_limits<uint32_t>::max()));
  }
  case TerrainGPUJobStage::ScanTree64:
    return tree64Work(counts).candidates;
  case TerrainGPUJobStage::LinkTree64:
    // A side-four tree has a terminal root and no upper link candidates. Keep
    // one no-op control item so the generic staged scheduler can advance.
    return std::max(1u, tree64Work(counts).upperCandidates);
  case TerrainGPUJobStage::Validate:
    // Structural validation is one bounded invocation per candidate. The
    // descriptor finalizer is the only single-invocation Tree64 stage.
    return tree64Work(counts).candidates;
  default:
    return 0u;
  }
}

TerrainGPUJobStage nextStage(const TerrainGPUJobStage stage, const bool buildRasterParents)
{
  switch (stage)
  {
  case TerrainGPUJobStage::SampleColumns:
    return TerrainGPUJobStage::ClassifyFineBricks;
  case TerrainGPUJobStage::ClassifyFineBricks:
    return TerrainGPUJobStage::ScanFineBricks;
  case TerrainGPUJobStage::ScanFineBricks:
    return TerrainGPUJobStage::EmitFineBricks;
  case TerrainGPUJobStage::EmitFineBricks:
    return buildRasterParents ? TerrainGPUJobStage::BuildRasterParents : TerrainGPUJobStage::BuildTree64Masks;
  case TerrainGPUJobStage::BuildRasterParents:
    return TerrainGPUJobStage::BuildTree64Masks;
  case TerrainGPUJobStage::BuildTree64Masks:
    return TerrainGPUJobStage::ScanTree64;
  case TerrainGPUJobStage::ScanTree64:
    return TerrainGPUJobStage::LinkTree64;
  case TerrainGPUJobStage::LinkTree64:
    return TerrainGPUJobStage::Validate;
  case TerrainGPUJobStage::Validate:
    return TerrainGPUJobStage::ReadyToPublish;
  default:
    return stage;
  }
}

std::pair<uint32_t, uint32_t> coldCost(const TerrainGPUJobStage stage)
{
  switch (stage)
  {
  case TerrainGPUJobStage::SampleColumns:
    return {8u, 4u};
  case TerrainGPUJobStage::ClassifyFineBricks:
    return {6u, 4u};
  case TerrainGPUJobStage::ScanFineBricks:
    return {10u, 3u};
  case TerrainGPUJobStage::EmitFineBricks:
    return {8u, 5u};
  case TerrainGPUJobStage::BuildRasterParents:
    return {8u, 4u};
  case TerrainGPUJobStage::BuildTree64Masks:
    return {8u, 4u};
  case TerrainGPUJobStage::ScanTree64:
    return {10u, 3u};
  case TerrainGPUJobStage::LinkTree64:
    return {8u, 4u};
  case TerrainGPUJobStage::Validate:
    return {6u, 1u};
  default:
    return {8u, 4u};
  }
}

} // namespace

TerrainGPUV2FencedArena::TerrainGPUV2FencedArena(const uint64_t capacityBytes, const uint64_t minimumClassBytes)
{
  if (capacityBytes != 0u)
  {
    std::string error;
    if (!configure(capacityBytes, minimumClassBytes, &error))
      throw std::invalid_argument(error);
  }
}

bool TerrainGPUV2FencedArena::configure(const uint64_t capacityBytes, const uint64_t minimumClassBytes, std::string *error)
{
  if (!allocations_.empty())
    return fail(error, "terrain GPU V2 arena cannot reconfigure live leases");
  if (!powerOfTwo(capacityBytes) || !powerOfTwo(minimumClassBytes) || minimumClassBytes > capacityBytes)
    return fail(error, "terrain GPU V2 arena capacity and minimum class must be powers of two");
  capacityBytes_ = capacityBytes;
  minimumClassBytes_ = minimumClassBytes;
  requestedBytes_ = reservedBytes_ = highWatermarkBytes_ = 0u;
  allocationFailures_ = invalidReleaseAttempts_ = 0u;
  nextId_ = 1u;
  const uint32_t maximumOrder = integerLog2(capacityBytes / minimumClassBytes);
  freeByOrder_.assign(maximumOrder + 1u, {});
  freeByOrder_.back().insert(0u);
  return true;
}

uint64_t TerrainGPUV2FencedArena::sizeClass(const uint64_t requestedBytes) const
{
  if (requestedBytes == 0u || minimumClassBytes_ == 0u || requestedBytes > capacityBytes_)
    return 0u;
  uint64_t result = minimumClassBytes_;
  while (result < requestedBytes)
  {
    if (result > capacityBytes_ / 2u)
      return 0u;
    result *= 2u;
  }
  return result;
}

std::optional<TerrainGPUV2Lease> TerrainGPUV2FencedArena::allocate(const uint64_t requestedBytes)
{
  const uint64_t reserved = sizeClass(requestedBytes);
  if (reserved == 0u)
  {
    ++allocationFailures_;
    return std::nullopt;
  }
  const uint32_t wantedOrder = integerLog2(reserved / minimumClassBytes_);
  uint32_t foundOrder = wantedOrder;
  while (foundOrder < freeByOrder_.size() && freeByOrder_[foundOrder].empty())
    ++foundOrder;
  if (foundOrder == freeByOrder_.size())
  {
    ++allocationFailures_;
    return std::nullopt;
  }
  const uint64_t offset = *freeByOrder_[foundOrder].begin();
  freeByOrder_[foundOrder].erase(freeByOrder_[foundOrder].begin());
  uint64_t blockBytes = minimumClassBytes_ << foundOrder;
  while (foundOrder > wantedOrder)
  {
    --foundOrder;
    blockBytes /= 2u;
    freeByOrder_[foundOrder].insert(offset + blockBytes);
  }
  TerrainGPUV2Lease lease{nextId_++, offset, requestedBytes, reserved};
  allocations_.emplace(lease.id, Allocation{lease, wantedOrder, false, 0u});
  requestedBytes_ += requestedBytes;
  reservedBytes_ += reserved;
  highWatermarkBytes_ = std::max(highWatermarkBytes_, reservedBytes_);
  return lease;
}

bool TerrainGPUV2FencedArena::releaseDeferred(const TerrainGPUV2Lease lease, const uint64_t completionFence)
{
  const auto found = allocations_.find(lease.id);
  if (found == allocations_.end() || found->second.deferred || found->second.lease.offset != lease.offset || found->second.lease.reservedBytes != lease.reservedBytes)
  {
    ++invalidReleaseAttempts_;
    return false;
  }
  found->second.deferred = true;
  found->second.completionFence = completionFence;
  return true;
}

void TerrainGPUV2FencedArena::insertAndMerge(uint64_t offset, uint32_t order)
{
  while (order + 1u < freeByOrder_.size())
  {
    const uint64_t blockBytes = minimumClassBytes_ << order;
    const uint64_t buddy = offset ^ blockBytes;
    const auto found = freeByOrder_[order].find(buddy);
    if (found == freeByOrder_[order].end())
      break;
    freeByOrder_[order].erase(found);
    offset = std::min(offset, buddy);
    ++order;
  }
  freeByOrder_[order].insert(offset);
}

uint32_t TerrainGPUV2FencedArena::reclaim(const uint64_t completedFence)
{
  std::vector<uint64_t> reclaimable;
  for (const auto &[id, allocation] : allocations_)
    if (allocation.deferred && allocation.completionFence <= completedFence)
      reclaimable.push_back(id);
  for (const uint64_t id : reclaimable)
  {
    const auto found = allocations_.find(id);
    requestedBytes_ -= found->second.lease.requestedBytes;
    reservedBytes_ -= found->second.lease.reservedBytes;
    insertAndMerge(found->second.lease.offset, found->second.order);
    allocations_.erase(found);
  }
  return static_cast<uint32_t>(reclaimable.size());
}

TerrainGPUV2ArenaStats TerrainGPUV2FencedArena::stats() const
{
  TerrainGPUV2ArenaStats result{};
  result.capacityBytes = capacityBytes_;
  result.requestedBytes = requestedBytes_;
  result.reservedBytes = reservedBytes_;
  result.highWatermarkBytes = highWatermarkBytes_;
  result.internalFragmentationBytes = reservedBytes_ - requestedBytes_;
  result.liveLeases = static_cast<uint32_t>(allocations_.size());
  result.deferredLeases = static_cast<uint32_t>(std::count_if(
      allocations_.begin(),
      allocations_.end(),
      [](const auto &entry)
      {
        return entry.second.deferred;
      }));
  result.allocationFailures = allocationFailures_;
  result.invalidReleaseAttempts = invalidReleaseAttempts_;
  return result;
}

TerrainGPUV2ParameterRing::TerrainGPUV2ParameterRing(const uint32_t laneCount)
{
  reset(laneCount);
}

void TerrainGPUV2ParameterRing::reset(const uint32_t laneCount)
{
  slots_.assign(laneCount, {});
  nextSerial_ = 1u;
  acquisitionFailures_ = invalidTransitions_ = 0u;
}

uint32_t TerrainGPUV2ParameterRing::reclaim(const uint64_t completedFence)
{
  uint32_t count = 0u;
  for (auto &slot : slots_)
    if (slot.state == 2u && slot.completionFence <= completedFence)
    {
      slot.state = 0u;
      ++count;
    }
  return count;
}

std::optional<TerrainGPUV2ParameterLane> TerrainGPUV2ParameterRing::acquire(const TerrainGPUChunkJobGPU &parameters, const uint64_t completedFence)
{
  reclaim(completedFence);
  for (uint32_t index = 0u; index < slots_.size(); ++index)
    if (slots_[index].state == 0u)
    {
      auto &slot = slots_[index];
      slot.parameters = parameters;
      slot.serial = nextSerial_++;
      slot.completionFence = 0u;
      slot.state = 1u;
      return TerrainGPUV2ParameterLane{index, slot.serial};
    }
  ++acquisitionFailures_;
  return std::nullopt;
}

bool TerrainGPUV2ParameterRing::retire(const TerrainGPUV2ParameterLane lane, const uint64_t completionFence)
{
  if (lane.index >= slots_.size() || slots_[lane.index].state != 1u || slots_[lane.index].serial != lane.serial)
  {
    ++invalidTransitions_;
    return false;
  }
  slots_[lane.index].state = 2u;
  slots_[lane.index].completionFence = completionFence;
  return true;
}

bool TerrainGPUV2ParameterRing::abandon(const TerrainGPUV2ParameterLane lane)
{
  if (lane.index >= slots_.size() || slots_[lane.index].state != 1u || slots_[lane.index].serial != lane.serial)
  {
    ++invalidTransitions_;
    return false;
  }
  slots_[lane.index].state = 0u;
  return true;
}

std::optional<TerrainGPUChunkJobGPU> TerrainGPUV2ParameterRing::parameters(const TerrainGPUV2ParameterLane lane) const
{
  if (lane.index >= slots_.size() || slots_[lane.index].state == 0u || slots_[lane.index].serial != lane.serial)
    return std::nullopt;
  return slots_[lane.index].parameters;
}

TerrainGPUV2ParameterRingStats TerrainGPUV2ParameterRing::stats() const
{
  TerrainGPUV2ParameterRingStats result{};
  result.laneCount = static_cast<uint32_t>(slots_.size());
  for (const auto &slot : slots_)
  {
    result.reservedLanes += slot.state == 1u ? 1u : 0u;
    result.inFlightLanes += slot.state == 2u ? 1u : 0u;
  }
  result.acquisitionFailures = acquisitionFailures_;
  result.invalidTransitions = invalidTransitions_;
  return result;
}

bool TerrainGPUV2StageCostModel::Key::operator<(const Key &other) const
{
  return std::tie(stage, shape.bricksX, shape.bricksY, shape.bricksZ, shape.brickSide, deviceProfile) <
         std::tie(other.stage, other.shape.bricksX, other.shape.bricksY, other.shape.bricksZ, other.shape.brickSide, other.deviceProfile);
}

TerrainGPUV2StageCost TerrainGPUV2StageCostModel::predict(const TerrainGPUJobStage stage, const TerrainGPUChunkShape &shape, const uint64_t deviceProfile, const uint32_t workgroups) const
{
  const auto [coldFixed, coldPerGroup] = coldCost(stage);
  TerrainGPUV2StageCost result{};
  result.fixedMicroseconds = coldFixed;
  result.perWorkgroupMicroseconds = coldPerGroup;
  const auto found = estimates_.find({stage, shape, deviceProfile});
  if (found != estimates_.end())
  {
    result.fixedMicroseconds = static_cast<uint32_t>(std::ceil(found->second.fixed));
    result.perWorkgroupMicroseconds = std::max(1u, static_cast<uint32_t>(std::ceil(found->second.perWorkgroup)));
    result.samples = found->second.samples;
  }
  const uint64_t predicted = uint64_t{result.fixedMicroseconds} + uint64_t{result.perWorkgroupMicroseconds} * workgroups;
  result.predictedMicroseconds = static_cast<uint32_t>(std::min<uint64_t>(predicted, std::numeric_limits<uint32_t>::max()));
  return result;
}

void TerrainGPUV2StageCostModel::observe(const TerrainGPUJobStage stage, const TerrainGPUChunkShape &shape, const uint64_t deviceProfile, const uint32_t workgroups, const uint32_t elapsedMicroseconds)
{
  if (workgroups == 0u)
    return;
  auto &estimate = estimates_[{stage, shape, deviceProfile}];
  const auto [coldFixed, coldPerGroup] = coldCost(stage);
  if (estimate.samples == 0u)
  {
    estimate.fixed = coldFixed;
    estimate.perWorkgroup = coldPerGroup;
  }
  const double observedPerGroup = std::max(0.25, (static_cast<double>(elapsedMicroseconds) - estimate.fixed) / workgroups);
  constexpr double alpha = 0.25;
  estimate.perWorkgroup = estimate.samples == 0u ? observedPerGroup : estimate.perWorkgroup * (1.0 - alpha) + observedPerGroup * alpha;
  ++estimate.samples;
  estimate.history.push_back(elapsedMicroseconds);
  if (estimate.history.size() > 64u)
    estimate.history.erase(estimate.history.begin());
}

struct TerrainGPUV2Scheduler::JobRecord
{
  TerrainGPUV2Job publicJob{};
  TerrainGPUV2Request request{};
  uint64_t sequence = 0u;
  uint64_t queuedFrame = 0u;
  uint64_t activeDispatch = 0u;
  uint64_t lastFence = 0u;
  bool atlasUploadPending = false;
  bool publicationPending = false;
};

struct TerrainGPUV2Scheduler::DispatchRecord
{
  TerrainGPUV2Dispatch dispatch{};
  uint64_t deviceProfile = 0u;
  bool completed = false;
  bool timingReported = false;
};

TerrainGPUV2Scheduler::TerrainGPUV2Scheduler(TerrainGPUV2Budget budget)
{
  std::string error;
  if (!configure(budget, &error))
    throw std::invalid_argument(error);
}

TerrainGPUV2Scheduler::~TerrainGPUV2Scheduler() = default;

bool TerrainGPUV2Scheduler::configure(const TerrainGPUV2Budget &budget, std::string *error)
{
  if (!jobs_.empty())
    return fail(error, "terrain GPU V2 scheduler cannot reconfigure live jobs");
  if (budget.targetGenerationMicroseconds == 0u || budget.hardGenerationMicroseconds < budget.targetGenerationMicroseconds || budget.maximumSingleDispatchMicroseconds == 0u ||
      budget.maximumSingleDispatchMicroseconds > budget.hardGenerationMicroseconds || budget.maximumChunksPerFrame == 0u || budget.maximumNewChunksPerFrame == 0u ||
      budget.maximumNewChunksPerFrame > budget.maximumChunksPerFrame || budget.maximumWorkgroupsPerFrame == 0u ||
      budget.maximumBackgroundPriorityCandidatesPerFrame == 0u || budget.parameterLaneCount < 2u || budget.fairnessReservePermille > 1000u)
    return fail(error, "terrain GPU V2 scheduler budget is invalid");
  TerrainGPUV2FencedArena scratch;
  TerrainGPUV2FencedArena output;
  if (!scratch.configure(budget.scratchCapacityBytes, budget.minimumAllocationClassBytes, error) || !output.configure(budget.outputCapacityBytes, budget.minimumAllocationClassBytes, error))
    return false;
  budget_ = budget;
  scratch_ = std::move(scratch);
  output_ = std::move(output);
  parameterRing_.reset(budget.parameterLaneCount);
  priorityQueue_.clear();
  return true;
}

void TerrainGPUV2Scheduler::setWorldBundleRevision(const uint64_t revision)
{
  worldBundleRevision_ = revision;
}

void TerrainGPUV2Scheduler::setTerrainGeneration(const uint32_t terrainSlot, const uint32_t generation)
{
  terrainGenerations_[terrainSlot] = generation;
}

bool TerrainGPUV2Scheduler::current(const JobRecord &job) const
{
  const auto generation = terrainGenerations_.find(job.publicJob.key.terrainSlot);
  return job.publicJob.key.worldBundleRevision == worldBundleRevision_ && generation != terrainGenerations_.end() && generation->second == job.publicJob.key.expectedGeneration;
}

bool TerrainGPUV2Scheduler::terminalStage(const TerrainGPUJobStage stage)
{
  return terminal(stage);
}

bool TerrainGPUV2Scheduler::urgentPriority(const TerrainGPUV2PriorityClass priority)
{
  return priority <= TerrainGPUV2PriorityClass::VisibleRefinement;
}

TerrainGPUV2Scheduler::JobOrderKey TerrainGPUV2Scheduler::orderKey(const JobRecord &job) const
{
  const float projectedError =
      std::isfinite(job.request.projectedErrorPixels)
          ? std::max(0.0f, job.request.projectedErrorPixels)
          : 0.0f;
  const float distance =
      std::isfinite(job.request.distanceMetres)
          ? std::max(0.0f, job.request.distanceMetres)
          : std::numeric_limits<float>::max();
  return {
    job.request.priorityClass,
    -projectedError,
    distance,
    job.publicJob.key,
    job.sequence,
    job.publicJob.id,
  };
}

void TerrainGPUV2Scheduler::enqueuePriority(JobRecord &job)
{
  if (terminalStage(job.publicJob.stage) || job.publicJob.cancelRequested)
    return;
  priorityQueue_.insert(orderKey(job));
}

void TerrainGPUV2Scheduler::dequeuePriority(const JobRecord &job)
{
  priorityQueue_.erase(orderKey(job));
}

bool TerrainGPUV2Scheduler::request(const TerrainGPUV2Request &request, uint64_t &jobId, std::string *error)
{
  jobId = 0u;
  TerrainGPUChunkShapeCounts counts{};
  TerrainGPUChunkBounds bounds{};
  if (!request.required || request.scratchBytes == 0u || request.outputReservationBytes == 0u || (!request.atlasResident && request.atlasUploadBytes == 0u) || !terrainGPUChunkShapeCounts(request.shape, counts, error) ||
      !terrainGPUChunkBounds(request.key, request.shape, bounds, error))
    return fail(error, "terrain GPU V2 request contract is invalid");
  JobRecord record{};
  record.publicJob.id = nextJobId_++;
  record.publicJob.key = request.key;
  record.publicJob.shape = request.shape;
  record.publicJob.stage = request.atlasResident ? TerrainGPUJobStage::SampleColumns : TerrainGPUJobStage::WaitingForAtlas;
  record.publicJob.atlasResident = request.atlasResident;
  record.request = request;
  record.sequence = nextSequence_++;
  record.queuedFrame = lastFrameIndex_;
  if (!current(record))
    return fail(error, "terrain GPU V2 request generation is stale");
  jobId = record.publicJob.id;
  const auto inserted = jobs_.emplace(jobId, std::move(record));
  enqueuePriority(inserted.first->second);
  refreshDiagnostics();
  return true;
}

bool TerrainGPUV2Scheduler::updatePriority(const uint64_t jobId, const TerrainGPUV2PriorityClass priorityClass, const float projectedErrorPixels, const float distanceMetres, const bool required)
{
  const auto found = jobs_.find(jobId);
  if (found == jobs_.end() || terminal(found->second.publicJob.stage))
    return false;
  dequeuePriority(found->second);
  found->second.request.priorityClass = priorityClass;
  found->second.request.projectedErrorPixels = projectedErrorPixels;
  found->second.request.distanceMetres = distanceMetres;
  if (!required)
    return cancel(jobId);
  enqueuePriority(found->second);
  return true;
}

bool TerrainGPUV2Scheduler::completeAtlasUpload(const uint64_t jobId, const bool success, std::string *error)
{
  const auto found = jobs_.find(jobId);
  if (found == jobs_.end() || !found->second.atlasUploadPending || found->second.publicJob.stage != TerrainGPUJobStage::WaitingForAtlas)
    return fail(error, "terrain GPU V2 atlas completion is invalid");
  auto &record = found->second;
  record.atlasUploadPending = false;
  if (!success)
    terminate(record, TerrainGPUJobStage::Failed, TerrainGPUV2Failure::AtlasFailure, lastCompletedFence_);
  else if (!current(record) || record.publicJob.cancelRequested)
    terminate(record, TerrainGPUJobStage::Cancelled, current(record) ? TerrainGPUV2Failure::Cancelled : TerrainGPUV2Failure::StaleGeneration, lastCompletedFence_);
  else
  {
    record.publicJob.atlasResident = true;
    record.publicJob.stage = TerrainGPUJobStage::SampleColumns;
  }
  refreshDiagnostics();
  return true;
}

bool TerrainGPUV2Scheduler::cancel(const uint64_t jobId)
{
  const auto found = jobs_.find(jobId);
  if (found == jobs_.end() || terminal(found->second.publicJob.stage) || found->second.publicJob.cancelRequested)
    return false;
  auto &record = found->second;
  record.publicJob.cancelRequested = true;
  if (record.activeDispatch == 0u && !record.publicationPending)
    terminate(record, TerrainGPUJobStage::Cancelled, TerrainGPUV2Failure::Cancelled, record.lastFence);
  refreshDiagnostics();
  return true;
}

void TerrainGPUV2Scheduler::terminate(JobRecord &job, const TerrainGPUJobStage stage, const TerrainGPUV2Failure failure, const uint64_t fence)
{
  dequeuePriority(job);
  job.publicJob.stage = stage;
  job.publicJob.failure = failure;
  const uint64_t releaseFence = std::max(fence, job.lastFence);
  if (job.publicJob.scratch.valid())
  {
    scratch_.releaseDeferred(job.publicJob.scratch, releaseFence);
    job.publicJob.scratch = {};
  }
  if (job.publicJob.output.valid())
  {
    output_.releaseDeferred(job.publicJob.output, releaseFence);
    job.publicJob.output = {};
  }
}

void TerrainGPUV2Scheduler::cancelStale(const uint64_t fence)
{
  for (auto &[id, job] : jobs_)
  {
    if (terminal(job.publicJob.stage) || current(job))
      continue;
    if (!job.publicJob.cancelRequested)
      ++diagnostics_.staleJobs;
    job.publicJob.cancelRequested = true;
    if (job.activeDispatch == 0u && !job.publicationPending)
      terminate(job, TerrainGPUJobStage::Cancelled, TerrainGPUV2Failure::StaleGeneration, fence);
  }
}

TerrainGPUV2FrameSchedule TerrainGPUV2Scheduler::scheduleFrame(const TerrainGPUV2FrameInput &input)
{
  lastFrameIndex_ = std::max(lastFrameIndex_, input.frameIndex);
  lastCompletedFence_ = std::max(lastCompletedFence_, input.completedFence);
  scratch_.reclaim(lastCompletedFence_);
  output_.reclaim(lastCompletedFence_);
  parameterRing_.reclaim(lastCompletedFence_);
  cancelStale(lastCompletedFence_);

  TerrainGPUV2FrameSchedule schedule{};
  schedule.frameIndex = input.frameIndex;
  if (input.frameTargetMicroseconds != 0u)
  {
    const double load = static_cast<double>(input.foregroundMicroseconds) / input.frameTargetMicroseconds;
    if (load < 0.95)
    {
      const double scale = load <= 0.85 ? 1.0 : (0.95 - load) / 0.10;
      schedule.effectiveTargetMicroseconds = static_cast<uint32_t>(std::floor(budget_.targetGenerationMicroseconds * std::clamp(scale, 0.0, 1.0)));
    }
  }
  if (timingDebtMicroseconds_ >= schedule.effectiveTargetMicroseconds)
  {
    timingDebtMicroseconds_ -= schedule.effectiveTargetMicroseconds;
    schedule.effectiveTargetMicroseconds = 0u;
  }
  else
  {
    schedule.effectiveTargetMicroseconds -= timingDebtMicroseconds_;
    timingDebtMicroseconds_ = 0u;
  }

  // A saturated foreground used to reduce the generation quantum to zero.
  // That is reasonable for speculative prefetch, but it made an on-screen
  // missing page wait forever and turned ordinary camera motion into seconds
  // of stale terrain. Keep a small hard-bounded floor only while genuinely
  // visible, dispatchable work exists.
  const bool urgentWorkReady = [&]()
  {
    for (const auto &order : priorityQueue_)
    {
      if (!urgentPriority(order.priorityClass))
        break;
      const auto found = jobs_.find(order.jobId);
      if (found == jobs_.end())
        continue;
      const auto &job = found->second;
      if (!terminal(job.publicJob.stage) &&
          !job.publicJob.cancelRequested && current(job) &&
          job.activeDispatch == 0u && !job.publicationPending &&
          job.publicJob.atlasResident &&
          job.publicJob.stage >= TerrainGPUJobStage::SampleColumns &&
          job.publicJob.stage <= TerrainGPUJobStage::Validate)
      {
        return true;
      }
    }
    return false;
  }();
  if (urgentWorkReady)
    schedule.effectiveTargetMicroseconds = std::max(
        schedule.effectiveTargetMicroseconds,
        std::min(
            budget_.minimumUrgentGenerationMicroseconds,
            budget_.hardGenerationMicroseconds));
  schedule.effectiveTargetMicroseconds = std::min(schedule.effectiveTargetMicroseconds, budget_.hardGenerationMicroseconds);

  std::vector<JobRecord *> ordered;
  uint32_t backgroundCandidates = 0u;
  ordered.reserve(std::min<size_t>(priorityQueue_.size(), budget_.maximumChunksPerFrame + budget_.maximumBackgroundPriorityCandidatesPerFrame));
  for (const auto &order : priorityQueue_)
  {
    const auto found = jobs_.find(order.jobId);
    if (found == jobs_.end())
      continue;
    auto &job = found->second;
    if (terminal(job.publicJob.stage) || job.publicJob.cancelRequested || !current(job))
      continue;
    if (job.activeDispatch != 0u || job.publicationPending || job.atlasUploadPending)
      continue;
    ++schedule.priorityCandidatesVisited;
    const uint64_t age = input.frameIndex >= job.queuedFrame ? input.frameIndex - job.queuedFrame : 0u;
    const bool urgentOrStarved =
        urgentPriority(job.request.priorityClass) ||
        age >= budget_.starvationAgeFrames ||
        job.publicJob.stage == TerrainGPUJobStage::ReadyToPublish;
    if (!urgentOrStarved)
    {
      if (backgroundCandidates >= budget_.maximumBackgroundPriorityCandidatesPerFrame)
      {
        ++schedule.priorityCandidatesSkipped;
        break;
      }
      ++backgroundCandidates;
    }
    ordered.push_back(&job);
  }
  diagnostics_.priorityCandidatesVisited += schedule.priorityCandidatesVisited;
  diagnostics_.priorityCandidatesSkipped += schedule.priorityCandidatesSkipped;

  for (JobRecord *job : ordered)
  {
    if (job->publicJob.stage != TerrainGPUJobStage::WaitingForAtlas || job->atlasUploadPending || job->request.atlasUploadBytes == 0u)
      continue;
    if (schedule.atlasUploadBytes + job->request.atlasUploadBytes > budget_.maximumAtlasUploadBytesPerFrame)
      continue;
    schedule.atlasUploads.push_back({job->publicJob.id, job->request.atlasUploadBytes});
    schedule.atlasUploadBytes += job->request.atlasUploadBytes;
    job->atlasUploadPending = true;
    ++diagnostics_.atlasUploads;
  }

  std::set<uint64_t> advanced;
  uint32_t newAdmissions = 0u;
  const uint32_t fairnessReserve = static_cast<uint32_t>(uint64_t{schedule.effectiveTargetMicroseconds} * budget_.fairnessReservePermille / 1000u);
  const uint32_t foregroundLimit = schedule.effectiveTargetMicroseconds - fairnessReserve;

  auto fairJob = [&](const JobRecord &job)
  {
    const uint64_t age = input.frameIndex >= job.queuedFrame ? input.frameIndex - job.queuedFrame : 0u;
    return job.request.priorityClass == TerrainGPUV2PriorityClass::Background || job.request.priorityClass == TerrainGPUV2PriorityClass::NearFrustumPrefetch || age >= budget_.starvationAgeFrames;
  };

  auto attempt = [&](JobRecord &job, const uint32_t timeLimit, const bool fairness, const bool secondRound)
  {
    if (schedule.effectiveTargetMicroseconds == 0u || job.activeDispatch != 0u || job.publicationPending || !job.publicJob.atlasResident || job.publicJob.stage < TerrainGPUJobStage::SampleColumns ||
        job.publicJob.stage > TerrainGPUJobStage::Validate)
      return false;
    const bool distinct = advanced.count(job.publicJob.id) == 0u;
    if (distinct && advanced.size() >= budget_.maximumChunksPerFrame)
      return false;
    const uint32_t total = stageItems(job.publicJob.stage, job.publicJob.shape, job.request.buildRasterParents);
    if (total == 0u || job.publicJob.stageCursor >= total)
      return false;
    const uint32_t groupSize = workgroupSize(job.publicJob.stage);
    const uint32_t remaining = total - job.publicJob.stageCursor;
    uint32_t groups = std::min((remaining + groupSize - 1u) / groupSize, budget_.maximumWorkgroupsPerFrame - std::min(schedule.workgroups, budget_.maximumWorkgroupsPerFrame));
    while (groups != 0u)
    {
      const auto cost = costModel_.predict(job.publicJob.stage, job.publicJob.shape, input.deviceProfile, groups);
      if (cost.predictedMicroseconds <= budget_.maximumSingleDispatchMicroseconds && schedule.predictedMicroseconds + cost.predictedMicroseconds <= timeLimit &&
          schedule.predictedMicroseconds + cost.predictedMicroseconds <= budget_.hardGenerationMicroseconds)
        break;
      --groups;
    }
    if (groups == 0u)
    {
      ++diagnostics_.hardLimitRejections;
      return false;
    }
    const uint32_t items = std::min(remaining, groups * groupSize);
    const auto cost = costModel_.predict(job.publicJob.stage, job.publicJob.shape, input.deviceProfile, groups);
    const bool newAdmission = !job.publicJob.scratch.valid();
    if (newAdmission)
    {
      if (newAdmissions >= budget_.maximumNewChunksPerFrame)
        return false;
      const uint64_t scratchClass = scratch_.sizeClass(job.request.scratchBytes);
      const uint64_t outputClass = output_.sizeClass(job.request.outputReservationBytes);
      if (scratchClass == 0u || outputClass == 0u || schedule.scratchBytesReserved + scratchClass > budget_.maximumScratchReservationBytesPerFrame ||
          schedule.outputBytesReserved + outputClass > budget_.maximumOutputReservationBytesPerFrame)
        return false;
    }

    TerrainGPUChunkJobGPU parameters{};
    if (!terrainGPUChunkJobGPU(job.publicJob.key, job.publicJob.shape, job.publicJob.stage, items, parameters, nullptr))
      return false;
    const auto lane = parameterRing_.acquire(parameters, lastCompletedFence_);
    if (!lane)
      return false;
    if (newAdmission)
    {
      const auto scratchLease = scratch_.allocate(job.request.scratchBytes);
      const auto outputLease = output_.allocate(job.request.outputReservationBytes);
      if (!scratchLease || !outputLease)
      {
        if (scratchLease)
        {
          scratch_.releaseDeferred(*scratchLease, lastCompletedFence_);
          scratch_.reclaim(lastCompletedFence_);
        }
        if (outputLease)
        {
          output_.releaseDeferred(*outputLease, lastCompletedFence_);
          output_.reclaim(lastCompletedFence_);
        }
        parameterRing_.abandon(*lane);
        return false;
      }
      job.publicJob.scratch = *scratchLease;
      job.publicJob.output = *outputLease;
      schedule.scratchBytesReserved += scratchLease->reservedBytes;
      schedule.outputBytesReserved += outputLease->reservedBytes;
      ++newAdmissions;
    }
    TerrainGPUV2Dispatch dispatch{};
    dispatch.id = nextDispatchId_++;
    dispatch.jobId = job.publicJob.id;
    dispatch.key = job.publicJob.key;
    dispatch.shape = job.publicJob.shape;
    dispatch.stage = job.publicJob.stage;
    dispatch.cursor = job.publicJob.stageCursor;
    dispatch.itemCount = items;
    dispatch.workgroups = groups;
    dispatch.predictedMicroseconds = cost.predictedMicroseconds;
    dispatch.parameterLane = *lane;
    dispatches_.emplace(dispatch.id, DispatchRecord{dispatch, input.deviceProfile, false, false});
    job.activeDispatch = dispatch.id;
    schedule.dispatches.push_back(dispatch);
    schedule.predictedMicroseconds += dispatch.predictedMicroseconds;
    schedule.workgroups += groups;
    advanced.insert(job.publicJob.id);
    ++diagnostics_.admittedDispatches;
    if (fairness)
      ++diagnostics_.fairnessDispatches;
    if (secondRound)
      ++diagnostics_.secondRoundDispatches;
    return true;
  };

  for (JobRecord *job : ordered)
    if (!fairJob(*job))
      attempt(*job, foregroundLimit, false, false);
  for (JobRecord *job : ordered)
    if (fairJob(*job))
      attempt(*job, schedule.effectiveTargetMicroseconds, true, false);
  for (JobRecord *job : ordered)
    attempt(*job, schedule.effectiveTargetMicroseconds, fairJob(*job), true);

  schedule.advancedChunks = static_cast<uint32_t>(advanced.size());
  schedule.newlyAdmittedChunks = newAdmissions;

  if (input.publicationEnabled)
    for (JobRecord *job : ordered)
    {
      if (schedule.publications.size() >= budget_.maximumPublicationsPerFrame)
        break;
      if (job->publicJob.stage == TerrainGPUJobStage::ReadyToPublish && !job->publicationPending && current(*job))
      {
        schedule.publications.push_back({job->publicJob.id, job->publicJob.key});
        job->publicationPending = true;
        job->publicJob.stage = TerrainGPUJobStage::SubmittedForPublication;
      }
    }
  refreshDiagnostics();
  return schedule;
}

bool TerrainGPUV2Scheduler::completeDispatch(const TerrainGPUV2DispatchCompletion &completion, std::string *error)
{
  const auto found = dispatches_.find(completion.dispatchId);
  if (found == dispatches_.end() || found->second.completed)
    return fail(error, "terrain GPU V2 dispatch completion is invalid");
  auto &dispatch = found->second;
  const auto jobFound = jobs_.find(dispatch.dispatch.jobId);
  if (jobFound == jobs_.end())
    return fail(error, "terrain GPU V2 dispatch lost its job");
  auto &job = jobFound->second;
  if (!parameterRing_.retire(dispatch.dispatch.parameterLane, completion.completionFence))
    return fail(error, "terrain GPU V2 parameter lane retirement failed");
  dispatch.completed = true;
  job.activeDispatch = 0u;
  job.lastFence = std::max(job.lastFence, completion.completionFence);
  if (completion.elapsedMicroseconds)
  {
    costModel_.observe(dispatch.dispatch.stage, dispatch.dispatch.shape, dispatch.deviceProfile, dispatch.dispatch.workgroups, *completion.elapsedMicroseconds);
    dispatch.timingReported = true;
    ++diagnostics_.timingSamples;
    if (*completion.elapsedMicroseconds > dispatch.dispatch.predictedMicroseconds)
      timingDebtMicroseconds_ = std::max(timingDebtMicroseconds_, *completion.elapsedMicroseconds - dispatch.dispatch.predictedMicroseconds);
  }
  else
    ++diagnostics_.delayedTimingSamples;

  if (job.publicJob.cancelRequested || !current(job))
    terminate(job, TerrainGPUJobStage::Cancelled, current(job) ? TerrainGPUV2Failure::Cancelled : TerrainGPUV2Failure::StaleGeneration, completion.completionFence);
  else if (completion.failure != TerrainGPUV2Failure::None)
    terminate(job, TerrainGPUJobStage::Failed, completion.failure, completion.completionFence);
  else if (job.publicJob.stage != dispatch.dispatch.stage || job.publicJob.stageCursor != dispatch.dispatch.cursor)
    return fail(error, "terrain GPU V2 dispatch no longer matches job cursor");
  else
  {
    job.publicJob.stageCursor += dispatch.dispatch.itemCount;
    const uint32_t total = stageItems(job.publicJob.stage, job.publicJob.shape, job.request.buildRasterParents);
    if (job.publicJob.stageCursor >= total)
    {
      job.publicJob.stage = nextStage(job.publicJob.stage, job.request.buildRasterParents);
      job.publicJob.stageCursor = 0u;
    }
  }
  if (dispatch.timingReported)
    dispatches_.erase(found);
  refreshDiagnostics();
  return true;
}

bool TerrainGPUV2Scheduler::reportTiming(const uint64_t dispatchId, const uint32_t elapsedMicroseconds, std::string *error)
{
  const auto found = dispatches_.find(dispatchId);
  if (found == dispatches_.end() || !found->second.completed || found->second.timingReported)
    return fail(error, "terrain GPU V2 delayed timing is invalid");
  auto &dispatch = found->second;
  costModel_.observe(dispatch.dispatch.stage, dispatch.dispatch.shape, dispatch.deviceProfile, dispatch.dispatch.workgroups, elapsedMicroseconds);
  ++diagnostics_.timingSamples;
  if (elapsedMicroseconds > dispatch.dispatch.predictedMicroseconds)
    timingDebtMicroseconds_ = std::max(timingDebtMicroseconds_, elapsedMicroseconds - dispatch.dispatch.predictedMicroseconds);
  dispatches_.erase(found);
  refreshDiagnostics();
  return true;
}

bool TerrainGPUV2Scheduler::completePublication(const uint64_t jobId, const uint64_t completionFence, const bool accepted, std::string *error)
{
  const auto found = jobs_.find(jobId);
  if (found == jobs_.end() || !found->second.publicationPending || found->second.publicJob.stage != TerrainGPUJobStage::SubmittedForPublication)
    return fail(error, "terrain GPU V2 publication completion is invalid");
  auto &job = found->second;
  job.publicationPending = false;
  job.lastFence = std::max(job.lastFence, completionFence);
  if (job.publicJob.cancelRequested || !current(job))
    terminate(job, TerrainGPUJobStage::Cancelled, current(job) ? TerrainGPUV2Failure::Cancelled : TerrainGPUV2Failure::StaleGeneration, completionFence);
  else if (!accepted)
    terminate(job, TerrainGPUJobStage::Failed, TerrainGPUV2Failure::PublicationRejected, completionFence);
  else
    terminate(job, TerrainGPUJobStage::Complete, TerrainGPUV2Failure::None, completionFence);
  refreshDiagnostics();
  return true;
}

std::optional<TerrainGPUV2Job> TerrainGPUV2Scheduler::job(const uint64_t jobId) const
{
  const auto found = jobs_.find(jobId);
  return found == jobs_.end() ? std::nullopt : std::optional<TerrainGPUV2Job>{found->second.publicJob};
}

void TerrainGPUV2Scheduler::refreshDiagnostics()
{
  diagnostics_.queuedJobs = diagnostics_.activeJobs = diagnostics_.readyToPublish = diagnostics_.completedJobs = diagnostics_.cancelledJobs = diagnostics_.failedJobs = 0u;
  for (const auto &[id, job] : jobs_)
  {
    if (job.activeDispatch != 0u || job.publicationPending)
      ++diagnostics_.activeJobs;
    else if (job.publicJob.stage == TerrainGPUJobStage::ReadyToPublish)
      ++diagnostics_.readyToPublish;
    else if (job.publicJob.stage == TerrainGPUJobStage::Complete)
      ++diagnostics_.completedJobs;
    else if (job.publicJob.stage == TerrainGPUJobStage::Cancelled)
      ++diagnostics_.cancelledJobs;
    else if (job.publicJob.stage == TerrainGPUJobStage::Failed)
      ++diagnostics_.failedJobs;
    else
      ++diagnostics_.queuedJobs;
  }
  diagnostics_.timingDebtMicroseconds = timingDebtMicroseconds_;
  diagnostics_.priorityQueueEntries = static_cast<uint32_t>(
      std::min<size_t>(priorityQueue_.size(), std::numeric_limits<uint32_t>::max()));
  diagnostics_.scratch = scratch_.stats();
  diagnostics_.output = output_.stats();
  diagnostics_.parameterRing = parameterRing_.stats();
}

TerrainGPUV2Diagnostics TerrainGPUV2Scheduler::diagnostics() const
{
  TerrainGPUV2Diagnostics result = diagnostics_;
  result.scratch = scratch_.stats();
  result.output = output_.stats();
  result.parameterRing = parameterRing_.stats();
  result.timingDebtMicroseconds = timingDebtMicroseconds_;
  result.priorityQueueEntries = static_cast<uint32_t>(
      std::min<size_t>(priorityQueue_.size(), std::numeric_limits<uint32_t>::max()));
  return result;
}

TerrainGPUV2StageCost TerrainGPUV2Scheduler::stageCost(const TerrainGPUJobStage stage, const TerrainGPUChunkShape &shape, const uint64_t deviceProfile, const uint32_t workgroups) const
{
  return costModel_.predict(stage, shape, deviceProfile, workgroups);
}

const char *terrainGPUV2StageName(const TerrainGPUJobStage stage)
{
  switch (stage)
  {
  case TerrainGPUJobStage::WaitingForAtlas:
    return "waiting-for-atlas";
  case TerrainGPUJobStage::SampleColumns:
    return "sample-columns";
  case TerrainGPUJobStage::ClassifyFineBricks:
    return "classify-fine-bricks";
  case TerrainGPUJobStage::ScanFineBricks:
    return "scan-fine-bricks";
  case TerrainGPUJobStage::EmitFineBricks:
    return "emit-fine-bricks";
  case TerrainGPUJobStage::BuildRasterParents:
    return "build-raster-parents";
  case TerrainGPUJobStage::BuildTree64Masks:
    return "build-tree64-masks";
  case TerrainGPUJobStage::ScanTree64:
    return "scan-tree64";
  case TerrainGPUJobStage::LinkTree64:
    return "link-tree64";
  case TerrainGPUJobStage::Validate:
    return "validate";
  case TerrainGPUJobStage::ReadyToPublish:
    return "ready-to-publish";
  case TerrainGPUJobStage::SubmittedForPublication:
    return "submitted-publication";
  case TerrainGPUJobStage::Complete:
    return "complete";
  case TerrainGPUJobStage::Cancelled:
    return "cancelled";
  case TerrainGPUJobStage::Failed:
    return "failed";
  }
  return "unknown";
}

} // namespace voxel::svraster
