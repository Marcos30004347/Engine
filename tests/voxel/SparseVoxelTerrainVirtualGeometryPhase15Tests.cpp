#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryBenchmark.hpp"
#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryRenderRoute.hpp"

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
  install.page = {0x5445525241494e59ull, 15u};
  install.pageGeneration = 0xf001u;
  install.geometryRevision = 0xf002u;
  install.materialRevision = 0xf003u;
  install.traceFormat = svr::kVoxelTraceFormatCubic64;
  install.localPageIndex = 15u;
  install.globalPageTableIndex = 115u;
  install.prioritySlot = 215u;
  install.readyForSceneInstall = true;
  install.hasTracePayload = true;
  install.hasMaterialPayload = true;
  install.virtualPageAllocation.pageId = install.localPageIndex;
  install.virtualPageAllocation.clusterCount = 32u;
  install.virtualPageAllocation.isInstalled = true;
  install.pageTableEntry.isInstalled = 1u;
  install.pageTableEntry.clusterCount = 32u;
  return install;
}

void testMetadataOnlyInstallFallsBackInAuto()
{
  std::cout << "Phase 15 auto route refuses metadata-only terrain VG\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainGBufferRouteSettings settings{};
  settings.sceneVisibleRasterPayloadReady = false;

  svr::SparseVoxelTerrainGBufferRouteDecision decision{};
  std::string error;
  check(
      svr::selectSparseVoxelTerrainGBufferRoute(
          &install,
          settings,
          decision,
          &error),
      "auto route selects a safe fallback: " + error);
  check(decision.route == svr::SparseVoxelTerrainGBufferRoute::BrickFallback, "metadata-only install does not select VG");
  check(decision.terrainVGSceneInstallReady, "decision still records the Phase 14 scene install readiness");
  check(decision.traceParityRequired && decision.traceParitySatisfied, "trace parity remains visible to diagnostics");
  check(decision.sceneVisibleRasterPayloadRequired && !decision.sceneVisibleRasterPayloadReady, "missing scene-visible raster payload is reported");
  check(decision.usedBrickFallback && !decision.usesCPUTerrainCulling, "fallback stays GPU-side and never enables CPU terrain culling");
}

void testForcedVirtualGeometryFailsWithoutSceneVisiblePayload()
{
  std::cout << "Phase 15 forced VG route fails closed without scene-visible payload\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainGBufferRouteSettings settings{};
  settings.request = svr::SparseVoxelTerrainGBufferRouteRequest::VirtualGeometry;
  settings.sceneVisibleRasterPayloadReady = false;

  svr::SparseVoxelTerrainGBufferRouteDecision decision{};
  std::string error;
  check(
      !svr::selectSparseVoxelTerrainGBufferRoute(
          &install,
          settings,
          decision,
          &error),
      "forced VG route rejects metadata-only payload");
  check(decision.route == svr::SparseVoxelTerrainGBufferRoute::None, "failed forced route selects no renderer");
  check(error.find("scene-visible raster payload") != std::string::npos, "error names the missing drawable payload");
}

void testAutoRouteUsesVGWhenSceneVisiblePayloadIsReady()
{
  std::cout << "Phase 15 auto route can select terrain VG once drawable payload exists\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainGBufferRouteSettings settings{};
  settings.sceneVisibleRasterPayloadReady = true;

  svr::SparseVoxelTerrainGBufferRouteDecision decision{};
  std::string error;
  check(
      svr::selectSparseVoxelTerrainGBufferRoute(
          &install,
          settings,
          decision,
          &error),
      "auto route selects successfully with drawable payload: " + error);
  check(decision.route == svr::SparseVoxelTerrainGBufferRoute::VirtualGeometry, "drawable payload selects VG");
  check(decision.usedDefaultVirtualGeometry, "VG was selected by default policy");
  check(decision.usesVirtualGeometryRenderer && decision.usesGPUVirtualGeometryCulling, "VG path uses renderer GPU culling");
  check(!decision.usesCPUTerrainCulling, "VG path does not use CPU terrain culling");
}

void testAutoRouteUsesVGWhenInstallCarriesDrawablePayload()
{
  std::cout << "Phase 15 auto route trusts drawable payload on the page install\n";
  auto install = readyInstall();
  install.sceneVisibleRasterPayloadReady = true;
  install.encodedPageWords = {1u};
  svr::SparseVoxelTerrainGBufferRouteSettings settings{};
  settings.sceneVisibleRasterPayloadReady = false;

  svr::SparseVoxelTerrainGBufferRouteDecision decision{};
  std::string error;
  check(
      svr::selectSparseVoxelTerrainGBufferRoute(
          &install,
          settings,
          decision,
          &error),
      "auto route selects successfully from install-owned drawable payload: " + error);
  check(decision.route == svr::SparseVoxelTerrainGBufferRoute::VirtualGeometry, "install-owned drawable payload selects VG");
  check(decision.sceneVisibleRasterPayloadReady, "decision reports drawable payload readiness from install");
  check(decision.usesVirtualGeometryRenderer && decision.usesGPUVirtualGeometryCulling, "install-owned VG payload uses normal VG renderer culling");
}

void testBenchmarkGateRequiresSceneVisiblePayload()
{
  std::cout << "Phase 15 benchmark gate refuses fake VG captures\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainVGRendererBenchmarkSettings settings{};
  settings.sceneVisibleRasterPayloadReady = false;

  std::vector<svr::SparseVoxelTerrainVGRendererBenchmarkPath> paths;
  std::string error;
  check(
      !svr::buildSparseVoxelTerrainVGRendererBenchmarkPlan(
          &install,
          settings,
          paths,
          &error),
      "benchmark plan rejects metadata-only VG route");
  check(paths.size() == 1u, "failed benchmark plan exposes only the brick baseline");
  check(error.find("ready virtual-geometry scene install") != std::string::npos, "benchmark error keeps the readiness wording");

  settings.sceneVisibleRasterPayloadReady = true;
  check(
      svr::buildSparseVoxelTerrainVGRendererBenchmarkPlan(
          &install,
          settings,
          paths,
          &error),
      "benchmark plan accepts drawable VG payload: " + error);
  check(paths.size() == 3u, "drawable payload benchmark contains brick, VG, and VG G-buffer-only paths");
  if (paths.size() == 3u)
    check(paths[1u].route.sceneVisibleRasterPayloadReady, "VG benchmark path records drawable payload readiness");
}

} // namespace

int main()
{
  testMetadataOnlyInstallFallsBackInAuto();
  testForcedVirtualGeometryFailsWithoutSceneVisiblePayload();
  testAutoRouteUsesVGWhenSceneVisiblePayloadIsReady();
  testAutoRouteUsesVGWhenInstallCarriesDrawablePayload();
  testBenchmarkGateRequiresSceneVisiblePayload();

  if (failures != 0)
  {
    std::cerr << failures << " terrain VG Phase 15 route/publisher checks failed\n";
    return 1;
  }

  std::cout << "Sparse voxel terrain virtual-geometry Phase 15 route/publisher tests passed\n";
  return 0;
}
