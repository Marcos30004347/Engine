#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sparse_voxel_sample
{
struct ShadowBenchmarkNamedTiming
{
  std::string name;
  double milliseconds = 0.0;
};

// One frame of adaptive tile-budget accounting, read back from the statistics
// bank the scheduler wrote. Populated only for sequences that measure
// invalidation latency; throughput sequences leave it zeroed so their GPU
// timings stay free of the readback stall.
struct ShadowBenchmarkRayBudget
{
  bool valid = false;
  uint32_t coveredTiles = 0u;
  uint32_t tilesAwake = 0u;
  uint32_t tilesConverged = 0u;
  uint32_t totalBudget = 0u;
  uint32_t raysScheduled = 0u;
  uint32_t heartbeatRays = 0u;
  uint32_t disocclusionRays = 0u;
  uint32_t motionRays = 0u;
  uint32_t lightingChangeRays = 0u;
  uint32_t noiseRays = 0u;
  uint32_t unstableTileRays = 0u;
  uint32_t resolvedRays = 0u;
  uint32_t visibilityChanges = 0u;
  // Summed per-pixel demand in 1/256 fixed point, and the covered receivers it
  // was summed over. Their ratio is the frame's mean demand, which is the
  // signal the whole convergence chain is trying to drive to zero.
  uint32_t totalDemand = 0u;
  uint32_t coveredPixels = 0u;
};

// Result of the moving-occluder sequence: how long a converged image takes to
// notice that a shadow caster moved, and whether the rest of the frame stayed
// asleep while it did.
struct ShadowBenchmarkOccluderInvalidation
{
  bool valid = false;
  // Frame the occluder stepped to its new position.
  uint32_t stepFrame = 0u;
  // Frames after the step until a sentinel reported a changed occluder, and
  // until the awake tile count rose above the settled baseline. Zero means the
  // signal never arrived inside the measured window.
  uint32_t framesToVisibilityChange = 0u;
  uint32_t framesToTileWake = 0u;
  // Awake and converged tile counts on the frame before the step.
  uint32_t settledTilesBeforeStep = 0u;
  uint32_t awakeTilesBeforeStep = 0u;
  // Peak awake tiles reached after the step, and the tiles still converged at
  // that peak.
  uint32_t peakAwakeTilesAfterStep = 0u;
  uint32_t convergedTilesAtPeak = 0u;
  // Largest per-frame budget attributed to disocclusion plus detected lighting
  // change after the step, and the total rays scheduled on that same frame.
  // Their ratio is the localisation measure: a correct invalidation claims a
  // region of the budget, not all of it. This is independent of whether the
  // scene converges, which the awake-tile counts are not.
  uint32_t peakChangeAttributedRays = 0u;
  uint32_t scheduledRaysAtChangePeak = 0u;
  // Frames after the step until the awake count returned to the baseline.
  uint32_t framesToResettle = 0u;
  // Sentinel visibility changes observed across the whole measured window.
  uint32_t totalVisibilityChanges = 0u;
};

struct ShadowBenchmarkFrame
{
  uint32_t frameIndex = 0u;
  double cpuTotalMilliseconds = 0.0;
  double gpuTotalMilliseconds = 0.0;
  double occupancySourceMilliseconds = 0.0;
  double occupancyPlanningMilliseconds = 0.0;
  double occupancyUploadMilliseconds = 0.0;
  double tree64PlanningMilliseconds = 0.0;
  double tree64UploadMilliseconds = 0.0;
  std::vector<ShadowBenchmarkNamedTiming> cpuSections;
  std::vector<ShadowBenchmarkNamedTiming> gpuPasses;
  ShadowBenchmarkRayBudget rayBudget{};
  struct ScreenTrace
  {
    bool valid = false;
    uint32_t activeRays = 0u;
    uint32_t nodeVisits = 0u;
    uint32_t leafVisits = 0u;
    uint32_t maximumLeafVisits = 0u;
    uint32_t hits = 0u;
    uint32_t missTMax = 0u;
    uint32_t escapedScene = 0u;
    uint32_t shortSegments = 0u;
  } screenTrace{};
  struct TerrainFeedback
  {
    bool valid = false;
    bool readbackSubmitted = false;
    bool readbackConsumed = false;
    bool skippedForCadence = false;
    bool skippedForBackPressure = false;
    uint32_t ageFrames = 0u;
    uint32_t requestCount = 0u;
    uint32_t queuedRequestCount = 0u;
  } terrainFeedback{};
  struct TerrainStreaming
  {
    bool valid = false;
    uint32_t residentPageCount = 0u;
    uint32_t residentPageCapacity = 0u;
    uint32_t pendingRefinementRequestCount = 0u;
    bool uploadSubmitted = false;
    uint64_t uploadBytesSubmitted = 0u;
    uint32_t uploadSectionCount = 0u;
    uint32_t uploadPublicationCount = 0u;
    uint32_t uploadBatchIndex = 0u;
    uint32_t uploadBatchCount = 0u;
    uint64_t pendingUploadBytes = 0u;
    uint32_t pendingUploadBatchCount = 0u;
    uint64_t uploadQueueLatencyFrames = 0u;
    uint32_t committedTree64PageCount = 0u;
  } terrainStreaming{};
};

struct ShadowBenchmarkEnvironment
{
  std::string gitCommit = "unknown";
  bool dirtyWorktree = false;
  std::string buildType = "unknown";
  std::string compiler = "unknown";
  std::string operatingSystem = "unknown";
  std::string deviceName = "unknown";
  uint32_t vendorId = 0u;
  uint32_t deviceId = 0u;
  uint32_t driverVersion = 0u;
  uint32_t subgroupSize = 0u;
  double timestampPeriodNanoseconds = 0.0;
  uint64_t sceneContentHash = 0u;
  uint64_t sceneBuildSettingsHash = 0u;
};

struct ShadowBenchmarkMemory
{
  uint64_t occupancyBaseResidentBytes = 0u;
  uint64_t occupancyBasePersistentBytes = 0u;
  uint64_t occupancyPyramidResidentBytes = 0u;
  uint64_t occupancyPyramidPersistentBytes = 0u;
  uint64_t occupancyTotalPersistentBytes = 0u;
  uint64_t tree64DirectoryBytes = 0u;
  uint64_t tree64NodePoolReservedBytes = 0u;
  uint64_t tree64BuildScratchBytes = 0u;
  uint64_t tree64TotalPersistentBytes = 0u;
  uint64_t skyGuideCDFBytes = 0u;
  uint64_t skyGuideFeedbackBytes = 0u;
  uint64_t skyGuideTotalBytes = 0u;
  // Incremental production buffers for the R2 Phase0 representation split.
  // Diagnostic counters/readbacks are excluded because benchmark startup
  // rejects them.
  uint64_t brickMicroRecordBufferBytes = 0u;
  uint64_t brickMicroResidualBufferBytes = 0u;
  uint64_t brickMicroDispatchBufferBytes = 0u;
  uint64_t brickMicroDrawArgsBufferBytes = 0u;
  uint64_t brickMicroTotalBufferBytes = 0u;
};

struct ShadowBenchmarkTraceSummary
{
  uint64_t outputHash = 0u;
  uint64_t rayHash = 0u;
  uint64_t resultHash = 0u;
  uint32_t rayCount = 0u;
  std::array<uint32_t, 4> statusCounts{};
  uint32_t maximumQueries = 0u;
  uint32_t maximumBaseSteps = 0u;
};

struct ShadowBenchmarkConfiguration
{
  std::string workload = "shadow-diagnostic";
  std::string backend = "tree64";
  std::string tree64Traversal = "region";
  std::string sequence = "static";
  std::string terrainPerformancePreset = "default";
  // Generated-world residency provenance. "none" is used by OBJ and packaged
  // planet workloads that do not exercise world-map chunk streaming.
  std::string terrainStreamingMode = "none";
  uint32_t terrainPagesPerUpdate = 0u;
  uint64_t terrainUploadBudgetBytes = 0u;
  uint32_t terrainFeedbackInterval = 1u;
  uint32_t terrainFeedbackPendingMaxStaleFrames = 0u;
  bool screenTraceStatisticsEnabled = false;
  // False when GPU timing comes from an external Xcode/Metal capture. The
  // report still carries CPU pacing and workload counters without presenting
  // Vulkan timestamp zeros as measured GPU time.
  bool gpuTimingsAvailable = true;
  uint32_t warmupFrames = 0u;
  uint32_t measuredFrames = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
  // Resolved scene thresholds, after presets and explicit CLI overrides. Keep
  // these separate: raster-detail experiments must not silently change the
  // independent Tree64 resolver threshold.
  float rasterLODPixelThreshold = 0.0f;
  float tree64LODPixelThreshold = 0.0f;
  // Number of graph-resident SVGF spatial passes. Zero is temporal-only;
  // one and two add the corresponding a-trous iterations.
  uint32_t svgfAtrousIterations = 2u;
  // Performance-sensitive graph and quality controls needed to reproduce a
  // native-resolution scene-lighting capture.
  uint32_t probeTileSize = 0u;
  uint32_t probeAdaptivePercent = 0u;
  uint32_t probeAwakeRays = 0u;
  uint32_t planetBackgroundStride = 1u;
  bool terrainMicroMaterialsEnabled = false;
  // Resolved graph state: false when the compatibility switch is selected or
  // when the active G-buffer resolver does not build the brick raster path.
  bool brickMicroRasterEnabled = false;
  // Stable implementation provenance for comparisons across shader revisions.
  // The current production values are "disabled", "r2-heightfield-parent" and
  // "r2-heightfield-parent-merged", the last being the same three draws
  // recorded into one render pass.
  std::string brickMicroRasterMode = "disabled";
  // Effective capacity after applying the renderer's default sizing policy or
  // an explicit override. Performance benchmarks reject overrides, but still
  // record the resolved value that sized the production buffer.
  uint32_t brickMicroRecordCapacity = 0u;
  // Zero identifies the unchanged/default brick shader. Positive values are
  // world metres and select the separately compiled distance-cutoff variant.
  float brickExactMicroDistanceCutoffMetres = 0.0f;
  // Resolved world-space thresholds, after automatic defaults and ordering.
  // Recording effective values keeps cutoff A/B reports self-describing even
  // when their command lines used zero or a terrain preset.
  float probeDensityHalfDistanceMetres = 0.0f;
  float probeDensityQuarterDistanceMetres = 0.0f;
  float probeTraceFadeStartDistanceMetres = 0.0f;
  float probeAnalyticalDistanceMetres = 0.0f;
  std::array<uint32_t, 3> occupancyResolution{};
  std::array<float, 3> occupancyCellSize{};
  std::array<float, 3> occupancyWorldExtent{};
  std::array<uint32_t, 3> tree64DirectoryResolution{};
  uint32_t tree64ChunkSide = 0u;
  uint32_t tree64BuildBatchCapacity = 0u;
  float tree64ShadowLODWorldSize = 0.0f;
  float tree64ShadowLODDistanceSlope = 0.0f;
  float tree64ShadowLODStartDistance = 0.0f;
  float tree64ShadowLODMaximumWorldSize = 0.0f;
  float skyGuidingStrength = 0.0f;
  float skyGuidingLearningRate = 0.0f;
  float rayRefreshInterval = 0.0f;
  uint32_t minimumConvergedSamples = 0u;
  float convergenceRelativeError = 0.0f;
  uint32_t stableObservationCount = 0u;
  uint32_t regionStabilityTileSize = 0u;
  uint32_t regionWakeFrames = 0u;
  float regionSoftVisibilityThreshold = 0.0f;
  uint32_t regionSoftVisibilityStreak = 0u;
  uint32_t regionSoftPrefillStride = 0u;
  float rayTileBudgetFraction = 0.0f;
  float rayTileBudgetBoost = 0.0f;
  uint32_t rayTileBudgetHeartbeat = 0u;
  // Frames the moving-occluder sequence holds the caster still before it steps.
  uint32_t occluderSettleFrames = 0u;
};

class ShadowBenchmarkReport
{
public:
  static constexpr uint32_t kSchemaVersion = 19u;

  ShadowBenchmarkFrame &frame(uint32_t frameIndex);
  bool write(const std::filesystem::path &path, std::string *error) const;

  ShadowBenchmarkConfiguration configuration{};
  ShadowBenchmarkEnvironment environment{};
  ShadowBenchmarkMemory memory{};
  ShadowBenchmarkTraceSummary trace{};
  ShadowBenchmarkOccluderInvalidation occluderInvalidation{};
  std::vector<ShadowBenchmarkFrame> frames;
};
} // namespace sparse_voxel_sample
