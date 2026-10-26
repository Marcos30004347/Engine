#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace voxel::svraster
{

// Versioned identity carried through every GPU terrain build stage. A result
// is publishable only while all fields still match the live terrain slot.
struct TerrainGPUGenerationKey
{
  uint64_t bundleSignature = 0u;
  int32_t chunkX = 0;
  int32_t chunkZ = 0;
  uint32_t internalLOD = 0u;
  uint32_t generatorRevision = 0u;
  uint32_t terrainSlot = 0u;
  uint32_t expectedTerrainGeneration = 0u;

  bool operator==(const TerrainGPUGenerationKey &other) const
  {
    return bundleSignature == other.bundleSignature &&
        chunkX == other.chunkX && chunkZ == other.chunkZ &&
        internalLOD == other.internalLOD &&
        generatorRevision == other.generatorRevision &&
        terrainSlot == other.terrainSlot &&
        expectedTerrainGeneration == other.expectedTerrainGeneration;
  }

  bool operator<(const TerrainGPUGenerationKey &other) const
  {
    if (bundleSignature != other.bundleSignature)
      return bundleSignature < other.bundleSignature;
    if (chunkX != other.chunkX) return chunkX < other.chunkX;
    if (chunkZ != other.chunkZ) return chunkZ < other.chunkZ;
    if (internalLOD != other.internalLOD)
      return internalLOD < other.internalLOD;
    if (generatorRevision != other.generatorRevision)
      return generatorRevision < other.generatorRevision;
    if (terrainSlot != other.terrainSlot)
      return terrainSlot < other.terrainSlot;
    return expectedTerrainGeneration < other.expectedTerrainGeneration;
  }
};

struct TerrainColumnGPU
{
  uint32_t topY = 0u;
  uint32_t firstSolidY = 0u;
  uint32_t packedMaterialContext = 0u;
  uint32_t packedFlags = 0u;
};

static_assert(sizeof(TerrainColumnGPU) == 16u);

enum class TerrainGPUBuildStage : uint32_t
{
  WaitingForMaps,
  UploadColumns,
  SampleColumns,
  FinalizeBoundaryStitches,
  CountFineBricks,
  CompactFineBricks,
  EmitFineBricks,
  ReduceBrickLevel,
  EmitBrickLevel,
  BuildTree64Level,
  FinalizeSidecars,
  Validate,
  ReadyToPublish,
  Published,
  Cancelled,
  Failed,
};

enum class TerrainGPUJobClass : uint32_t
{
  VisibleMissing,
  VisibleRefinement,
  NearFrustumPrefetch,
  Background,
};

enum class TerrainGPUFailure : uint32_t
{
  None,
  ScratchExhausted,
  CounterOverflow,
  InvalidResult,
  Injected,
  StaleGeneration,
};

struct TerrainGPUScratchHandle
{
  uint64_t id = 0u;
  uint64_t offset = 0u;
  uint64_t size = 0u;

  bool valid() const { return id != 0u; }
};

struct TerrainGPUScratchStats
{
  uint64_t capacityBytes = 0u;
  uint64_t allocatedBytes = 0u;
  uint64_t highWatermarkBytes = 0u;
  uint32_t liveAllocations = 0u;
  uint32_t deferredAllocations = 0u;
  uint64_t allocationFailures = 0u;
  uint64_t invalidReleaseAttempts = 0u;
};

// Fixed-capacity first-fit allocator. Releases are fence-deferred and each
// handle can enter the deferred queue exactly once.
class TerrainGPUFixedScratchAllocator
{
public:
  explicit TerrainGPUFixedScratchAllocator(uint64_t capacityBytes = 0u);

  void reset(uint64_t capacityBytes);
  std::optional<TerrainGPUScratchHandle> allocate(
      uint64_t bytes, uint64_t alignment = 256u);
  bool releaseDeferred(TerrainGPUScratchHandle handle, uint64_t fenceValue);
  uint32_t reclaim(uint64_t completedFenceValue);
  TerrainGPUScratchStats stats() const;

private:
  struct Block { uint64_t offset = 0u; uint64_t size = 0u; };
  struct Allocation
  {
    TerrainGPUScratchHandle handle{};
    bool deferred = false;
    uint64_t fenceValue = 0u;
  };

  void insertFree(Block block);

  uint64_t capacityBytes_ = 0u;
  uint64_t nextId_ = 1u;
  uint64_t allocatedBytes_ = 0u;
  uint64_t highWatermarkBytes_ = 0u;
  uint64_t allocationFailures_ = 0u;
  uint64_t invalidReleaseAttempts_ = 0u;
  std::vector<Block> free_{};
  std::map<uint64_t, Allocation> allocations_{};
};

struct TerrainGPUBuildBudget
{
  uint32_t targetGPUTimeMicroseconds = 750u;
  uint32_t admissionCeilingMicroseconds = 1250u;
  uint32_t maximumDispatchesPerFrame = 8u;
  uint32_t maximumPagesStartedPerFrame = 2u;
  uint32_t maximumPagesPublishedPerFrame = 4u;
  uint64_t maximumMapUploadBytesPerFrame = 4ull << 20u;
  uint64_t maximumScratchBytes = 64ull << 20u;
  uint32_t maximumInFlightJobs = 8u;
  uint32_t starvationAgeFrames = 120u;
  // Backend-qualified work granularity. The scheduler may shrink these under
  // pressure, but never intentionally emits a larger quantum.
  uint32_t sampleColumnQuantum = 16u;
  uint32_t fineBrickQuantum = 256u;
  uint32_t sidecarQuantum = 16u;
  uint32_t coldEstimateScalePermille = 1000u;
  uint32_t maximumQuantumMicroseconds = 250u;
};

struct TerrainGPUBuildRequest
{
  TerrainGPUGenerationKey key{};
  TerrainGPUJobClass jobClass = TerrainGPUJobClass::Background;
  float projectedError = 0.0f;
  float distance = 0.0f;
  uint64_t columnUploadBytes = 0u;
  uint64_t scratchBytes = 0u;
  uint32_t semanticRevision = 0u;
  bool mapsReady = false;
};

struct TerrainGPUBuildJob
{
  uint64_t id = 0u;
  TerrainGPUGenerationKey key{};
  TerrainGPUBuildStage stage = TerrainGPUBuildStage::WaitingForMaps;
  uint32_t stageCursor = 0u;
  uint32_t brickLevel = 0u;
  uint32_t tree64Level = 0u;
  uint32_t flags = 0u;
  TerrainGPUScratchHandle scratch{};
};

struct TerrainGPUBuildResult
{
  TerrainGPUFailure status = TerrainGPUFailure::None;
  TerrainGPUBuildStage overflowStage = TerrainGPUBuildStage::WaitingForMaps;
  uint32_t brickNodeCount = 0u;
  uint32_t tree64NodeCount = 0u;
  uint64_t occupiedVoxelCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t semanticRevision = 0u;
};

struct TerrainGPUDispatch
{
  uint64_t dispatchId = 0u;
  uint64_t jobId = 0u;
  TerrainGPUGenerationKey key{};
  TerrainGPUBuildStage stage = TerrainGPUBuildStage::WaitingForMaps;
  uint32_t cursor = 0u;
  uint32_t workUnits = 0u;
  uint32_t estimatedMicroseconds = 0u;
};

struct TerrainGPUColumnUpload
{
  uint64_t jobId = 0u;
  uint64_t byteOffset = 0u;
  uint64_t byteCount = 0u;
};

struct TerrainGPUPublication
{
  uint64_t jobId = 0u;
  TerrainGPUGenerationKey key{};
};

struct TerrainGPUFrameInput
{
  uint64_t frameIndex = 0u;
  uint64_t completedFenceValue = 0u;
  uint32_t foregroundGPUTimeMicroseconds = 0u;
  uint32_t frameTargetMicroseconds = 16667u;
  bool publicationEnabled = false;
};

struct TerrainGPUFrameSchedule
{
  uint64_t frameIndex = 0u;
  uint32_t effectiveTargetMicroseconds = 0u;
  uint32_t admittedEstimateMicroseconds = 0u;
  std::vector<TerrainGPUColumnUpload> uploads{};
  std::vector<TerrainGPUDispatch> dispatches{};
  std::vector<TerrainGPUPublication> publications{};
};

struct TerrainGPUDispatchCompletion
{
  uint64_t dispatchId = 0u;
  uint64_t completionFenceValue = 0u;
  TerrainGPUFailure failure = TerrainGPUFailure::None;
  std::optional<uint32_t> elapsedMicroseconds{};
};

// Completion of the Phase 5 relocation + joint terrain publication CAS.
// Rejection preserves the old live page and fails the build; either outcome
// retains all publication-owned allocations through completionFenceValue.
struct TerrainGPUPublicationCompletion
{
  uint64_t jobId = 0u;
  uint64_t completionFenceValue = 0u;
  bool accepted = false;
};

struct TerrainGPUStageTiming
{
  uint32_t sampleCount = 0u;
  double exponentialMeanMicroseconds = 0.0;
  uint32_t highWaterMicroseconds = 0u;
  uint32_t p95Microseconds = 0u;
  uint32_t estimateMicroseconds = 0u;
};

struct TerrainGPUBuildDiagnostics
{
  uint32_t queuedJobs = 0u;
  uint32_t inFlightJobs = 0u;
  uint32_t readyJobs = 0u;
  uint32_t failedJobs = 0u;
  uint32_t cancelledJobs = 0u;
  uint64_t admittedDispatches = 0u;
  uint64_t admittedUploads = 0u;
  uint64_t admittedUploadBytes = 0u;
  uint64_t publishedPages = 0u;
  uint64_t staleQueuedJobs = 0u;
  uint64_t staleInFlightCompletions = 0u;
  uint64_t cancelledJobsTotal = 0u;
  uint64_t failedJobsTotal = 0u;
  uint64_t ceilingLimitedFrames = 0u;
  uint64_t targetLimitedFrames = 0u;
  uint64_t pressureZeroFrames = 0u;
  uint64_t scratchLimitedStarts = 0u;
  uint64_t delayedTimingSamples = 0u;
  uint64_t droppedTimingSamples = 0u;
  uint64_t quantumBudgetViolations = 0u;
  uint64_t dispatchCeilingViolations = 0u;
  uint32_t gpuDebtMicroseconds = 0u;
  TerrainGPUScratchStats scratch{};
};

class TerrainGPUBuildScheduler
{
public:
  explicit TerrainGPUBuildScheduler(
      TerrainGPUBuildBudget budget = TerrainGPUBuildBudget{});
  ~TerrainGPUBuildScheduler();
  TerrainGPUBuildScheduler(const TerrainGPUBuildScheduler &) = delete;
  TerrainGPUBuildScheduler &operator=(
      const TerrainGPUBuildScheduler &) = delete;

  bool configure(const TerrainGPUBuildBudget &budget,
      std::string *error = nullptr);
  void setBundleSignature(uint64_t bundleSignature);
  void setTerrainGeneration(uint32_t terrainSlot, uint32_t generation);

  bool request(const TerrainGPUBuildRequest &request, uint64_t &jobId,
      std::string *error = nullptr);
  bool markMapsReady(uint64_t jobId, uint64_t columnUploadBytes,
      std::string *error = nullptr);
  bool cancel(uint64_t jobId);
  uint32_t cancelTerrainSlot(uint32_t terrainSlot);
  uint32_t cancelAll();

  TerrainGPUFrameSchedule scheduleFrame(const TerrainGPUFrameInput &input);
  bool completeDispatch(const TerrainGPUDispatchCompletion &completion,
      std::string *error = nullptr);
  bool reportTiming(uint64_t dispatchId, uint32_t elapsedMicroseconds,
      std::string *error = nullptr);
  bool completePublication(const TerrainGPUPublicationCompletion &completion,
      std::string *error = nullptr);
  // Compatibility helper for CPU publication users. The job must already
  // have been returned by scheduleFrame(... publicationEnabled=true).
  bool finishPublication(uint64_t jobId, std::string *error = nullptr);

  std::optional<TerrainGPUBuildJob> job(uint64_t jobId) const;
  std::vector<TerrainGPUBuildJob> unfinishedJobs() const;
  std::optional<TerrainGPUBuildResult> result(uint64_t jobId) const;
  TerrainGPUBuildDiagnostics diagnostics() const;
  TerrainGPUStageTiming stageTiming(TerrainGPUBuildStage stage) const;
  bool idle() const;

private:
  struct JobRecord;
  struct DispatchRecord;
  struct TimingEstimator;

  bool current(const JobRecord &job) const;
  void cancelStale(uint64_t releaseFence);
  void terminal(JobRecord &job, TerrainGPUBuildStage stage,
      TerrainGPUFailure failure, uint64_t releaseFence);
  void refreshDiagnostics();
  uint32_t effectiveTarget(const TerrainGPUFrameInput &input) const;
  uint32_t estimate(TerrainGPUBuildStage stage) const;

  TerrainGPUBuildBudget budget_{};
  TerrainGPUFixedScratchAllocator scratch_{};
  uint64_t bundleSignature_ = 0u;
  uint64_t nextJobId_ = 1u;
  uint64_t nextSequence_ = 1u;
  uint64_t nextDispatchId_ = 1u;
  uint64_t lastFrameIndex_ = 0u;
  uint64_t lastCompletedFence_ = 0u;
  uint32_t gpuDebtMicroseconds_ = 0u;
  std::map<uint32_t, uint32_t> terrainGenerations_{};
  std::map<uint64_t, JobRecord> jobs_{};
  std::map<uint64_t, DispatchRecord> dispatches_{};
  std::map<TerrainGPUBuildStage, TimingEstimator> timings_{};
  TerrainGPUBuildDiagnostics diagnostics_{};
};

const char *terrainGPUBuildStageName(TerrainGPUBuildStage stage);

} // namespace voxel::svraster
