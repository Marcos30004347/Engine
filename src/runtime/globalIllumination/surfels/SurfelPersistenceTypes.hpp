#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace globalillumination
{
namespace surfels
{

static constexpr uint32_t kSurfelPrefixThreadGroupSize = 512u;
static constexpr uint32_t kSurfelPrefixSegmentSize = kSurfelPrefixThreadGroupSize * 2u;
static constexpr uint32_t kSurfelLookupRegionCount = 7u;

constexpr uint32_t surfelLookupCellsPerRegion(uint32_t resolution)
{
  const uint32_t clampedResolution = resolution == 0u ? 1u : resolution;
  return clampedResolution * clampedResolution * clampedResolution;
}

constexpr uint32_t surfelLookupTotalCellCount(uint32_t resolution)
{
  return kSurfelLookupRegionCount * surfelLookupCellsPerRegion(resolution);
}

enum class SurfelAttachmentKind : uint32_t
{
  None = 0u,
  StaticMesh = 1u,
  SkinnedMesh = 2u,
};

struct SurfelAttachmentGPU
{
  uint32_t instanceId = 0u;
  uint32_t triangleId = 0u;
  uint32_t attachmentKind = static_cast<uint32_t>(SurfelAttachmentKind::None);
  uint32_t flags = 0u;
  float localPosition[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float localNormal[4]{0.0f, 1.0f, 0.0f, 0.0f};
};

static constexpr uint32_t kSurfelAttachmentBoneBits = 16u;
static constexpr uint32_t kSurfelAttachmentBoneMask = (1u << kSurfelAttachmentBoneBits) - 1u;
static constexpr uint32_t kSurfelAttachmentInvalidBone = kSurfelAttachmentBoneMask;

inline uint32_t packSurfelAttachmentFlags(uint32_t boneIndex, uint32_t extraFlags = 0u)
{
  return (extraFlags & ~kSurfelAttachmentBoneMask) | (boneIndex & kSurfelAttachmentBoneMask);
}

inline uint32_t unpackSurfelAttachmentBoneIndex(uint32_t flags)
{
  return flags & kSurfelAttachmentBoneMask;
}

struct SurfelLifecycleGPU
{
  uint32_t ageFrames = 0u;
  uint32_t lastTouchedFrame = 0u;
  float lastCoverage = 0.0f;
  uint32_t flags = 0u;
};

struct SurfelMaterialGPU
{
  float baseColorMetallic[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float shadingNormalRoughness[4]{0.0f, 1.0f, 0.0f, 1.0f};
};

struct SurfelIrradianceGPU
{
  float irradianceWeight[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

struct SurfelReservoirGPU
{
  float sampleRadianceWeight[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float sampleDirectionPdf[4]{0.0f, 0.0f, 1.0f, 0.0f};
  float weightSumCount[4]{0.0f, 0.0f, 0.0f, 0.0f};
};

enum class SurfelOwnerSelectionMode : uint32_t
{
  Oldest = 0u,
  Newest = 1u,
};

static constexpr uint32_t kSurfelLifecycleLevelBits = 5u;
static constexpr uint32_t kSurfelLifecycleLevelMask = (1u << kSurfelLifecycleLevelBits) - 1u;

inline uint32_t packSurfelLifecycleFlags(uint32_t level, uint32_t extraFlags = 0u)
{
  return (extraFlags & ~kSurfelLifecycleLevelMask) | (level & kSurfelLifecycleLevelMask);
}

inline uint32_t unpackSurfelLifecycleLevel(uint32_t flags)
{
  return flags & kSurfelLifecycleLevelMask;
}

struct SurfelRuntimeSettings
{
  uint32_t surfelCapacity = 1u << 18u;
  uint32_t allocationBudgetPerFrame = 1024u;
  uint32_t spawnDownsample = 2u;
  uint32_t spawnTileSize = 16u;
  uint32_t targetSurfelsPerTile = 1u;
  uint32_t maxSurfelsPerTile = 3u;
  uint32_t lookupCellResolution = 16u;
  float centralGridHalfExtent = 0.2f;
  float gridScale = 10.0f;
  float frustumFarDistance = 4096.0f;
  uint32_t gridCapacity = surfelLookupTotalCellCount(16u);
  uint32_t listEntriesPerSurfel = 16u;
  float targetScreenRadiusPixels = 6.0f;
  float lookupRadiusScale = 1.0f;
  float recycleCoverageThreshold = 0.25f;
  float recycleFreeListPressureBias = 0.5f;
  uint32_t visibleRecycleMaxAgeFrames = 120u;
  uint32_t hiddenRecycleMaxAgeFrames = 360u;
  uint32_t recycleMaxAgeFrames = 240u;
  uint32_t visibleUntouchedAgeStep = 8u;
  uint32_t hiddenUntouchedAgeStep = 2u;
  uint32_t denseSurfelsAgeStep = 24u;
  float hiddenDistanceThresholdScale = 1.0f;
  float hiddenDensityThresholdScale = 1.0f;
  float hiddenDensityAgeScale = 1.0f;
  uint32_t lightingUpdatePeriodFrames = 8u;
  float lightingHysteresis = 0.9f;
  float lightingIrradianceDecay = 0.995f;
  float lightingEnergyPreservation = 0.95f;
  uint32_t lightingTraceMaxSteps = 64u;
  float lightingTraceNormalBias = 0.02f;
  float lightingTraceViewBias = 0.05f;
  float lightingTraceHitThresholdScale = 0.75f;
  float lightingTraceEmptySpaceStepScale = 1.0f;
  float lightingMaxTraceDistance = 3000.0f;
  int32_t lightingTraceCascadeOverride = -1;
  float lightingDefaultDiffuseAlbedo = 0.35f;
  float localSpaceFollowMaxDistance = 0.10f;
  float spawnMinRadius = 0.05f;
  float spawnDepthRadiusScale = 0.01f;
  float radiusUpdateRate = 0.25f;
  float recycleMinRadiusScale = 0.5f;
  float recycleMaxRadiusScale = 2.0f;
  float undersizedAgeBias = 0.5f;
  float oversizedAgeBias = 1.5f;
  uint32_t ownerSelectionMode = static_cast<uint32_t>(SurfelOwnerSelectionMode::Oldest);
};

inline uint32_t surfelGridEntryCount(const SurfelRuntimeSettings &settings)
{
  return 1u + std::max(1u, settings.gridCapacity);
}

inline uint32_t surfelListCapacity(const SurfelRuntimeSettings &settings)
{
  const uint64_t surfelCount = static_cast<uint64_t>(std::max(1u, settings.surfelCapacity));
  const uint64_t entriesPerSurfel = static_cast<uint64_t>(std::max(1u, settings.listEntriesPerSurfel));
  return static_cast<uint32_t>(std::min<uint64_t>(surfelCount * entriesPerSurfel, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
}

inline uint64_t surfelStackBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(uint32_t) * static_cast<uint64_t>(1u + std::max(1u, settings.surfelCapacity));
}

inline uint64_t surfelPositionRadiusBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(float) * 4ull * static_cast<uint64_t>(std::max(1u, settings.surfelCapacity));
}

inline uint64_t surfelNormalLifecycleBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(float) * 4ull * static_cast<uint64_t>(std::max(1u, settings.surfelCapacity));
}

inline uint64_t surfelAttachmentBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(SurfelAttachmentGPU) * static_cast<uint64_t>(std::max(1u, settings.surfelCapacity));
}

inline uint64_t surfelLifecycleBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(SurfelLifecycleGPU) * static_cast<uint64_t>(std::max(1u, settings.surfelCapacity));
}

inline uint64_t surfelMaterialBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(SurfelMaterialGPU) * static_cast<uint64_t>(std::max(1u, settings.surfelCapacity));
}

inline uint64_t surfelIrradianceBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(SurfelIrradianceGPU) * static_cast<uint64_t>(std::max(1u, settings.surfelCapacity));
}

inline uint64_t surfelReservoirBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(SurfelReservoirGPU) * static_cast<uint64_t>(std::max(1u, settings.surfelCapacity));
}

inline uint64_t surfelRecycleClaimBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(uint32_t) * static_cast<uint64_t>(std::max(1u, settings.surfelCapacity));
}

inline uint64_t surfelGridBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(uint32_t) * static_cast<uint64_t>(surfelGridEntryCount(settings));
}

inline uint64_t surfelGridWriteCursorBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(uint32_t) * static_cast<uint64_t>(surfelGridEntryCount(settings));
}

inline uint64_t surfelOwnerCellBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(uint32_t) * static_cast<uint64_t>(surfelGridEntryCount(settings));
}

inline uint64_t surfelListBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(uint32_t) * static_cast<uint64_t>(surfelListCapacity(settings));
}

inline uint32_t surfelPrefixSegmentCount(const SurfelRuntimeSettings &settings)
{
  const uint32_t gridEntries = surfelGridEntryCount(settings);
  return std::max(1u, (gridEntries + kSurfelPrefixSegmentSize - 1u) / kSurfelPrefixSegmentSize);
}

inline uint64_t surfelPrefixSegmentsBufferSize(const SurfelRuntimeSettings &settings)
{
  return sizeof(uint32_t) * static_cast<uint64_t>(surfelPrefixSegmentCount(settings));
}

inline uint64_t surfelPersistentBufferBytes(const SurfelRuntimeSettings &settings)
{
  return surfelStackBufferSize(settings) +
         surfelPositionRadiusBufferSize(settings) +
         surfelNormalLifecycleBufferSize(settings) +
         surfelAttachmentBufferSize(settings) +
         surfelLifecycleBufferSize(settings) +
         surfelMaterialBufferSize(settings) +
         surfelIrradianceBufferSize(settings) +
         surfelReservoirBufferSize(settings) +
         surfelRecycleClaimBufferSize(settings) +
         surfelGridBufferSize(settings) +
         surfelGridWriteCursorBufferSize(settings) +
         surfelOwnerCellBufferSize(settings) +
         surfelListBufferSize(settings) +
         surfelPrefixSegmentsBufferSize(settings);
}

static_assert(sizeof(SurfelAttachmentGPU) % 16u == 0u);
static_assert(sizeof(SurfelLifecycleGPU) % 16u == 0u);
static_assert(sizeof(SurfelMaterialGPU) % 16u == 0u);
static_assert(sizeof(SurfelIrradianceGPU) % 16u == 0u);
static_assert(sizeof(SurfelReservoirGPU) % 16u == 0u);

} // namespace surfels
} // namespace globalillumination
