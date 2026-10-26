#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryRenderRoute.hpp"

#include <cstdint>
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

svr::SparseVoxelTerrainVGScenePageInstall readyInstall(
    const bool traceReady = true,
    const bool materialReady = true)
{
  svr::SparseVoxelTerrainVGScenePageInstall install{};
  install.page = {0x5445525241494e59ull, 7u};
  install.pageGeneration = 0xb001u;
  install.geometryRevision = 0xb001u;
  install.materialRevision = materialReady ? 0xb001u : 0u;
  install.traceFormat = traceReady ? svr::kVoxelTraceFormatCubic64 : svr::kVoxelTraceFormatInvalid;
  install.localPageIndex = 3u;
  install.globalPageTableIndex = 103u;
  install.prioritySlot = 203u;
  install.readyForSceneInstall = true;
  install.hasTracePayload = traceReady;
  install.hasMaterialPayload = materialReady;
  install.virtualPageAllocation.pageId = install.localPageIndex;
  install.virtualPageAllocation.clusterCount = 8u;
  install.virtualPageAllocation.isInstalled = true;
  install.pageTableEntry.isInstalled = 1u;
  install.pageTableEntry.clusterCount = 8u;
  return install;
}

void testDefaultRouteUsesVirtualGeometryWhenSceneInstallIsReady()
{
  std::cout << "default terrain G-buffer route uses Virtual Geometry when a scene install is ready\n";
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
      "default route selects successfully: " + error);
  check(decision.route == svr::SparseVoxelTerrainGBufferRoute::VirtualGeometry, "default route selects virtual geometry");
  check(decision.usedDefaultVirtualGeometry, "decision records the default VG route");
  check(decision.usesVirtualGeometryRenderer && decision.usesGPUVirtualGeometryCulling, "decision routes through VG renderer and GPU culling");
  check(!decision.usesCPUTerrainCulling, "decision never enables CPU terrain culling");
  check(decision.traceParityRequired && decision.traceParitySatisfied, "decision requires and satisfies trace parity");
  check(decision.sceneVisibleRasterPayloadRequired && decision.sceneVisibleRasterPayloadReady, "decision requires and satisfies scene-visible raster payload readiness");
}

void testDefaultRouteFallsBackToBrickWhenVirtualGeometryIsNotReady()
{
  std::cout << "default terrain route falls back to bricks only when VG is missing\n";
  svr::SparseVoxelTerrainGBufferRouteSettings settings{};
  svr::SparseVoxelTerrainGBufferRouteDecision decision{};
  std::string error;

  check(
      svr::selectSparseVoxelTerrainGBufferRoute(
          nullptr,
          settings,
          decision,
          &error),
      "default route with no VG install selects fallback: " + error);
  check(decision.route == svr::SparseVoxelTerrainGBufferRoute::BrickFallback, "missing VG uses brick fallback");
  check(decision.usedBrickFallback, "decision records brick fallback");
  check(!decision.usesVirtualGeometryRenderer && !decision.usesCPUTerrainCulling, "fallback does not route through the CPU culling oracle");
}

void testForcedVirtualGeometryRejectsMissingTraceParity()
{
  std::cout << "forced VG terrain route rejects pages without required trace parity\n";
  auto install = readyInstall(false, true);
  svr::SparseVoxelTerrainGBufferRouteSettings settings{};
  settings.request = svr::SparseVoxelTerrainGBufferRouteRequest::VirtualGeometry;
  settings.requireTracePayload = true;
  settings.sceneVisibleRasterPayloadReady = true;

  svr::SparseVoxelTerrainGBufferRouteDecision decision{};
  std::string error;
  check(
      !svr::selectSparseVoxelTerrainGBufferRoute(
          &install,
          settings,
          decision,
          &error),
      "forced VG route rejects missing trace payload");
  check(error.find("not ready") != std::string::npos, "error names readiness failure");
  check(decision.traceParityRequired && !decision.traceParitySatisfied, "decision reports missing trace parity");
}

void testDefaultRouteRejectsWhenFallbackDisabled()
{
  std::cout << "default terrain route fails closed when VG is missing and brick fallback is disabled\n";
  svr::SparseVoxelTerrainGBufferRouteSettings settings{};
  settings.allowBrickFallback = false;
  settings.brickFallbackAvailable = false;

  svr::SparseVoxelTerrainGBufferRouteDecision decision{};
  std::string error;
  check(
      !svr::selectSparseVoxelTerrainGBufferRoute(
          nullptr,
          settings,
          decision,
          &error),
      "route fails closed with no VG and no brick fallback");
  check(decision.route == svr::SparseVoxelTerrainGBufferRoute::None, "no route is selected on failure");
  check(error.find("fallback is disabled") != std::string::npos, "error names disabled fallback");
}

void testExplicitBrickRouteRemainsDiagnosticOverride()
{
  std::cout << "explicit brick route remains available as a diagnostic override\n";
  const auto install = readyInstall();
  svr::SparseVoxelTerrainGBufferRouteSettings settings{};
  settings.request = svr::SparseVoxelTerrainGBufferRouteRequest::BrickFallback;
  settings.brickFallbackAvailable = true;
  settings.requireSceneVisibleRasterPayload = false;

  svr::SparseVoxelTerrainGBufferRouteDecision decision{};
  std::string error;
  check(
      svr::selectSparseVoxelTerrainGBufferRoute(
          &install,
          settings,
          decision,
          &error),
      "explicit brick route selects successfully: " + error);
  check(decision.route == svr::SparseVoxelTerrainGBufferRoute::BrickFallback, "explicit brick route wins over ready VG");
  check(decision.usedBrickFallback, "decision records diagnostic brick fallback");
  check(!decision.usesCPUTerrainCulling, "diagnostic brick route does not use CPU culling");
}

void testMaterialRequirementCanKeepDefaultOnFallback()
{
  std::cout << "material parity requirement keeps default route on fallback until VG material is ready\n";
  auto install = readyInstall(true, false);
  svr::SparseVoxelTerrainGBufferRouteSettings settings{};
  settings.requireMaterialPayload = true;
  settings.sceneVisibleRasterPayloadReady = true;

  svr::SparseVoxelTerrainGBufferRouteDecision decision{};
  std::string error;
  check(
      svr::selectSparseVoxelTerrainGBufferRoute(
          &install,
          settings,
          decision,
          &error),
      "default route falls back while material parity is missing: " + error);
  check(decision.route == svr::SparseVoxelTerrainGBufferRoute::BrickFallback, "missing VG material keeps brick fallback selected");
  check(decision.materialParityRequired && !decision.materialParitySatisfied, "decision reports missing material parity");
}

} // namespace

int main()
{
  testDefaultRouteUsesVirtualGeometryWhenSceneInstallIsReady();
  testDefaultRouteFallsBackToBrickWhenVirtualGeometryIsNotReady();
  testForcedVirtualGeometryRejectsMissingTraceParity();
  testDefaultRouteRejectsWhenFallbackDisabled();
  testExplicitBrickRouteRemainsDiagnosticOverride();
  testMaterialRequirementCanKeepDefaultOnFallback();

  if (failures != 0)
  {
    std::cerr << failures << " terrain VG Phase 11 render-route checks failed\n";
    return 1;
  }

  std::cout << "Sparse voxel terrain virtual-geometry Phase 11 render-route tests passed\n";
  return 0;
}
