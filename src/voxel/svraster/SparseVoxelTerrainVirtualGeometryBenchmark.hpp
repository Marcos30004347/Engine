#pragma once

#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryRenderRoute.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

enum class SparseVoxelTerrainVGRendererBenchmarkPathKind : uint32_t
{
  BrickBaseline = 0u,
  VirtualGeometryDefault,
  VirtualGeometryGBufferOnly,
  VirtualGeometrySettledResidency,
};

enum class SparseVoxelTerrainComputeRasterRequest : uint32_t
{
  Disabled = 0u,
  Auto,
  Force,
};

enum class SparseVoxelTerrainComputeRasterBackend : uint32_t
{
  None = 0u,
  NativeMetalAtomic64,
  VulkanTiledAtomic32,
};

enum class SparseVoxelTerrainComputeRasterClusterRoute : uint32_t
{
  Hardware = 0u,
  Compute,
  HardwareOverflowFallback,
};

struct SparseVoxelTerrainComputeRasterCapabilities
{
  bool platformApple = false;
  bool moltenVKActive = false;
  bool nativeMetalDeviceAvailable = false;
  bool nativeMetalQueueCompatibleWithMoltenVK = false;
  bool nativeMetalBufferInteropAvailable = false;
  bool nativeMetalSupportsAtomic64MinMax = false;
  bool vulkanShaderBufferInt64Atomics = false;
  bool vulkanShaderBufferInt32Atomics = true;
  uint32_t maximumCandidatesPerTile = 0u;
};

struct SparseVoxelTerrainComputeRasterSettings
{
  SparseVoxelTerrainComputeRasterRequest request =
      SparseVoxelTerrainComputeRasterRequest::Disabled;
  SparseVoxelTerrainGBufferRoute gbufferRoute =
      SparseVoxelTerrainGBufferRoute::None;
  bool requireVirtualGeometryGBufferRoute = true;
  // Nanite-style software-raster routing threshold, evaluated per cluster.
  // Clusters whose estimated average projected triangle edge length is below
  // this value route to the compute/software raster path; larger clusters stay
  // on the hardware rasterizer.
  double projectedTriangleEdgePixelThreshold = 32.0;
  double highOverdrawThreshold = 4.0;
  uint32_t tileCandidateCapacity = 4096u;
  bool allowNativeMetalInterop = true;
  bool allowVulkanTiled32Fallback = true;
};

struct SparseVoxelTerrainComputeRasterRouteDecision
{
  SparseVoxelTerrainComputeRasterBackend backend =
      SparseVoxelTerrainComputeRasterBackend::None;
  bool enabled = false;
  bool allocateComputeRasterBuffers = false;
  bool registerComputeRasterPasses = false;
  bool bypassesMoltenVKAtomic64Feature = false;
  bool usesPacked64DepthPayloadAtomic = false;
  bool usesPacked32TileDepthSlotAtomic = false;
  std::string reason{};
};

struct SparseVoxelTerrainComputeRasterClusterStats
{
  double averageProjectedTriangleEdgePixels = 0.0;
  double overdrawDepthComplexityProxy = 0.0;
  uint32_t maximumCandidatesPerTile = 0u;
  bool fullyResident = true;
};

bool selectSparseVoxelTerrainComputeRasterRoute(
    const SparseVoxelTerrainComputeRasterCapabilities &capabilities,
    const SparseVoxelTerrainComputeRasterSettings &settings,
    SparseVoxelTerrainComputeRasterRouteDecision &decision,
    std::string *error = nullptr);

SparseVoxelTerrainComputeRasterClusterRoute
classifySparseVoxelTerrainComputeRasterCluster(
    const SparseVoxelTerrainComputeRasterRouteDecision &decision,
    const SparseVoxelTerrainComputeRasterSettings &settings,
    const SparseVoxelTerrainComputeRasterClusterStats &cluster);

struct SparseVoxelTerrainVGRendererBenchmarkSettings
{
  std::string worldMap = "world-data";
  uint32_t terrainRenderDistanceKilometres = 5u;
  uint32_t resolutionWidth = 1220u;
  uint32_t resolutionHeight = 720u;
  uint32_t durationSeconds = 30u;
  uint32_t warmupFrames = 120u;
  uint32_t measuredFrames = 1800u;
  bool includeSettledResidencyRerun = false;
  bool requireStreaming = true;
  bool requireGpuTimings = true;
  bool requireFullRenderer = true;
  bool sceneVisibleRasterPayloadReady = false;
  bool disableVsync = true;
};

struct SparseVoxelTerrainVGRendererBenchmarkPath
{
  SparseVoxelTerrainVGRendererBenchmarkPathKind kind =
      SparseVoxelTerrainVGRendererBenchmarkPathKind::BrickBaseline;
  std::string label;
  SparseVoxelTerrainGBufferRouteDecision route{};
  std::vector<std::string> sampleArguments;
  uint32_t durationSeconds = 0u;
  uint32_t warmupFrames = 0u;
  uint32_t measuredFrames = 0u;
  bool streamingRequired = true;
  bool gpuTimingsRequired = true;
  bool fullRendererRequired = true;
  bool lightingEnabled = true;
  bool gbufferOnly = false;
  bool settledResidencyRequired = false;
};

struct SparseVoxelTerrainVGRendererBenchmarkMetrics
{
  bool valid = false;
  SparseVoxelTerrainGBufferRoute route = SparseVoxelTerrainGBufferRoute::None;
  uint32_t measuredFrames = 0u;
  double measuredSeconds = 0.0;
  double medianFrameMilliseconds = 0.0;
  double p95FrameMilliseconds = 0.0;
  double meanFrameMilliseconds = 0.0;
  double cpuMeanFrameMilliseconds = 0.0;
  double gpuMeanFrameMilliseconds = 0.0;
  double realFramesPerSecond = 0.0;
  double virtualGeometryCullMilliseconds = 0.0;
  double virtualGeometryPrepassMilliseconds = 0.0;
  double virtualGeometryMaterialMilliseconds = 0.0;
  double virtualGeometryFinalDrawMilliseconds = 0.0;
  double brickRasterMilliseconds = 0.0;
  double traceMilliseconds = 0.0;
  double pageInstallLatencyMilliseconds = 0.0;
  double overdrawDepthComplexityProxy = 0.0;
  uint64_t submittedMeshlets = 0u;
  uint64_t submittedTriangles = 0u;
  uint64_t rejectedMeshlets = 0u;
  uint64_t visibleBricks = 0u;
  uint64_t traceRayCount = 0u;
  uint64_t unresolvedTracePageRequests = 0u;
  uint64_t terrainPixels = 0u;
  uint64_t visibleHolePixels = 0u;
  uint64_t crackPixels = 0u;
  uint64_t pagePublicationRejectionCount = 0u;
  bool usedVirtualGeometryRenderer = false;
  bool usedGPUVirtualGeometryCulling = false;
  bool usedCPUTerrainCulling = false;
  bool usedBrickFallback = false;
  bool streamingEnabled = false;
  bool lightingEnabled = true;
  bool gbufferOnly = false;
  bool liveRenderGraphUsedVGPasses = false;
  bool liveRenderGraphUsedBrickPhasePasses = false;
  bool screenTracePageIdentityMatched = false;
  bool settledResidency = false;
};

bool buildSparseVoxelTerrainVGRendererBenchmarkPlan(
    const SparseVoxelTerrainVGScenePageInstall *sceneInstall,
    const SparseVoxelTerrainVGRendererBenchmarkSettings &settings,
    std::vector<SparseVoxelTerrainVGRendererBenchmarkPath> &paths,
    std::string *error = nullptr);

bool validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
    const SparseVoxelTerrainVGRendererBenchmarkPath &path,
    const SparseVoxelTerrainVGRendererBenchmarkMetrics &metrics,
    std::string *error = nullptr);

struct SparseVoxelTerrainVGRendererBenchmarkGateReport
{
  bool passed = false;
  double fullFrameFPSRatio = 0.0;
  double gbufferTimeRatio = 0.0;
  std::string reason{};
};

bool validateSparseVoxelTerrainVGRendererBenchmarkGate(
    const SparseVoxelTerrainVGRendererBenchmarkMetrics &brickBaseline,
    const SparseVoxelTerrainVGRendererBenchmarkMetrics &virtualGeometryFull,
    const SparseVoxelTerrainVGRendererBenchmarkMetrics &virtualGeometryGBufferOnly,
    SparseVoxelTerrainVGRendererBenchmarkGateReport &report,
    std::string *error = nullptr);

} // namespace voxel::svraster
