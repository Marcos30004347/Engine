#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryRenderRoute.hpp"

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

bool sceneInstallSatisfiesRoute(
    const SparseVoxelTerrainVGScenePageInstall *sceneInstall,
    const SparseVoxelTerrainGBufferRouteSettings &settings,
    SparseVoxelTerrainGBufferRouteDecision &decision)
{
  decision.terrainVGSceneInstallReady =
      sceneInstall != nullptr && sceneInstall->readyForSceneInstall;
  decision.traceParityRequired = settings.requireTracePayload;
  decision.materialParityRequired = settings.requireMaterialPayload;
  decision.sceneVisibleRasterPayloadRequired =
      settings.requireSceneVisibleRasterPayload;
  decision.traceParitySatisfied =
      sceneInstall != nullptr && sceneInstall->hasTracePayload;
  decision.materialParitySatisfied =
      !settings.requireMaterialPayload ||
      (sceneInstall != nullptr && sceneInstall->hasMaterialPayload);
  const bool installHasDrawablePayload =
      sceneInstall != nullptr &&
      sparseVoxelTerrainVGSceneInstallHasDrawablePayload(*sceneInstall);
  decision.sceneVisibleRasterPayloadReady =
      !settings.requireSceneVisibleRasterPayload ||
      settings.sceneVisibleRasterPayloadReady ||
      installHasDrawablePayload;

  return decision.terrainVGSceneInstallReady &&
      (!settings.requireTracePayload || decision.traceParitySatisfied) &&
      decision.materialParitySatisfied &&
      decision.sceneVisibleRasterPayloadReady;
}

void chooseVirtualGeometry(
    SparseVoxelTerrainGBufferRouteDecision &decision,
    const char *reason,
    const bool usedDefault)
{
  decision.route = SparseVoxelTerrainGBufferRoute::VirtualGeometry;
  decision.usedDefaultVirtualGeometry = usedDefault;
  decision.usedBrickFallback = false;
  decision.usesVirtualGeometryRenderer = true;
  decision.usesGPUVirtualGeometryCulling = true;
  decision.usesCPUTerrainCulling = false;
  decision.reason = reason;
}

void chooseBrickFallback(
    SparseVoxelTerrainGBufferRouteDecision &decision,
    const char *reason)
{
  decision.route = SparseVoxelTerrainGBufferRoute::BrickFallback;
  decision.usedDefaultVirtualGeometry = false;
  decision.usedBrickFallback = true;
  decision.usesVirtualGeometryRenderer = false;
  decision.usesGPUVirtualGeometryCulling = false;
  decision.usesCPUTerrainCulling = false;
  decision.reason = reason;
}

} // namespace

const char *sparseVoxelTerrainGBufferRouteName(
    const SparseVoxelTerrainGBufferRoute route)
{
  switch (route)
  {
  case SparseVoxelTerrainGBufferRoute::None:
    return "none";
  case SparseVoxelTerrainGBufferRoute::VirtualGeometry:
    return "virtual-geometry";
  case SparseVoxelTerrainGBufferRoute::BrickFallback:
    return "brick-fallback";
  }
  return "unknown";
}

bool selectSparseVoxelTerrainGBufferRoute(
    const SparseVoxelTerrainVGScenePageInstall *sceneInstall,
    const SparseVoxelTerrainGBufferRouteSettings &settings,
    SparseVoxelTerrainGBufferRouteDecision &decision,
    std::string *error)
{
  decision = {};
  decision.request = settings.request;
  const bool vgReady = sceneInstallSatisfiesRoute(sceneInstall, settings, decision);

  if (settings.request == SparseVoxelTerrainGBufferRouteRequest::VirtualGeometry)
  {
    if (!vgReady)
      return fail(error, "terrain virtual-geometry G-buffer route was requested but the scene install or scene-visible raster payload is not ready");
    chooseVirtualGeometry(decision, "forced virtual-geometry terrain route", false);
    if (error != nullptr)
      error->clear();
    return true;
  }

  if (settings.request == SparseVoxelTerrainGBufferRouteRequest::BrickFallback)
  {
    if (!settings.brickFallbackAvailable)
      return fail(error, "terrain brick fallback route was requested but no brick fallback is available");
    chooseBrickFallback(decision, "forced brick terrain fallback route");
    if (error != nullptr)
      error->clear();
    return true;
  }

  if (vgReady)
  {
    chooseVirtualGeometry(decision, "default terrain route uses virtual geometry", true);
    if (error != nullptr)
      error->clear();
    return true;
  }

  if (settings.allowBrickFallback && settings.brickFallbackAvailable)
  {
    chooseBrickFallback(decision, "default terrain route fell back to brick raster because virtual geometry is not ready");
    if (error != nullptr)
      error->clear();
    return true;
  }

  return fail(error, "terrain G-buffer route has no ready virtual-geometry page and brick fallback is disabled");
}

} // namespace voxel::svraster
