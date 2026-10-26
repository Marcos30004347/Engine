#pragma once

#include "voxel/svraster/SparseVoxelTerrainVirtualGeometrySceneBridge.hpp"

#include <cstdint>
#include <string>

namespace voxel::svraster
{

enum class SparseVoxelTerrainGBufferRouteRequest : uint32_t
{
  Auto = 0u,
  VirtualGeometry,
  BrickFallback,
};

enum class SparseVoxelTerrainGBufferRoute : uint32_t
{
  None = 0u,
  VirtualGeometry,
  BrickFallback,
};

struct SparseVoxelTerrainGBufferRouteSettings
{
  SparseVoxelTerrainGBufferRouteRequest request =
      SparseVoxelTerrainGBufferRouteRequest::Auto;
  bool allowBrickFallback = true;
  bool brickFallbackAvailable = true;
  bool requireTracePayload = true;
  bool requireMaterialPayload = false;
  bool requireSceneVisibleRasterPayload = true;
  bool sceneVisibleRasterPayloadReady = false;
};

struct SparseVoxelTerrainGBufferRouteDecision
{
  SparseVoxelTerrainGBufferRoute route = SparseVoxelTerrainGBufferRoute::None;
  SparseVoxelTerrainGBufferRouteRequest request =
      SparseVoxelTerrainGBufferRouteRequest::Auto;
  bool usedDefaultVirtualGeometry = false;
  bool usedBrickFallback = false;
  bool usesVirtualGeometryRenderer = false;
  bool usesGPUVirtualGeometryCulling = false;
  bool usesCPUTerrainCulling = false;
  bool traceParityRequired = false;
  bool materialParityRequired = false;
  bool sceneVisibleRasterPayloadRequired = false;
  bool traceParitySatisfied = false;
  bool materialParitySatisfied = false;
  bool sceneVisibleRasterPayloadReady = false;
  bool terrainVGSceneInstallReady = false;
  std::string reason{};
};

const char *sparseVoxelTerrainGBufferRouteName(SparseVoxelTerrainGBufferRoute route);

bool selectSparseVoxelTerrainGBufferRoute(
    const SparseVoxelTerrainVGScenePageInstall *sceneInstall,
    const SparseVoxelTerrainGBufferRouteSettings &settings,
    SparseVoxelTerrainGBufferRouteDecision &decision,
    std::string *error = nullptr);

} // namespace voxel::svraster
