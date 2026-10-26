#pragma once

#include <algorithm>
#include <cstdint>

namespace globalillumination::worldgi
{

enum GITraceSurfaceSource : uint32_t
{
  GITraceSurfaceSource_Invalid = 0u,
  GITraceSurfaceSource_PrimaryGBuffer = 1u,
  GITraceSurfaceSource_ScreenTrace = 2u,
  GITraceSurfaceSource_CWBVH = 3u,
  GITraceSurfaceSource_SDF = 4u,
  GITraceSurfaceSource_EmissiveSample = 5u,
  GITraceSurfaceSource_LightSample = 6u,
  GITraceSurfaceSource_SkyMiss = 7u,
};

enum GITraceSurfaceFlags : uint32_t
{
  GITraceSurfaceFlag_None = 0u,
  GITraceSurfaceFlag_Valid = 1u << 0u,
  GITraceSurfaceFlag_SurfaceCacheMaterial = 1u << 1u,
  GITraceSurfaceFlag_FallbackMaterial = 1u << 2u,
  GITraceSurfaceFlag_ScreenTraceMiss = 1u << 3u,
};

enum WorldGIRayType : uint32_t
{
  WorldGIRayType_Invalid = 0u,
  WorldGIRayType_Diffuse = 1u,
  WorldGIRayType_Glossy = 2u,
  WorldGIRayType_FinalGather = 3u,
};

enum WorldGIRayBackendMask : uint32_t
{
  WorldGIRayBackend_None = 0u,
  WorldGIRayBackend_ScreenTrace = 1u << 0u,
  WorldGIRayBackend_SDF = 1u << 1u,
  WorldGIRayBackend_CWBVH = 1u << 2u,
};

enum WorldGIRayFlags : uint32_t
{
  WorldGIRayFlag_None = 0u,
  WorldGIRayFlag_Valid = 1u << 0u,
};

enum WorldGILightType : uint32_t
{
  WorldGILightType_Invalid = 0u,
  WorldGILightType_Directional = 1u,
  WorldGILightType_Point = 2u,
  WorldGILightType_ReflectionProbe = 3u,
  WorldGILightType_Decal = 4u,
  WorldGILightType_EmissiveRange = 5u,
};

enum WorldGILightFlags : uint32_t
{
  WorldGILightFlag_None = 0u,
  WorldGILightFlag_Enabled = 1u << 0u,
};

static constexpr uint32_t kWorldGILightGridMaxCascades = 8u;
static constexpr uint32_t kWorldGILightGridMaxLights = 16u;
static constexpr uint32_t kWorldGILightGridDefaultResolution = 16u;
static constexpr uint32_t kWorldGILightGridDefaultMaxItemsPerCell = 64u;

struct alignas(16) WorldGIRayGPU
{
  float originMinT[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float directionMaxT[4]{0.0f, 1.0f, 0.0f, 0.0f};
  float receiverPositionPdf[4]{0.0f, 0.0f, 0.0f, 0.15915494f};
  float receiverNormalRoughness[4]{0.0f, 1.0f, 0.0f, 1.0f};
  float receiverBaseColorMetallic[4]{1.0f, 1.0f, 1.0f, 0.0f};
  float samplePdfMisWeight[4]{0.15915494f, 0.15915494f, 1.0f, 1.0f};
  uint32_t pixelSample[4]{0u, 0u, 0u, 0u};
  uint32_t typeBackendFlags[4]{WorldGIRayType_Invalid, WorldGIRayBackend_None, WorldGIRayFlag_None, 0u};
};

static_assert(sizeof(WorldGIRayGPU) == 128u, "WorldGIRayGPU must match WGSL layout");

inline uint64_t worldGIRayBufferSize(const uint32_t rayCount)
{
  return static_cast<uint64_t>(rayCount == 0u ? 1u : rayCount) * sizeof(WorldGIRayGPU);
}

struct alignas(16) GITraceSurfaceGPU
{
  float worldPositionHitT[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float geometryNormalConfidence[4]{0.0f, 1.0f, 0.0f, 0.0f};
  float shadingNormalRoughness[4]{0.0f, 1.0f, 0.0f, 1.0f};
  float viewDirectionMetallic[4]{0.0f, 0.0f, 1.0f, 0.0f};
  float rayDirectionPdf[4]{0.0f, 1.0f, 0.0f, 0.15915494f};
  float uvBarycentrics[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float baseColorOpacity[4]{1.0f, 1.0f, 1.0f, 1.0f};
  float emissiveRadiance[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float receiverPositionPdf[4]{0.0f, 0.0f, 0.0f, 0.15915494f};
  float receiverNormalValidity[4]{0.0f, 1.0f, 0.0f, 0.0f};
  float receiverAlbedo[4]{1.0f, 1.0f, 1.0f, 1.0f};
  uint32_t ids[4]{UINT32_MAX, UINT32_MAX, UINT32_MAX, 0u};
  uint32_t sourceFlags[4]{GITraceSurfaceSource_Invalid, 0u, 0u, 0u};
};

static_assert(sizeof(GITraceSurfaceGPU) == 208u, "GITraceSurfaceGPU must match WGSL layout");

inline uint64_t giTraceSurfaceBufferSize(const uint32_t surfaceCount)
{
  return static_cast<uint64_t>(surfaceCount == 0u ? 1u : surfaceCount) * sizeof(GITraceSurfaceGPU);
}

struct alignas(16) WorldGILightGPU
{
  float positionRange[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float directionType[4]{0.0f, -1.0f, 0.0f, 0.0f};
  float colorIntensity[4]{1.0f, 1.0f, 1.0f, 0.0f};
  uint32_t flags[4]{WorldGILightFlag_None, WorldGILightType_Invalid, 0u, 0u};
};

static_assert(sizeof(WorldGILightGPU) == 64u, "WorldGILightGPU must match WGSL layout");

struct alignas(16) WorldGILightGridCellGPU
{
  uint32_t itemCount[4]{0u, 0u, 0u, 0u};
};

static_assert(sizeof(WorldGILightGridCellGPU) == 16u, "WorldGILightGridCellGPU must match WGSL layout");

inline uint32_t worldGILightGridCellCount(uint32_t gridResolution, uint32_t cascadeCount)
{
  gridResolution = gridResolution == 0u ? 1u : gridResolution;
  cascadeCount = std::clamp(cascadeCount, 1u, kWorldGILightGridMaxCascades);
  return gridResolution * gridResolution * gridResolution * cascadeCount;
}

inline uint64_t worldGILightBufferSize(uint32_t lightCapacity)
{
  return static_cast<uint64_t>(lightCapacity == 0u ? 1u : lightCapacity) * sizeof(WorldGILightGPU);
}

inline uint64_t worldGILightGridCellBufferSize(uint32_t gridResolution, uint32_t cascadeCount)
{
  return static_cast<uint64_t>(worldGILightGridCellCount(gridResolution, cascadeCount)) * sizeof(WorldGILightGridCellGPU);
}

inline uint64_t worldGILightGridItemBufferSize(uint32_t gridResolution, uint32_t cascadeCount, uint32_t maxItemsPerCell)
{
  maxItemsPerCell = maxItemsPerCell == 0u ? 1u : maxItemsPerCell;
  return static_cast<uint64_t>(worldGILightGridCellCount(gridResolution, cascadeCount)) * static_cast<uint64_t>(maxItemsPerCell) * sizeof(uint32_t);
}

struct WorldGIRuntimeSettings
{
  bool enabled = false;
  bool diffuseEnabled = true;
  bool glossyEnabled = true;
  bool indirectEnabled = true;
  float diffuseWeight = 1.0f;
  float glossyWeight = 1.0f;
  float indirectIntensity = 1.0f;
  float glossyMaxRoughness = 0.65f;
  uint32_t rayResolutionDivisor = 4u;
  uint32_t raysPerPixel = 2u;
  uint32_t rayMaxSteps = 96u;
  float rayMinDistance = 0.02f;
  float rayMaxDistance = 64.0f;
  float rayNormalBias = 0.03f;
  bool lightGridEnabled = true;
  uint32_t lightGridResolution = kWorldGILightGridDefaultResolution;
  uint32_t lightGridCascadeCount = 6u;
  uint32_t lightGridMaxItemsPerCell = kWorldGILightGridDefaultMaxItemsPerCell;
  float lightGridBaseExtent = 2000.0f;
  float lightGridCascadeScale = 2.0f;
};

} // namespace globalillumination::worldgi
