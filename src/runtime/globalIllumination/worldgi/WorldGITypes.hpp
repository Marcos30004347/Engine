#pragma once

#include <algorithm>
#include <cstdint>

#include "math/math.hpp"

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
  WorldGILightType_Sphere = 6u,
  WorldGILightType_Planar = 7u,
  WorldGILightType_Sky = 8u,
};

enum WorldGILightFlags : uint32_t
{
  WorldGILightFlag_None = 0u,
  WorldGILightFlag_Enabled = 1u << 0u,
  // Sky payload stores four RGB first-order SH coefficients instead of the
  // legacy horizon/zenith/ground gradient.
  WorldGILightFlag_AtmosphereSH = 1u << 1u,
};

enum WorldGILightBVHNodeType : uint32_t
{
  WorldGILightBVHNodeType_Invalid = 0u,
  WorldGILightBVHNodeType_Leaf = 1u,
  WorldGILightBVHNodeType_Internal = 2u,
  WorldGILightBVHNodeType_Infinite = 3u,
};

static constexpr uint32_t kWorldGILightGridMaxCascades = 8u;
static constexpr uint32_t kWorldGILightGridMaxLights = 16u;
static constexpr uint32_t kWorldGILightGridDefaultResolution = 16u;
static constexpr uint32_t kWorldGILightGridDefaultMaxItemsPerCell = 64u;
static constexpr uint32_t kWorldGILightBVHInvalidNode = UINT32_MAX;
static constexpr uint32_t kWorldGIRadianceCacheDefaultCapacity = 1u << 19u;
static constexpr uint32_t kWorldGIRadianceCacheDefaultTTLFrames = 120u;
static constexpr uint32_t kWorldGIRadianceCacheMaxProbeCount = 16u;
static constexpr uint32_t kWorldGIShadowProbeOctahedralResolution = 8u;
static constexpr uint32_t kWorldGIShadowProbeOctahedralTexelCount = kWorldGIShadowProbeOctahedralResolution * kWorldGIShadowProbeOctahedralResolution;
static constexpr uint32_t kWorldGIShadowRaysPerProbe = 4u;
static constexpr uint32_t kWorldGIShadowReservoirSlotsPerProbe = kWorldGIShadowRaysPerProbe;
static constexpr uint32_t kWorldGIShadowTemporalSampleCount = 64u;

enum WorldGIRadianceCacheEntryFlags : uint32_t
{
  WorldGIRadianceCacheEntryFlag_Empty = 0u,
  WorldGIRadianceCacheEntryFlag_Valid = 1u << 0u,
  WorldGIRadianceCacheEntryFlag_Locked = 1u << 1u,
};

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
  // Legacy sky: positionRange=horizon, directionType=up,
  // shapeParams=ground/exponent, colorIntensity=zenith/intensity.
  // Atmosphere-SH sky: those four xyz vectors are respectively the L0, L1-X,
  // L1-Y, and L1-Z RGB coefficients; colorIntensity.w is the enable scale.
  float shapeParams[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float colorIntensity[4]{1.0f, 1.0f, 1.0f, 0.0f};
  // flags: enabled bits, type, stable light handle, light revision.
  uint32_t flags[4]{WorldGILightFlag_None, WorldGILightType_Invalid, 0u, 0u};
};

static_assert(sizeof(WorldGILightGPU) == 80u, "WorldGILightGPU must match WGSL layout");

// One authored finite emitter.
//
// Point, sphere and planar lights differ only in which shape parameters they
// read, so they share one authoring record and the type selects the
// interpretation. Keeping them in a list rather than as one singleton per type
// is what lets a scene hold several of any of them; the infinite lights
// (directional and sky) stay singletons because they genuinely are.
struct WorldGIFiniteLight
{
  uint32_t type = WorldGILightType_Point;
  bool enabled = true;
  math::Vec3f position = math::Vec3f(0.0f);
  math::Vec3f color = math::Vec3f(1.0f);
  float range = 16.0f;
  float intensity = 0.0f;
  // Sphere only.
  float radius = 0.5f;
  // Planar only: the emitting normal and the quad's half extents.
  math::Vec3f normal = math::Vec3f(0.0f, -1.0f, 0.0f);
  float halfWidth = 0.5f;
  float halfHeight = 0.5f;
  // Identity that survives reordering, and a revision that changes whenever any
  // property above does.
  //
  // Both are load bearing rather than diagnostic: they are what the per-
  // receiver light hash mixes, so pixels and probes re-light exactly when a
  // light they can see actually changed. Reusing a handle for a different light
  // hides a change; bumping a revision that did not change re-lights the screen
  // for nothing.
  uint32_t handle = 0u;
  uint32_t revision = 0u;
};

// Fill a GPU light record from an authored finite light. Shared so the BVH
// build and the light grid cannot drift apart on shape-parameter packing.
inline WorldGILightGPU worldGIMakeFiniteLight(const WorldGIFiniteLight &light)
{
  WorldGILightGPU result{};
  result.positionRange[0] = light.position.x();
  result.positionRange[1] = light.position.y();
  result.positionRange[2] = light.position.z();
  result.positionRange[3] = std::max(light.range, 0.0f);
  result.colorIntensity[0] = light.color.x();
  result.colorIntensity[1] = light.color.y();
  result.colorIntensity[2] = light.color.z();
  result.colorIntensity[3] = light.enabled ? std::max(light.intensity, 0.0f) : 0.0f;
  result.flags[0] = light.enabled ? WorldGILightFlag_Enabled : WorldGILightFlag_None;
  result.flags[1] = light.type;
  result.flags[2] = light.handle;
  result.flags[3] = light.revision;
  if (light.type == WorldGILightType_Sphere)
    result.shapeParams[0] = std::max(light.radius, 0.0f);
  else if (light.type == WorldGILightType_Planar)
  {
    result.directionType[0] = light.normal.x();
    result.directionType[1] = light.normal.y();
    result.directionType[2] = light.normal.z();
    result.shapeParams[1] = std::max(light.halfWidth, 0.0f);
    result.shapeParams[2] = std::max(light.halfHeight, 0.0f);
  }
  return result;
}

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

struct alignas(16) WorldGILightBVHNodeGPU
{
  float boundsMinNodeType[4]{0.0f, 0.0f, 0.0f, static_cast<float>(WorldGILightBVHNodeType_Invalid)};
  float boundsMaxImportance[4]{0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t childOrLightCount[4]{kWorldGILightBVHInvalidNode, 0u, UINT32_MAX, 0u};
};

static_assert(sizeof(WorldGILightBVHNodeGPU) == 48u, "WorldGILightBVHNodeGPU must match WGSL layout");

struct alignas(16) WorldGILightBVHInfoGPU
{
  uint32_t nodeCount = 0u;
  uint32_t rootNodeIndex = kWorldGILightBVHInvalidNode;
  uint32_t lightCount = 0u;
  uint32_t infiniteLightCount = 0u;
};

static_assert(sizeof(WorldGILightBVHInfoGPU) == 16u, "WorldGILightBVHInfoGPU must match WGSL layout");

enum WorldGIShadowProbeFlags : uint32_t
{
  WorldGIShadowProbeFlag_None = 0u,
  WorldGIShadowProbeFlag_Valid = 1u << 0u,
  WorldGIShadowProbeFlag_ActiveRay = 1u << 1u,
};

enum WorldGIShadowReservoirFlags : uint32_t
{
  WorldGIShadowReservoirFlag_None = 0u,
  WorldGIShadowReservoirFlag_Valid = 1u << 0u,
};

struct alignas(16) WorldGIShadowProbeGPU
{
  float worldPositionDepth[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float normalConfidence[4]{0.0f, 1.0f, 0.0f, 0.0f};
  uint32_t pixelLightFlags[4]{0u, 0u, UINT32_MAX, WorldGIShadowProbeFlag_None};
  float rayDirectionMaxT[4]{0.0f, 1.0f, 0.0f, 0.0f};
  float lightRadiancePdf[4]{0.0f, 0.0f, 0.0f, 1.0f};
  float visibilityVarianceHistory[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(WorldGIShadowProbeGPU) == 96u, "WorldGIShadowProbeGPU must match WGSL layout");

struct alignas(16) WorldGIShadowLightReservoirGPU
{
  uint32_t lightSampleFlags[4]{UINT32_MAX, 0u, 0u, WorldGIShadowReservoirFlag_None};
  float weightTargetConfidence[4]{0.0f, 0.0f, 1.0f, 0.0f};
  float radianceVisibility[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float worldPositionDepth[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float normalHistory[4]{0.0f, 1.0f, 0.0f, 0.0f};
  float sampleDirectionDistance[4]{0.0f, 1.0f, 0.0f, 0.0f};
};

static_assert(sizeof(WorldGIShadowLightReservoirGPU) == 96u, "WorldGIShadowLightReservoirGPU must match WGSL layout");

inline uint64_t worldGIShadowProbeBufferSize(const uint32_t probeCount)
{
  return static_cast<uint64_t>(probeCount == 0u ? 1u : probeCount) * sizeof(WorldGIShadowProbeGPU);
}

inline uint64_t worldGIShadowReservoirBufferSize(const uint32_t probeCount)
{
  return static_cast<uint64_t>(probeCount == 0u ? 1u : probeCount) * kWorldGIShadowReservoirSlotsPerProbe * sizeof(WorldGIShadowLightReservoirGPU);
}

inline uint64_t worldGIShadowProbeDirectionalBufferSize(const uint32_t probeCount)
{
  return static_cast<uint64_t>(probeCount == 0u ? 1u : probeCount) * kWorldGIShadowProbeOctahedralTexelCount * 4ull * sizeof(float);
}

inline uint32_t worldGILightBVHNodeCapacity(uint32_t lightCapacity)
{
  lightCapacity = lightCapacity == 0u ? 1u : lightCapacity;
  return lightCapacity * 2u - 1u;
}

inline uint64_t worldGILightBVHNodeBufferSize(uint32_t lightCapacity)
{
  return static_cast<uint64_t>(worldGILightBVHNodeCapacity(lightCapacity)) * sizeof(WorldGILightBVHNodeGPU);
}

inline uint64_t worldGILightBVHInfoBufferSize()
{
  return sizeof(WorldGILightBVHInfoGPU);
}

struct alignas(16) GIWorldRadianceCacheEntryGPU
{
  uint32_t key[4]{0u, 0u, 0u, WorldGIRadianceCacheEntryFlag_Empty};
  float worldPositionCellSize[4]{0.0f, 0.0f, 0.0f, 0.5f};
  float normalConfidence[4]{0.0f, 1.0f, 0.0f, 0.0f};
  float baseColorRoughness[4]{1.0f, 1.0f, 1.0f, 1.0f};
  float emissiveRadiance[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float directDiffuseRadiance[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float directSpecularRadiance[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float indirectDiffuseIrradianceSH0[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float glossyRadiance[4]{0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t metadata[4]{0u, UINT32_MAX, UINT32_MAX, 0u};
};

static_assert(sizeof(GIWorldRadianceCacheEntryGPU) == 160u, "GIWorldRadianceCacheEntryGPU must match WGSL layout");

struct alignas(16) GIWorldRadianceCacheActiveItemGPU
{
  uint32_t entrySurfaceProducerFlags[4]{UINT32_MAX, UINT32_MAX, 0u, 0u};
};

static_assert(sizeof(GIWorldRadianceCacheActiveItemGPU) == 16u, "GIWorldRadianceCacheActiveItemGPU must match WGSL layout");

inline uint64_t worldGIRadianceCacheEntryBufferSize(uint32_t entryCapacity)
{
  return static_cast<uint64_t>(entryCapacity == 0u ? 1u : entryCapacity) * sizeof(GIWorldRadianceCacheEntryGPU);
}

inline uint64_t worldGIRadianceCacheActiveListBufferSize(uint32_t activeCapacity)
{
  return static_cast<uint64_t>(activeCapacity == 0u ? 1u : activeCapacity) * sizeof(GIWorldRadianceCacheActiveItemGPU);
}

inline uint64_t worldGIRadianceCacheCounterBufferSize()
{
  return sizeof(uint32_t) * 4ull;
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
  bool lightBVHEnabled = true;
  bool lightGridEnabled = true;
  uint32_t lightGridResolution = kWorldGILightGridDefaultResolution;
  uint32_t lightGridCascadeCount = 6u;
  uint32_t lightGridMaxItemsPerCell = kWorldGILightGridDefaultMaxItemsPerCell;
  float lightGridBaseExtent = 2000.0f;
  float lightGridCascadeScale = 2.0f;
  bool radianceCacheEnabled = true;
  bool radianceCacheScreenHitsEnabled = false;
  uint32_t radianceCacheCapacity = kWorldGIRadianceCacheDefaultCapacity;
  uint32_t radianceCacheTTLFrames = kWorldGIRadianceCacheDefaultTTLFrames;
  float radianceCacheCellSize = 0.5f;
};

} // namespace globalillumination::worldgi
