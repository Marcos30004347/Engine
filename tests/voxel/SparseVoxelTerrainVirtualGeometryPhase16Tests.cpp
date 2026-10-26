#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryBenchmark.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

svr::SparseVoxelTerrainVGScenePageInstall readyInstall()
{
  svr::SparseVoxelTerrainVGScenePageInstall install{};
  install.page = {0x5445525241494e59ull, 16u};
  install.pageGeneration = 0x1601u;
  install.geometryRevision = 0x1602u;
  install.materialRevision = 0x1603u;
  install.traceFormat = svr::kVoxelTraceFormatCubic64;
  install.localPageIndex = 16u;
  install.globalPageTableIndex = 116u;
  install.prioritySlot = 216u;
  install.readyForSceneInstall = true;
  install.hasTracePayload = true;
  install.hasMaterialPayload = true;
  install.virtualPageAllocation.pageId = install.localPageIndex;
  install.virtualPageAllocation.clusterCount = 64u;
  install.virtualPageAllocation.isInstalled = true;
  install.pageTableEntry.isInstalled = 1u;
  install.pageTableEntry.clusterCount = 64u;
  return install;
}

svr::SparseVoxelTerrainVGRendererBenchmarkMetrics brickMetrics()
{
  svr::SparseVoxelTerrainVGRendererBenchmarkMetrics metrics{};
  metrics.valid = true;
  metrics.route = svr::SparseVoxelTerrainGBufferRoute::BrickFallback;
  metrics.measuredFrames = 1800u;
  metrics.measuredSeconds = 30.0;
  metrics.medianFrameMilliseconds = 18.0;
  metrics.p95FrameMilliseconds = 23.0;
  metrics.meanFrameMilliseconds = 20.0;
  metrics.cpuMeanFrameMilliseconds = 2.5;
  metrics.gpuMeanFrameMilliseconds = 14.0;
  metrics.realFramesPerSecond = 50.0;
  metrics.brickRasterMilliseconds = 5.0;
  metrics.visibleBricks = 4096u;
  metrics.traceRayCount = 1220u * 720u;
  metrics.traceMilliseconds = 1.8;
  metrics.terrainPixels = 600000u;
  metrics.overdrawDepthComplexityProxy = 3.5;
  metrics.usedBrickFallback = true;
  metrics.streamingEnabled = true;
  metrics.lightingEnabled = true;
  metrics.liveRenderGraphUsedBrickPhasePasses = true;
  return metrics;
}

svr::SparseVoxelTerrainVGRendererBenchmarkMetrics vgMetrics(
    const bool gbufferOnly = false)
{
  svr::SparseVoxelTerrainVGRendererBenchmarkMetrics metrics{};
  metrics.valid = true;
  metrics.route = svr::SparseVoxelTerrainGBufferRoute::VirtualGeometry;
  metrics.measuredFrames = 1800u;
  metrics.measuredSeconds = 30.0;
  metrics.medianFrameMilliseconds = 17.0;
  metrics.p95FrameMilliseconds = 21.0;
  metrics.meanFrameMilliseconds = 18.0;
  metrics.cpuMeanFrameMilliseconds = 2.0;
  metrics.gpuMeanFrameMilliseconds = 12.0;
  metrics.realFramesPerSecond = 51.0;
  metrics.virtualGeometryCullMilliseconds = 0.55;
  metrics.virtualGeometryPrepassMilliseconds = 0.65;
  metrics.virtualGeometryMaterialMilliseconds = gbufferOnly ? 0.0 : 0.35;
  metrics.virtualGeometryFinalDrawMilliseconds = 2.0;
  metrics.traceMilliseconds = 1.6;
  metrics.pageInstallLatencyMilliseconds = 4.0;
  metrics.overdrawDepthComplexityProxy = 1.5;
  metrics.submittedMeshlets = 2048u;
  metrics.submittedTriangles = 131072u;
  metrics.rejectedMeshlets = 8192u;
  metrics.traceRayCount = 1220u * 720u;
  metrics.terrainPixels = 600000u;
  metrics.usedVirtualGeometryRenderer = true;
  metrics.usedGPUVirtualGeometryCulling = true;
  metrics.streamingEnabled = true;
  metrics.lightingEnabled = !gbufferOnly;
  metrics.gbufferOnly = gbufferOnly;
  metrics.liveRenderGraphUsedVGPasses = true;
  metrics.screenTracePageIdentityMatched = true;
  return metrics;
}

void testPhase16PlanContainsRequiredCaptures()
{
  std::cout << "Phase 16 benchmark plan contains the required capture arms\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainVGRendererBenchmarkSettings settings{};
  settings.sceneVisibleRasterPayloadReady = true;

  std::vector<svr::SparseVoxelTerrainVGRendererBenchmarkPath> paths;
  std::string error;
  check(
      svr::buildSparseVoxelTerrainVGRendererBenchmarkPlan(
          &install,
          settings,
          paths,
          &error),
      "benchmark plan builds from drawable VG payload: " + error);
  check(paths.size() == 3u, "plan contains brick, VG full, and VG G-buffer-only captures");
  if (paths.size() != 3u)
    return;
  check(paths[0u].kind == svr::SparseVoxelTerrainVGRendererBenchmarkPathKind::BrickBaseline, "first capture is brick baseline");
  check(paths[1u].kind == svr::SparseVoxelTerrainVGRendererBenchmarkPathKind::VirtualGeometryDefault, "second capture is default VG full renderer");
  check(paths[2u].kind == svr::SparseVoxelTerrainVGRendererBenchmarkPathKind::VirtualGeometryGBufferOnly, "third capture is VG G-buffer-only");
  check(paths[0u].durationSeconds == 30u && paths[1u].durationSeconds == 30u && paths[2u].durationSeconds == 30u, "each capture is a 30 second benchmark");
  check(paths[2u].gbufferOnly && !paths[2u].lightingEnabled, "G-buffer-only capture disables lighting");
}

void testPhase16SettledResidencyCaptureIsOptional()
{
  std::cout << "Phase 16 can request an optional settled-residency rerun\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainVGRendererBenchmarkSettings settings{};
  settings.sceneVisibleRasterPayloadReady = true;
  settings.includeSettledResidencyRerun = true;

  std::vector<svr::SparseVoxelTerrainVGRendererBenchmarkPath> paths;
  std::string error;
  check(
      svr::buildSparseVoxelTerrainVGRendererBenchmarkPlan(
          &install,
          settings,
          paths,
          &error),
      "settled-residency benchmark plan builds: " + error);
  check(paths.size() == 4u, "optional settled-residency plan adds a fourth capture");
  if (paths.size() == 4u)
  {
    check(paths[3u].kind == svr::SparseVoxelTerrainVGRendererBenchmarkPathKind::VirtualGeometrySettledResidency, "fourth capture is the settled-residency VG rerun");
    check(paths[3u].settledResidencyRequired, "settled-residency capture records the residency requirement");
  }
}

void testPhase16PerPathMetricsRejectWrongGraphAndIdentity()
{
  std::cout << "Phase 16 path metrics reject wrong graph routes and page identity mismatches\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainVGRendererBenchmarkSettings settings{};
  settings.sceneVisibleRasterPayloadReady = true;
  std::vector<svr::SparseVoxelTerrainVGRendererBenchmarkPath> paths;
  std::string error;
  check(
      svr::buildSparseVoxelTerrainVGRendererBenchmarkPlan(
          &install,
          settings,
          paths,
          &error),
      "benchmark plan builds for metric validation: " + error);
  if (paths.size() != 3u)
    return;

  auto vg = vgMetrics(false);
  check(
      svr::validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
          paths[1u],
          vg,
          &error),
      "valid VG full-renderer metrics pass: " + error);
  vg.liveRenderGraphUsedBrickPhasePasses = true;
  check(
      !svr::validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
          paths[1u],
          vg,
          &error),
      "VG metrics reject mixed brick graph passes");

  vg = vgMetrics(false);
  vg.screenTracePageIdentityMatched = false;
  check(
      !svr::validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
          paths[1u],
          vg,
          &error),
      "VG metrics reject screen/trace page identity mismatches");

  auto brick = brickMetrics();
  check(
      svr::validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
          paths[0u],
          brick,
          &error),
      "valid brick baseline metrics pass: " + error);
  brick.liveRenderGraphUsedVGPasses = true;
  check(
      !svr::validateSparseVoxelTerrainVGRendererBenchmarkMetrics(
          paths[0u],
          brick,
          &error),
      "brick metrics reject mixed VG graph passes");
}

void testPhase16GateAcceptsOnlyFasterCleanVG()
{
  std::cout << "Phase 16 benchmark gate accepts only clean baseline-neutral VG\n";
  auto brick = brickMetrics();
  auto vgFull = vgMetrics(false);
  auto vgGBuffer = vgMetrics(true);
  svr::SparseVoxelTerrainVGRendererBenchmarkGateReport report{};
  std::string error;

  check(
      svr::validateSparseVoxelTerrainVGRendererBenchmarkGate(
          brick,
          vgFull,
          vgGBuffer,
          report,
          &error),
      "clean faster VG metrics pass gate: " + error);
  check(report.passed, "gate report records pass");
  check(report.fullFrameFPSRatio >= 1.0, "VG full renderer is baseline-neutral or faster");
  check(report.gbufferTimeRatio < 0.90, "VG G-buffer time is materially lower than brick raster");

  auto slowFull = vgMetrics(false);
  slowFull.realFramesPerSecond = 45.0;
  check(
      !svr::validateSparseVoxelTerrainVGRendererBenchmarkGate(
          brick,
          slowFull,
          vgGBuffer,
          report,
          &error),
      "gate rejects full-frame regression");

  auto slowGBuffer = vgMetrics(true);
  slowGBuffer.virtualGeometryFinalDrawMilliseconds = 4.0;
  check(
      !svr::validateSparseVoxelTerrainVGRendererBenchmarkGate(
          brick,
          vgFull,
          slowGBuffer,
          report,
          &error),
      "gate rejects insufficient raw G-buffer improvement");

  auto cracked = vgMetrics(false);
  cracked.crackPixels = 1u;
  check(
      !svr::validateSparseVoxelTerrainVGRendererBenchmarkGate(
          brick,
          cracked,
          vgGBuffer,
          report,
          &error),
      "gate rejects terrain cracks");
}

} // namespace

int main()
{
  testPhase16PlanContainsRequiredCaptures();
  testPhase16SettledResidencyCaptureIsOptional();
  testPhase16PerPathMetricsRejectWrongGraphAndIdentity();
  testPhase16GateAcceptsOnlyFasterCleanVG();

  if (failures != 0)
  {
    std::cerr << failures << " terrain VG Phase 16 benchmark gate checks failed\n";
    return 1;
  }

  std::cout << "Sparse voxel terrain virtual-geometry Phase 16 benchmark gate tests passed\n";
  return 0;
}
