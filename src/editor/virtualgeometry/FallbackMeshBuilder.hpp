#pragma once

#include <vector>

#include "VirtualGeometryBuilder.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"

namespace virtualgeometry
{

class FallbackMeshBuilder
{
public:
  struct Settings
  {
    float lod0TargetTriangleRatio = 0.35f;
    float lod0TargetError = 0.0035f;
    float lodErrorMultiplier = 1.5f;
    uint32_t maxLODCount = 1u;

    float proxyTargetTriangleRatio = 0.35f;
    float proxyTargetError = 1.5f;
    uint32_t proxyMinTriangleCount = 256u;
  };

  struct SurfaceBakeSettings
  {
    uint32_t fallbackTextureWidth = 1024u;
    uint32_t fallbackTextureHeight = 1024u;
    bool loadMaterialFiles = true;
    bool bakeVertexAttributes = true;
    bool bakeTriangleMipHints = true;
  };

  static FallbackMeshData buildOriginal(const VirtualGeometrySourceAsset &asset);
  static std::vector<FallbackMeshData> buildOriginalPerShape(const VirtualGeometrySourceAsset &asset);
  static FallbackMeshData buildSDFOriginal(const VirtualGeometrySourceAsset &asset);
  static FallbackMeshData buildSDFOriginal(
      const VirtualGeometrySourceAsset &asset,
      const SurfaceBakeSettings &surfaceSettings);
  static std::vector<FallbackMeshData> buildSDFOriginalPerShape(const VirtualGeometrySourceAsset &asset);
  static std::vector<FallbackMeshData> buildSDFOriginalPerShape(
      const VirtualGeometrySourceAsset &asset,
      const SurfaceBakeSettings &surfaceSettings);
  static FallbackMeshData buildProxy(const VirtualGeometrySourceAsset &asset);
  static FallbackMeshData buildProxy(const VirtualGeometrySourceAsset &asset, const Settings &settings);
  static FallbackMeshData buildSDFProxy(const VirtualGeometrySourceAsset &asset);
  static FallbackMeshData buildSDFProxy(
      const VirtualGeometrySourceAsset &asset,
      const Settings &settings);
  static FallbackMeshData buildSDFProxy(
      const VirtualGeometrySourceAsset &asset,
      const Settings &settings,
      const SurfaceBakeSettings &surfaceSettings);
  static std::vector<FallbackMeshData> buildProxyPerShape(const VirtualGeometrySourceAsset &asset);
  static std::vector<FallbackMeshData> buildProxyPerShape(const VirtualGeometrySourceAsset &asset, const Settings &settings);
  static std::vector<FallbackMeshData> buildSDFProxyPerShape(const VirtualGeometrySourceAsset &asset);
  static std::vector<FallbackMeshData> buildSDFProxyPerShape(
      const VirtualGeometrySourceAsset &asset,
      const Settings &settings);
  static std::vector<FallbackMeshData> buildSDFProxyPerShape(
      const VirtualGeometrySourceAsset &asset,
      const Settings &settings,
      const SurfaceBakeSettings &surfaceSettings);

  static std::vector<FallbackMeshData> buildLODs(const VirtualGeometrySourceAsset &asset);
  static std::vector<FallbackMeshData> buildLODs(
      const VirtualGeometrySourceAsset &asset,
      const Settings &settings);
};

} // namespace virtualgeometry
