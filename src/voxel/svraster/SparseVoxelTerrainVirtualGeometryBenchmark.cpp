#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryBenchmark.hpp"

#include <algorithm>
#include <cmath>

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

std::string resolutionString(
    const SparseVoxelTerrainVGRendererBenchmarkSettings &settings)
{
  return std::to_string(settings.resolutionWidth) + "x" +
      std::to_string(settings.resolutionHeight);
}

std::vector<std::string> commonArguments(
    const SparseVoxelTerrainVGRendererBenchmarkSettings &settings)
{
  std::vector<std::string> args{
      "--world-map",
      settings.worldMap,
      "--terrain-render-distance-km",
      std::to_string(settings.terrainRenderDistanceKilometres),
      "--resolution",
      resolutionString(settings),
      "--benchmark-sequence",
      "static",
      "--benchmark-warmup",
      std::to_string(settings.warmupFrames),
      "--benchmark-frames",
      std::to_string(settings.measuredFrames),
  };
  if (settings.requireGpuTimings)
    args.push_back("--gpu-timings");
  if (settings.disableVsync)
    args.push_back("--disable-vsync");
  return args;
}

bool containsArgument(
    const std::vector<std::string> &arguments,
    const std::string &argument)
{
  return std::find(arguments.begin(), arguments.end(), argument) !=
      arguments.end();
}

double virtualGeometryGBufferMilliseconds(
    const SparseVoxelTerrainVGRendererBenchmarkMetrics &metrics)
{
  return metrics.virtualGeometryCullMilliseconds +
      metrics.virtualGeometryPrepassMilliseconds +
      metrics.virtualGeometryMaterialMilliseconds +
      metrics.virtualGeometryFinalDrawMilliseconds;
}

void fillCommonPathFields(
    SparseVoxelTerrainVGRendererBenchmarkPath &path,
    const SparseVoxelTerrainVGRendererBenchmarkSettings &settings)
{
  path.durationSeconds = settings.durationSeconds;
  path.warmupFrames = settings.warmupFrames;
  path.measuredFrames = settings.measuredFrames;
  path.streamingRequired = settings.requireStreaming;
  path.gpuTimingsRequired = settings.requireGpuTimings;
  path.fullRendererRequired = settings.requireFullRenderer;
}

} // namespace

bool selectSparseVoxelTerrainComputeRasterRoute(
    const SparseVoxelTerrainComputeRasterCapabilities &capabilities,
    const SparseVoxelTerrainComputeRasterSettings &settings,
    SparseVoxelTerrainComputeRasterRouteDecision &decision,
    std::string *error)
{
  decision = {};
  if (error != nullptr)
    error->clear();

  if (settings.projectedTriangleEdgePixelThreshold <= 0.0)
    return fail(error, "VG compute raster needs a positive projected triangle-edge threshold");
  if (settings.tileCandidateCapacity == 0u)
    return fail(error, "VG compute raster needs non-zero tile candidate capacity");

  if (settings.request == SparseVoxelTerrainComputeRasterRequest::Disabled)
  {
    decision.reason = "VG compute raster disabled by settings";
    return true;
  }

  if (settings.requireVirtualGeometryGBufferRoute &&
      settings.gbufferRoute != SparseVoxelTerrainGBufferRoute::VirtualGeometry)
  {
    const std::string message =
        "VG compute raster requires the virtual-geometry G-buffer route; "
        "brick fallback and legacy sparse-voxel resolvers keep using their "
        "own raster paths";
    if (settings.request == SparseVoxelTerrainComputeRasterRequest::Force)
      return fail(error, message);
    decision.reason = message;
    return true;
  }

  const bool nativeMetalAtomic64 =
      settings.allowNativeMetalInterop &&
      capabilities.platformApple &&
      capabilities.nativeMetalDeviceAvailable &&
      capabilities.nativeMetalQueueCompatibleWithMoltenVK &&
      capabilities.nativeMetalBufferInteropAvailable &&
      capabilities.nativeMetalSupportsAtomic64MinMax;
  if (nativeMetalAtomic64)
  {
    decision.backend = SparseVoxelTerrainComputeRasterBackend::NativeMetalAtomic64;
    decision.enabled = true;
    decision.allocateComputeRasterBuffers = true;
    decision.registerComputeRasterPasses = true;
    decision.bypassesMoltenVKAtomic64Feature =
        capabilities.moltenVKActive &&
        !capabilities.vulkanShaderBufferInt64Atomics;
    decision.usesPacked64DepthPayloadAtomic = true;
    decision.reason =
        decision.bypassesMoltenVKAtomic64Feature
        ? "native Metal compute raster uses Metal atomic64 directly while MoltenVK keeps Vulkan atomic64 disabled"
        : "native Metal compute raster selected";
    return true;
  }

  const bool vulkanTiled32 =
      settings.allowVulkanTiled32Fallback &&
      capabilities.vulkanShaderBufferInt32Atomics &&
      settings.tileCandidateCapacity <= 4096u &&
      (capabilities.maximumCandidatesPerTile == 0u ||
          capabilities.maximumCandidatesPerTile <= settings.tileCandidateCapacity);
  if (vulkanTiled32)
  {
    decision.backend = SparseVoxelTerrainComputeRasterBackend::VulkanTiledAtomic32;
    decision.enabled = true;
    decision.allocateComputeRasterBuffers = true;
    decision.registerComputeRasterPasses = true;
    decision.usesPacked32TileDepthSlotAtomic = true;
    decision.reason =
        "Vulkan/MoltenVK compute raster selected with 32-bit tiled depth-slot packing";
    return true;
  }

  const std::string message =
      "VG compute raster is unsupported: needs native Metal interop with "
      "atomic64 min/max or the bounded 32-bit tiled fallback";
  if (settings.request == SparseVoxelTerrainComputeRasterRequest::Force)
    return fail(error, message);

  decision.reason = message;
  return true;
}

SparseVoxelTerrainComputeRasterClusterRoute
classifySparseVoxelTerrainComputeRasterCluster(
    const SparseVoxelTerrainComputeRasterRouteDecision &decision,
    const SparseVoxelTerrainComputeRasterSettings &settings,
    const SparseVoxelTerrainComputeRasterClusterStats &cluster)
{
  if (!decision.enabled)
    return SparseVoxelTerrainComputeRasterClusterRoute::Hardware;
  if (!cluster.fullyResident)
    return SparseVoxelTerrainComputeRasterClusterRoute::Hardware;
  if (cluster.maximumCandidatesPerTile > settings.tileCandidateCapacity)
    return SparseVoxelTerrainComputeRasterClusterRoute::HardwareOverflowFallback;

  const bool smallTriangles =
      cluster.averageProjectedTriangleEdgePixels > 0.0 &&
      cluster.averageProjectedTriangleEdgePixels <
          settings.projectedTriangleEdgePixelThreshold;
  const bool highOverdraw =
      cluster.overdrawDepthComplexityProxy >= settings.highOverdrawThreshold;
  return (smallTriangles || highOverdraw)
      ? SparseVoxelTerrainComputeRasterClusterRoute::Compute
      : SparseVoxelTerrainComputeRasterClusterRoute::Hardware;
}

bool buildSparseVoxelTerrainVGRendererBenchmarkPlan(
    const SparseVoxelTerrainVGScenePageInstall *sceneInstall,
    const SparseVoxelTerrainVGRendererBenchmarkSettings &settings,
    std::vector<SparseVoxelTerrainVGRendererBenchmarkPath> &paths,
    std::string *error)
{
  paths.clear();
  if (error != nullptr)
    error->clear();
  if (settings.worldMap.empty())
    return fail(error, "terrain VG renderer benchmark needs a world-map path");
  if (settings.resolutionWidth == 0u || settings.resolutionHeight == 0u)
    return fail(error, "terrain VG renderer benchmark needs a non-zero resolution");
  if (settings.durationSeconds < 30u)
    return fail(error, "terrain VG renderer benchmark must run for at least 30 seconds");
  if (settings.measuredFrames == 0u)
    return fail(error, "terrain VG renderer benchmark needs measured frames");

  auto baseArgs = commonArguments(settings);

  SparseVoxelTerrainGBufferRouteSettings brickSettings{};
  brickSettings.request = SparseVoxelTerrainGBufferRouteRequest::BrickFallback;
  brickSettings.requireTracePayload = false;
  brickSettings.requireSceneVisibleRasterPayload = false;
  SparseVoxelTerrainGBufferRouteDecision brickDecision{};
  if (!selectSparseVoxelTerrainGBufferRoute(
          sceneInstall, brickSettings, brickDecision, error))
    return false;
  SparseVoxelTerrainVGRendererBenchmarkPath brick{};
  brick.kind = SparseVoxelTerrainVGRendererBenchmarkPathKind::BrickBaseline;
  brick.label = "brick-baseline";
  brick.route = brickDecision;
  brick.sampleArguments = baseArgs;
  brick.sampleArguments.push_back("--gbuffer-resolver");
  brick.sampleArguments.push_back("bricks");
  brick.lightingEnabled = true;
  fillCommonPathFields(brick, settings);
  paths.push_back(std::move(brick));

  SparseVoxelTerrainGBufferRouteSettings vgSettings{};
  vgSettings.request = SparseVoxelTerrainGBufferRouteRequest::Auto;
  vgSettings.allowBrickFallback = false;
  vgSettings.requireTracePayload = true;
  vgSettings.sceneVisibleRasterPayloadReady =
      settings.sceneVisibleRasterPayloadReady;
  SparseVoxelTerrainGBufferRouteDecision vgDecision{};
  if (!selectSparseVoxelTerrainGBufferRoute(
          sceneInstall, vgSettings, vgDecision, error))
    return fail(
        error,
        "terrain VG renderer benchmark cannot start until the default route "
        "selects a ready virtual-geometry scene install");
  if (vgDecision.route != SparseVoxelTerrainGBufferRoute::VirtualGeometry)
    return fail(
        error,
        "terrain VG renderer benchmark default route did not select virtual geometry");

  SparseVoxelTerrainVGRendererBenchmarkPath vg{};
  vg.kind =
      SparseVoxelTerrainVGRendererBenchmarkPathKind::VirtualGeometryDefault;
  vg.label = "terrain-vg-default";
  vg.route = vgDecision;
  // No explicit resolver switch here: Phase 12 is specifically checking that
  // the renderer default goes through the Phase 11 auto route.
  vg.sampleArguments = baseArgs;
  vg.lightingEnabled = true;
  fillCommonPathFields(vg, settings);
  paths.push_back(std::move(vg));

  SparseVoxelTerrainVGRendererBenchmarkPath vgGBuffer{};
  vgGBuffer.kind =
      SparseVoxelTerrainVGRendererBenchmarkPathKind::VirtualGeometryGBufferOnly;
  vgGBuffer.label = "terrain-vg-gbuffer-only";
  vgGBuffer.route = vgDecision;
  vgGBuffer.sampleArguments = baseArgs;
  vgGBuffer.sampleArguments.push_back("--gbuffer-resolver");
  vgGBuffer.sampleArguments.push_back("terrain-vg");
  vgGBuffer.sampleArguments.push_back("--disable-lighting");
  vgGBuffer.lightingEnabled = false;
  vgGBuffer.gbufferOnly = true;
  fillCommonPathFields(vgGBuffer, settings);
  paths.push_back(std::move(vgGBuffer));

  if (settings.includeSettledResidencyRerun)
  {
    SparseVoxelTerrainVGRendererBenchmarkPath settled{};
    settled.kind = SparseVoxelTerrainVGRendererBenchmarkPathKind::
        VirtualGeometrySettledResidency;
    settled.label = "terrain-vg-settled-residency";
    settled.route = vgDecision;
    settled.sampleArguments = baseArgs;
    settled.sampleArguments.push_back("--gbuffer-resolver");
    settled.sampleArguments.push_back("terrain-vg");
    settled.sampleArguments.push_back("--terrain-freeze-requests");
    settled.lightingEnabled = true;
    settled.settledResidencyRequired = true;
    fillCommonPathFields(settled, settings);
    paths.push_back(std::move(settled));
  }

  return true;
}

bool validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
    const SparseVoxelTerrainVGRendererBenchmarkPath &path,
    const SparseVoxelTerrainVGRendererBenchmarkMetrics &metrics,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!metrics.valid)
    return fail(error, "terrain VG renderer benchmark metrics are not valid");
  if (metrics.route != path.route.route)
    return fail(error, "terrain VG renderer benchmark metrics came from the wrong G-buffer route");
  if (metrics.measuredFrames == 0u ||
      (path.measuredFrames != 0u && metrics.measuredFrames < path.measuredFrames))
    return fail(error, "terrain VG renderer benchmark did not capture the requested measured frames");
  if (metrics.measuredSeconds + 1.0e-6 < static_cast<double>(path.durationSeconds))
    return fail(error, "terrain VG renderer benchmark did not run for the required duration");
  if (metrics.medianFrameMilliseconds <= 0.0 ||
      metrics.p95FrameMilliseconds < metrics.medianFrameMilliseconds ||
      metrics.meanFrameMilliseconds <= 0.0 ||
      metrics.realFramesPerSecond <= 0.0)
    return fail(error, "terrain VG renderer benchmark frame timing metrics are incomplete");
  if (path.streamingRequired && !metrics.streamingEnabled)
    return fail(error, "terrain VG renderer benchmark did not run with streaming enabled");
  if (path.gbufferOnly && metrics.lightingEnabled)
    return fail(error, "terrain VG G-buffer-only benchmark ran with lighting enabled");
  if (!path.gbufferOnly && metrics.gbufferOnly)
    return fail(error, "full renderer benchmark was captured as G-buffer-only");
  if (path.settledResidencyRequired && !metrics.settledResidency)
    return fail(error, "settled-residency benchmark was captured before residency settled");
  if (metrics.usedCPUTerrainCulling)
    return fail(error, "terrain VG renderer benchmark used CPU terrain culling");
  if (metrics.terrainPixels == 0u)
    return fail(error, "terrain VG renderer benchmark rendered no terrain pixels");
  if (metrics.visibleHolePixels != 0u || metrics.crackPixels != 0u)
    return fail(error, "terrain VG renderer benchmark found terrain holes or cracks");

  if (path.route.route == SparseVoxelTerrainGBufferRoute::VirtualGeometry)
  {
    if (!metrics.usedVirtualGeometryRenderer ||
        !metrics.usedGPUVirtualGeometryCulling)
      return fail(error, "terrain VG benchmark did not use the VG renderer and GPU culling");
    if (!metrics.liveRenderGraphUsedVGPasses ||
        metrics.liveRenderGraphUsedBrickPhasePasses)
      return fail(error, "terrain VG benchmark render graph did not use the VG pass route exclusively");
    if (!metrics.screenTracePageIdentityMatched)
      return fail(error, "terrain VG benchmark screen and trace page identities did not match");
    if (metrics.submittedMeshlets == 0u || metrics.submittedTriangles == 0u)
      return fail(error, "terrain VG benchmark submitted no meshlets or triangles");
    if (metrics.traceRayCount == 0u)
      return fail(error, "terrain VG benchmark captured no trace rays");
    if (virtualGeometryGBufferMilliseconds(metrics) <= 0.0)
      return fail(error, "terrain VG benchmark is missing VG G-buffer timing");
    if (metrics.traceMilliseconds <= 0.0)
      return fail(error, "terrain VG benchmark is missing trace timing");
  }
  else if (path.route.route == SparseVoxelTerrainGBufferRoute::BrickFallback)
  {
    if (!metrics.usedBrickFallback || metrics.visibleBricks == 0u)
      return fail(error, "brick baseline benchmark captured no visible brick work");
    if (!metrics.liveRenderGraphUsedBrickPhasePasses ||
        metrics.liveRenderGraphUsedVGPasses)
      return fail(error, "brick baseline render graph did not use the brick route exclusively");
    if (containsArgument(path.sampleArguments, "--gpu-timings") &&
        path.gpuTimingsRequired && metrics.brickRasterMilliseconds <= 0.0)
      return fail(error, "brick baseline benchmark is missing brick raster timing");
  }
  return true;
}

bool validateSparseVoxelTerrainVGRendererBenchmarkGate(
    const SparseVoxelTerrainVGRendererBenchmarkMetrics &brickBaseline,
    const SparseVoxelTerrainVGRendererBenchmarkMetrics &virtualGeometryFull,
    const SparseVoxelTerrainVGRendererBenchmarkMetrics &virtualGeometryGBufferOnly,
    SparseVoxelTerrainVGRendererBenchmarkGateReport &report,
    std::string *error)
{
  report = {};
  if (error != nullptr)
    error->clear();

  if (!brickBaseline.valid || !virtualGeometryFull.valid ||
      !virtualGeometryGBufferOnly.valid)
    return fail(error, "terrain VG renderer gate needs three valid benchmark captures");
  if (brickBaseline.route != SparseVoxelTerrainGBufferRoute::BrickFallback)
    return fail(error, "terrain VG renderer gate baseline metrics are not from the brick route");
  if (virtualGeometryFull.route != SparseVoxelTerrainGBufferRoute::VirtualGeometry ||
      virtualGeometryGBufferOnly.route != SparseVoxelTerrainGBufferRoute::VirtualGeometry)
    return fail(error, "terrain VG renderer gate needs VG metrics for both VG captures");
  if (brickBaseline.usedCPUTerrainCulling ||
      virtualGeometryFull.usedCPUTerrainCulling ||
      virtualGeometryGBufferOnly.usedCPUTerrainCulling)
    return fail(error, "terrain VG renderer gate captured CPU terrain culling");
  if (!brickBaseline.streamingEnabled || !virtualGeometryFull.streamingEnabled ||
      !virtualGeometryGBufferOnly.streamingEnabled)
    return fail(error, "terrain VG renderer gate requires streaming-enabled captures");
  if (!virtualGeometryFull.liveRenderGraphUsedVGPasses ||
      virtualGeometryFull.liveRenderGraphUsedBrickPhasePasses ||
      !virtualGeometryGBufferOnly.liveRenderGraphUsedVGPasses ||
      virtualGeometryGBufferOnly.liveRenderGraphUsedBrickPhasePasses)
    return fail(error, "terrain VG renderer gate did not capture the live VG pass route");
  if (!brickBaseline.liveRenderGraphUsedBrickPhasePasses ||
      brickBaseline.liveRenderGraphUsedVGPasses)
    return fail(error, "terrain VG renderer gate baseline did not capture the brick pass route");
  if (!virtualGeometryFull.screenTracePageIdentityMatched ||
      !virtualGeometryGBufferOnly.screenTracePageIdentityMatched)
    return fail(error, "terrain VG renderer gate screen and trace page identities differ");
  if (brickBaseline.terrainPixels == 0u ||
      virtualGeometryFull.terrainPixels == 0u ||
      virtualGeometryGBufferOnly.terrainPixels == 0u)
    return fail(error, "terrain VG renderer gate has an empty terrain capture");
  if (brickBaseline.visibleHolePixels != 0u ||
      brickBaseline.crackPixels != 0u ||
      virtualGeometryFull.visibleHolePixels != 0u ||
      virtualGeometryFull.crackPixels != 0u ||
      virtualGeometryGBufferOnly.visibleHolePixels != 0u ||
      virtualGeometryGBufferOnly.crackPixels != 0u)
    return fail(error, "terrain VG renderer gate found terrain holes or cracks");
  if (virtualGeometryGBufferOnly.lightingEnabled ||
      !virtualGeometryGBufferOnly.gbufferOnly)
    return fail(error, "terrain VG renderer gate G-buffer-only capture did not disable lighting");

  const double vgGBufferMs =
      virtualGeometryGBufferMilliseconds(virtualGeometryGBufferOnly);
  if (brickBaseline.brickRasterMilliseconds <= 0.0 || vgGBufferMs <= 0.0)
    return fail(error, "terrain VG renderer gate is missing comparable G-buffer timings");
  if (brickBaseline.realFramesPerSecond <= 0.0 ||
      virtualGeometryFull.realFramesPerSecond <= 0.0)
    return fail(error, "terrain VG renderer gate is missing comparable FPS timings");

  report.fullFrameFPSRatio =
      virtualGeometryFull.realFramesPerSecond /
      brickBaseline.realFramesPerSecond;
  report.gbufferTimeRatio =
      vgGBufferMs / brickBaseline.brickRasterMilliseconds;

  constexpr double kFullFrameNeutralFPSRatio = 0.98;
  constexpr double kMaterialGBufferImprovementRatio = 0.90;
  if (report.fullFrameFPSRatio < kFullFrameNeutralFPSRatio)
  {
    report.reason = "terrain VG full-frame FPS is below the baseline-neutral threshold";
    if (error != nullptr)
      *error = report.reason;
    return false;
  }
  if (report.gbufferTimeRatio > kMaterialGBufferImprovementRatio)
  {
    report.reason = "terrain VG raw G-buffer time is not materially lower than brick raster";
    if (error != nullptr)
      *error = report.reason;
    return false;
  }

  report.passed = true;
  report.reason = "terrain VG renderer benchmark gate passed";
  return true;
}

} // namespace voxel::svraster
