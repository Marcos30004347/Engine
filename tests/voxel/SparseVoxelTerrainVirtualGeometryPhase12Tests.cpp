#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryBenchmark.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

bool hasArg(const std::vector<std::string> &args, const std::string &arg)
{
  return std::find(args.begin(), args.end(), arg) != args.end();
}

svr::SparseVoxelTerrainVGScenePageInstall readyInstall()
{
  svr::SparseVoxelTerrainVGScenePageInstall install{};
  install.page = {0x5445525241494e59ull, 12u};
  install.pageGeneration = 0xc001u;
  install.geometryRevision = 0xc002u;
  install.materialRevision = 0xc003u;
  install.traceFormat = svr::kVoxelTraceFormatCubic64;
  install.localPageIndex = 5u;
  install.globalPageTableIndex = 205u;
  install.prioritySlot = 305u;
  install.readyForSceneInstall = true;
  install.hasTracePayload = true;
  install.hasMaterialPayload = true;
  install.virtualPageAllocation.pageId = install.localPageIndex;
  install.virtualPageAllocation.clusterCount = 16u;
  install.virtualPageAllocation.isInstalled = true;
  install.pageTableEntry.isInstalled = 1u;
  install.pageTableEntry.clusterCount = 16u;
  return install;
}

svr::SparseVoxelTerrainVGRendererBenchmarkMetrics goodMetricsFor(
    const svr::SparseVoxelTerrainVGRendererBenchmarkPath &path)
{
  svr::SparseVoxelTerrainVGRendererBenchmarkMetrics metrics{};
  metrics.valid = true;
  metrics.route = path.route.route;
  metrics.measuredFrames = path.measuredFrames;
  metrics.measuredSeconds = static_cast<double>(path.durationSeconds);
  metrics.medianFrameMilliseconds = 16.0;
  metrics.p95FrameMilliseconds = 20.0;
  metrics.meanFrameMilliseconds = 17.0;
  metrics.cpuMeanFrameMilliseconds = 2.0;
  metrics.gpuMeanFrameMilliseconds = 12.0;
  metrics.realFramesPerSecond = 62.5;
  metrics.terrainPixels = 400000u;
  metrics.streamingEnabled = true;
  if (path.route.route == svr::SparseVoxelTerrainGBufferRoute::VirtualGeometry)
  {
    metrics.usedVirtualGeometryRenderer = true;
    metrics.usedGPUVirtualGeometryCulling = true;
    metrics.virtualGeometryCullMilliseconds = 0.5;
    metrics.virtualGeometryPrepassMilliseconds = 0.6;
    metrics.virtualGeometryMaterialMilliseconds = path.gbufferOnly ? 0.0 : 0.4;
    metrics.virtualGeometryFinalDrawMilliseconds = 1.0;
    metrics.traceMilliseconds = 1.5;
    metrics.submittedMeshlets = 2048u;
    metrics.submittedTriangles = 131072u;
    metrics.rejectedMeshlets = 4096u;
    metrics.traceRayCount = 1220u * 720u;
    metrics.lightingEnabled = path.lightingEnabled;
    metrics.gbufferOnly = path.gbufferOnly;
    metrics.liveRenderGraphUsedVGPasses = true;
    metrics.liveRenderGraphUsedBrickPhasePasses = false;
    metrics.screenTracePageIdentityMatched = true;
    metrics.settledResidency = path.settledResidencyRequired;
  }
  else
  {
    metrics.usedBrickFallback = true;
    metrics.brickRasterMilliseconds = 4.0;
    metrics.visibleBricks = 4096u;
    metrics.traceRayCount = 1220u * 720u;
    metrics.lightingEnabled = path.lightingEnabled;
    metrics.gbufferOnly = path.gbufferOnly;
    metrics.liveRenderGraphUsedVGPasses = false;
    metrics.liveRenderGraphUsedBrickPhasePasses = true;
  }
  return metrics;
}

void testPhase12BenchmarkPlanContainsBothRendererPaths()
{
  std::cout << "Phase 12 renderer benchmark plan contains brick and default VG paths\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainVGRendererBenchmarkSettings settings{};
  settings.sceneVisibleRasterPayloadReady = true;
  std::vector<svr::SparseVoxelTerrainVGRendererBenchmarkPath> paths;
  std::string error;
  check(
      svr::buildSparseVoxelTerrainVGRendererBenchmarkPlan(
          &install, settings, paths, &error),
      "benchmark plan builds from a ready Phase 10 scene install: " + error);
  check(paths.size() == 3u, "benchmark plan has exactly three required paths");
  if (paths.size() != 3u)
    return;
  const auto &brick = paths[0u];
  const auto &vg = paths[1u];
  const auto &vgGBuffer = paths[2u];
  check(brick.label == "brick-baseline", "first path is the old brick baseline");
  check(vg.label == "terrain-vg-default", "second path is the default terrain VG path");
  check(vgGBuffer.label == "terrain-vg-gbuffer-only", "third path isolates the terrain VG G-buffer");
  check(brick.route.route == svr::SparseVoxelTerrainGBufferRoute::BrickFallback, "baseline route selects brick fallback");
  check(vg.route.route == svr::SparseVoxelTerrainGBufferRoute::VirtualGeometry, "default route selects virtual geometry");
  check(vgGBuffer.route.route == svr::SparseVoxelTerrainGBufferRoute::VirtualGeometry, "G-buffer-only route selects virtual geometry");
  check(vg.route.usedDefaultVirtualGeometry, "VG path is selected by default/auto policy");
  check(!brick.route.usesCPUTerrainCulling && !vg.route.usesCPUTerrainCulling && !vgGBuffer.route.usesCPUTerrainCulling, "no benchmark path enables CPU terrain culling");
  check(brick.durationSeconds == 30u && vg.durationSeconds == 30u && vgGBuffer.durationSeconds == 30u, "all paths require a 30 second capture");
  check(brick.measuredFrames == 1800u && vg.measuredFrames == 1800u && vgGBuffer.measuredFrames == 1800u, "all paths request the same measured frame count");
  check(hasArg(brick.sampleArguments, "--gbuffer-resolver") && hasArg(brick.sampleArguments, "bricks"), "brick baseline pins the old resolver explicitly");
  check(!hasArg(vg.sampleArguments, "--gbuffer-resolver"), "VG path relies on the default route instead of a legacy resolver switch");
  check(hasArg(vgGBuffer.sampleArguments, "--gbuffer-resolver") && hasArg(vgGBuffer.sampleArguments, "terrain-vg"), "G-buffer-only path pins the VG resolver explicitly");
  check(hasArg(vgGBuffer.sampleArguments, "--disable-lighting") && !vgGBuffer.lightingEnabled && vgGBuffer.gbufferOnly, "G-buffer-only path disables lighting");
  check(hasArg(vg.sampleArguments, "--gpu-timings") && hasArg(vg.sampleArguments, "--disable-vsync"), "VG path requests GPU timings with vsync disabled");
  check(hasArg(vg.sampleArguments, "1220x720") && hasArg(vg.sampleArguments, "5"), "VG path uses the requested resolution and terrain distance");
}

void testPhase12BenchmarkPlanRefusesFakeVGRun()
{
  std::cout << "Phase 12 benchmark plan refuses to call fallback a VG run\n";
  svr::SparseVoxelTerrainVGRendererBenchmarkSettings settings{};
  std::vector<svr::SparseVoxelTerrainVGRendererBenchmarkPath> paths;
  std::string error;
  check(
      !svr::buildSparseVoxelTerrainVGRendererBenchmarkPlan(
          nullptr, settings, paths, &error),
      "benchmark plan fails without a ready VG scene install");
  check(paths.size() == 1u, "failed plan may expose only the brick baseline");
  check(error.find("default route") != std::string::npos, "failure explains that default VG routing is missing");
}

void testPhase12MetricGateChecksRouteQualityAndTimings()
{
  std::cout << "Phase 12 metric gate validates route, quality, and timing fields\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainVGRendererBenchmarkSettings settings{};
  settings.sceneVisibleRasterPayloadReady = true;
  std::vector<svr::SparseVoxelTerrainVGRendererBenchmarkPath> paths;
  std::string error;
  check(
      svr::buildSparseVoxelTerrainVGRendererBenchmarkPlan(
          &install, settings, paths, &error),
      "benchmark plan builds for metric gate: " + error);
  if (paths.size() != 2u)
    return;

  for (const auto &path : paths)
  {
    auto metrics = goodMetricsFor(path);
    check(
        svr::validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
            path, metrics, &error),
        "good metrics validate for " + path.label + ": " + error);
    metrics.usedCPUTerrainCulling = true;
    check(
        !svr::validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
            path, metrics, &error),
        "metrics using CPU terrain culling are rejected for " + path.label);
  }

  auto wrongRoute = goodMetricsFor(paths[1u]);
  wrongRoute.route = svr::SparseVoxelTerrainGBufferRoute::BrickFallback;
  wrongRoute.usedVirtualGeometryRenderer = false;
  wrongRoute.usedGPUVirtualGeometryCulling = false;
  wrongRoute.usedBrickFallback = true;
  check(
      !svr::validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
          paths[1u], wrongRoute, &error),
      "VG benchmark rejects metrics captured from the brick route");

  auto holes = goodMetricsFor(paths[1u]);
  holes.visibleHolePixels = 1u;
  check(
      !svr::validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
          paths[1u], holes, &error),
      "VG benchmark rejects visible terrain holes");
}

} // namespace

int main()
{
  testPhase12BenchmarkPlanContainsBothRendererPaths();
  testPhase12BenchmarkPlanRefusesFakeVGRun();
  testPhase12MetricGateChecksRouteQualityAndTimings();
  if (failures != 0)
  {
    std::cerr << failures << " terrain VG Phase 12 benchmark gate checks failed\n";
    return 1;
  }
  std::cout << "Sparse voxel terrain virtual-geometry Phase 12 benchmark gate tests passed\n";
  return 0;
}
