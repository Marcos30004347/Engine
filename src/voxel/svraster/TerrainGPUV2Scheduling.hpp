#pragma once

#include "voxel/svraster/TerrainGPUChunkContract.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace voxel::svraster
{

struct TerrainGPUV2Lease
{
  uint64_t id = 0u;
  uint64_t offset = 0u;
  uint64_t requestedBytes = 0u;
  uint64_t reservedBytes = 0u;

  bool valid() const
  {
    return id != 0u;
  }
};

struct TerrainGPUV2ArenaStats
{
  uint64_t capacityBytes = 0u;
  uint64_t requestedBytes = 0u;
  uint64_t reservedBytes = 0u;
  uint64_t highWatermarkBytes = 0u;
  uint64_t internalFragmentationBytes = 0u;
  uint32_t liveLeases = 0u;
  uint32_t deferredLeases = 0u;
  uint64_t allocationFailures = 0u;
  uint64_t invalidReleaseAttempts = 0u;
};

// Power-of-two size classes with buddy coalescing. A released range remains
// unavailable until its completion fence has passed.
class TerrainGPUV2FencedArena
{
public:
  TerrainGPUV2FencedArena(uint64_t capacityBytes = 0u, uint64_t minimumClassBytes = 256u);

  bool configure(uint64_t capacityBytes, uint64_t minimumClassBytes, std::string *error = nullptr);
  std::optional<TerrainGPUV2Lease> allocate(uint64_t requestedBytes);
  bool releaseDeferred(TerrainGPUV2Lease lease, uint64_t completionFence);
  uint32_t reclaim(uint64_t completedFence);
  TerrainGPUV2ArenaStats stats() const;
  uint64_t sizeClass(uint64_t requestedBytes) const;

private:
  struct Allocation
  {
    TerrainGPUV2Lease lease{};
    uint32_t order = 0u;
    bool deferred = false;
    uint64_t completionFence = 0u;
  };

  void insertAndMerge(uint64_t offset, uint32_t order);

  uint64_t capacityBytes_ = 0u;
  uint64_t minimumClassBytes_ = 0u;
  uint64_t nextId_ = 1u;
  uint64_t requestedBytes_ = 0u;
  uint64_t reservedBytes_ = 0u;
  uint64_t highWatermarkBytes_ = 0u;
  uint64_t allocationFailures_ = 0u;
  uint64_t invalidReleaseAttempts_ = 0u;
  std::vector<std::set<uint64_t>> freeByOrder_{};
  std::map<uint64_t, Allocation> allocations_{};
};

struct TerrainGPUV2ParameterLane
{
  uint32_t index = 0u;
  uint64_t serial = 0u;
};

struct TerrainGPUV2ParameterRingStats
{
  uint32_t laneCount = 0u;
  uint32_t reservedLanes = 0u;
  uint32_t inFlightLanes = 0u;
  uint64_t acquisitionFailures = 0u;
  uint64_t invalidTransitions = 0u;
};

class TerrainGPUV2ParameterRing
{
public:
  explicit TerrainGPUV2ParameterRing(uint32_t laneCount = 0u);
  void reset(uint32_t laneCount);
  std::optional<TerrainGPUV2ParameterLane> acquire(const TerrainGPUChunkJobGPU &parameters, uint64_t completedFence);
  bool retire(TerrainGPUV2ParameterLane lane, uint64_t completionFence);
  bool abandon(TerrainGPUV2ParameterLane lane);
  uint32_t reclaim(uint64_t completedFence);
  std::optional<TerrainGPUChunkJobGPU> parameters(TerrainGPUV2ParameterLane lane) const;
  TerrainGPUV2ParameterRingStats stats() const;

private:
  struct Slot
  {
    TerrainGPUChunkJobGPU parameters{};
    uint64_t serial = 0u;
    uint64_t completionFence = 0u;
    uint32_t state = 0u; // 0 free, 1 reserved, 2 in flight
  };
  std::vector<Slot> slots_{};
  uint64_t nextSerial_ = 1u;
  uint64_t acquisitionFailures_ = 0u;
  uint64_t invalidTransitions_ = 0u;
};

struct TerrainGPUV2StageCost
{
  uint32_t predictedMicroseconds = 0u;
  uint32_t fixedMicroseconds = 0u;
  uint32_t perWorkgroupMicroseconds = 0u;
  uint32_t samples = 0u;
};

class TerrainGPUV2StageCostModel
{
public:
  TerrainGPUV2StageCost predict(TerrainGPUJobStage stage, const TerrainGPUChunkShape &shape, uint64_t deviceProfile, uint32_t workgroups) const;
  void observe(TerrainGPUJobStage stage, const TerrainGPUChunkShape &shape, uint64_t deviceProfile, uint32_t workgroups, uint32_t elapsedMicroseconds);

private:
  struct Key
  {
    TerrainGPUJobStage stage{};
    TerrainGPUChunkShape shape{};
    uint64_t deviceProfile = 0u;
    bool operator<(const Key &other) const;
  };
  struct Estimate
  {
    double fixed = 0.0;
    double perWorkgroup = 0.0;
    uint32_t samples = 0u;
    std::vector<uint32_t> history{};
  };
  std::map<Key, Estimate> estimates_{};
};

enum class TerrainGPUV2PriorityClass : uint32_t
{
  VisibleMissing,
  VisibleRefinement,
  NearFrustumPrefetch,
  Background,
};

enum class TerrainGPUV2Failure : uint32_t
{
  None,
  Cancelled,
  StaleGeneration,
  AtlasFailure,
  DispatchFailure,
  PublicationRejected,
  AllocationFailure,
};

struct TerrainGPUV2Budget
{
  uint32_t targetGenerationMicroseconds = 750u;
  uint32_t hardGenerationMicroseconds = 1500u;
  // Visible holes and visible refinement must continue to make progress even
  // when foreground rendering consumes the nominal frame target.  The floor
  // is still capped by hardGenerationMicroseconds and every independent work,
  // memory, and dispatch limit below.
  uint32_t minimumUrgentGenerationMicroseconds = 500u;
  uint32_t maximumSingleDispatchMicroseconds = 500u;
  uint32_t maximumChunksPerFrame = 128u;
  uint32_t maximumNewChunksPerFrame = 32u;
  uint32_t maximumPublicationsPerFrame = 128u;
  uint32_t maximumWorkgroupsPerFrame = 8192u;
  uint64_t maximumAtlasUploadBytesPerFrame = 8ull << 20u;
  uint64_t scratchCapacityBytes = 128ull << 20u;
  uint64_t outputCapacityBytes = 512ull << 20u;
  uint64_t maximumScratchReservationBytesPerFrame = 32ull << 20u;
  uint64_t maximumOutputReservationBytesPerFrame = 128ull << 20u;
  uint64_t minimumAllocationClassBytes = 256u;
  // Scheduler-side priority selection should be incremental and bounded. All
  // visible/urgent jobs are still considered, but speculative background work
  // is capped so a large request cloud does not rebuild/sort the whole cut on
  // the CPU every frame.
  uint32_t maximumBackgroundPriorityCandidatesPerFrame = 512u;
  // One immutable parameter record is retained per in-flight dispatch. Keep
  // enough default lanes for the advertised 128-chunk frame quantum plus an
  // additional frame so normal fence latency does not become the scheduler's
  // accidental primary budget.
  uint32_t parameterLaneCount = 256u;
  uint32_t fairnessReservePermille = 100u;
  uint32_t starvationAgeFrames = 120u;
};

// Admission for the renderer's fixed-capacity batched page chain. This is
// intentionally independent of the scheduler's candidate cap: a lane owns a
// complete columns/raster/Tree64 address space until its publication fence.
struct TerrainGPUV2BatchAdmission
{
  uint32_t maximumLanes = 4u;
  uint32_t queuedUrgentPages = 0u;
  uint32_t generationTimeBudgetMicroseconds = 0u;
  uint32_t p95PageGenerationMicroseconds = 0u;
  uint64_t transientMemoryBudgetBytes = 0u;
  uint64_t scratchBytesPerLane = 0u;
  uint64_t hiddenOutputBytesPerLane = 0u;
};

uint32_t terrainGPUV2BatchLaneAdmission(
    const TerrainGPUV2BatchAdmission &admission);

struct TerrainGPUV2Request
{
  TerrainGPUChunkKey key{};
  TerrainGPUChunkShape shape{8u, 4u, 8u, 8u};
  TerrainGPUV2PriorityClass priorityClass = TerrainGPUV2PriorityClass::Background;
  float projectedErrorPixels = 0.0f;
  float distanceMetres = 0.0f;
  uint64_t scratchBytes = 0u;
  uint64_t outputReservationBytes = 0u;
  uint64_t atlasUploadBytes = 0u;
  bool atlasResident = false;
  bool buildRasterParents = false;
  bool required = true;
};

struct TerrainGPUV2Job
{
  uint64_t id = 0u;
  TerrainGPUChunkKey key{};
  TerrainGPUChunkShape shape{};
  TerrainGPUJobStage stage = TerrainGPUJobStage::WaitingForAtlas;
  uint32_t stageCursor = 0u;
  bool cancelRequested = false;
  bool atlasResident = false;
  TerrainGPUV2Lease scratch{};
  TerrainGPUV2Lease output{};
  TerrainGPUV2Failure failure = TerrainGPUV2Failure::None;
};

struct TerrainGPUV2Dispatch
{
  uint64_t id = 0u;
  uint64_t jobId = 0u;
  TerrainGPUChunkKey key{};
  TerrainGPUChunkShape shape{};
  TerrainGPUJobStage stage{};
  uint32_t cursor = 0u;
  uint32_t itemCount = 0u;
  uint32_t workgroups = 0u;
  uint32_t predictedMicroseconds = 0u;
  TerrainGPUV2ParameterLane parameterLane{};
};

struct TerrainGPUV2AtlasUpload
{
  uint64_t jobId = 0u;
  uint64_t bytes = 0u;
};

struct TerrainGPUV2Publication
{
  uint64_t jobId = 0u;
  TerrainGPUChunkKey key{};
};

struct TerrainGPUV2FrameInput
{
  uint64_t frameIndex = 0u;
  uint64_t completedFence = 0u;
  uint64_t deviceProfile = 0u;
  uint32_t foregroundMicroseconds = 0u;
  uint32_t frameTargetMicroseconds = 16667u;
  bool publicationEnabled = false;
};

struct TerrainGPUV2FrameSchedule
{
  uint64_t frameIndex = 0u;
  uint32_t effectiveTargetMicroseconds = 0u;
  uint32_t predictedMicroseconds = 0u;
  uint32_t workgroups = 0u;
  uint32_t advancedChunks = 0u;
  uint32_t newlyAdmittedChunks = 0u;
  uint64_t scratchBytesReserved = 0u;
  uint64_t outputBytesReserved = 0u;
  uint64_t atlasUploadBytes = 0u;
  uint32_t priorityCandidatesVisited = 0u;
  uint32_t priorityCandidatesSkipped = 0u;
  std::vector<TerrainGPUV2Dispatch> dispatches{};
  std::vector<TerrainGPUV2AtlasUpload> atlasUploads{};
  std::vector<TerrainGPUV2Publication> publications{};
};

struct TerrainGPUV2DispatchCompletion
{
  uint64_t dispatchId = 0u;
  uint64_t completionFence = 0u;
  TerrainGPUV2Failure failure = TerrainGPUV2Failure::None;
  std::optional<uint32_t> elapsedMicroseconds{};
};

struct TerrainGPUV2Diagnostics
{
  uint32_t queuedJobs = 0u;
  uint32_t activeJobs = 0u;
  uint32_t readyToPublish = 0u;
  uint32_t completedJobs = 0u;
  uint32_t cancelledJobs = 0u;
  uint32_t failedJobs = 0u;
  uint64_t admittedDispatches = 0u;
  uint64_t atlasUploads = 0u;
  uint64_t staleJobs = 0u;
  uint64_t timingSamples = 0u;
  uint64_t delayedTimingSamples = 0u;
  uint64_t hardLimitRejections = 0u;
  uint64_t fairnessDispatches = 0u;
  uint64_t secondRoundDispatches = 0u;
  uint64_t rendererDrainCalls = 0u;
  uint64_t waitIdleCalls = 0u;
  uint32_t priorityQueueEntries = 0u;
  uint32_t priorityCandidatesVisited = 0u;
  uint32_t priorityCandidatesSkipped = 0u;
  uint64_t fullPrioritySorts = 0u;
  uint32_t timingDebtMicroseconds = 0u;
  TerrainGPUV2ArenaStats scratch{};
  TerrainGPUV2ArenaStats output{};
  TerrainGPUV2ParameterRingStats parameterRing{};
};

class TerrainGPUV2Scheduler
{
public:
  explicit TerrainGPUV2Scheduler(TerrainGPUV2Budget budget = TerrainGPUV2Budget{});
  ~TerrainGPUV2Scheduler();
  TerrainGPUV2Scheduler(const TerrainGPUV2Scheduler &) = delete;
  TerrainGPUV2Scheduler &operator=(const TerrainGPUV2Scheduler &) = delete;
  bool configure(const TerrainGPUV2Budget &budget, std::string *error = nullptr);
  void setWorldBundleRevision(uint64_t revision);
  void setTerrainGeneration(uint32_t terrainSlot, uint32_t generation);
  bool request(const TerrainGPUV2Request &request, uint64_t &jobId, std::string *error = nullptr);
  bool updatePriority(uint64_t jobId, TerrainGPUV2PriorityClass priorityClass, float projectedErrorPixels, float distanceMetres, bool required);
  bool completeAtlasUpload(uint64_t jobId, bool success, std::string *error = nullptr);
  bool cancel(uint64_t jobId);
  TerrainGPUV2FrameSchedule scheduleFrame(const TerrainGPUV2FrameInput &input);
  bool completeDispatch(const TerrainGPUV2DispatchCompletion &completion, std::string *error = nullptr);
  bool reportTiming(uint64_t dispatchId, uint32_t elapsedMicroseconds, std::string *error = nullptr);
  bool completePublication(uint64_t jobId, uint64_t completionFence, bool accepted, std::string *error = nullptr);
  std::optional<TerrainGPUV2Job> job(uint64_t jobId) const;
  TerrainGPUV2Diagnostics diagnostics() const;
  TerrainGPUV2StageCost stageCost(TerrainGPUJobStage stage, const TerrainGPUChunkShape &shape, uint64_t deviceProfile, uint32_t workgroups) const;

private:
  struct JobRecord;
  struct DispatchRecord;
  struct JobOrderKey
  {
    TerrainGPUV2PriorityClass priorityClass = TerrainGPUV2PriorityClass::Background;
    float negativeProjectedErrorPixels = 0.0f;
    float distanceMetres = 0.0f;
    TerrainGPUChunkKey key{};
    uint64_t sequence = 0u;
    uint64_t jobId = 0u;

    bool operator<(const JobOrderKey &other) const
    {
      return std::tie(priorityClass, negativeProjectedErrorPixels, distanceMetres, key, sequence, jobId) <
             std::tie(other.priorityClass, other.negativeProjectedErrorPixels, other.distanceMetres, other.key, other.sequence, other.jobId);
    }
  };
  bool current(const JobRecord &job) const;
  static bool terminalStage(TerrainGPUJobStage stage);
  static bool urgentPriority(TerrainGPUV2PriorityClass priority);
  JobOrderKey orderKey(const JobRecord &job) const;
  void enqueuePriority(JobRecord &job);
  void dequeuePriority(const JobRecord &job);
  void terminate(JobRecord &job, TerrainGPUJobStage stage, TerrainGPUV2Failure failure, uint64_t fence);
  void cancelStale(uint64_t fence);
  void refreshDiagnostics();

  TerrainGPUV2Budget budget_{};
  TerrainGPUV2FencedArena scratch_{};
  TerrainGPUV2FencedArena output_{};
  TerrainGPUV2ParameterRing parameterRing_{};
  TerrainGPUV2StageCostModel costModel_{};
  uint64_t worldBundleRevision_ = 0u;
  uint64_t nextJobId_ = 1u;
  uint64_t nextDispatchId_ = 1u;
  uint64_t nextSequence_ = 1u;
  uint64_t lastFrameIndex_ = 0u;
  uint64_t lastCompletedFence_ = 0u;
  uint32_t timingDebtMicroseconds_ = 0u;
  std::map<uint32_t, uint32_t> terrainGenerations_{};
  std::map<uint64_t, JobRecord> jobs_{};
  std::map<uint64_t, DispatchRecord> dispatches_{};
  std::set<JobOrderKey> priorityQueue_{};
  TerrainGPUV2Diagnostics diagnostics_{};
};

const char *terrainGPUV2StageName(TerrainGPUJobStage stage);

} // namespace voxel::svraster
