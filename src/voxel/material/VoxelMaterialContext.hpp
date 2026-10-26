#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::material
{

enum class VoxelMaterialContextField : uint32_t
{
  BaseSurface = 1u << 0u,
  MicroSlope = 1u << 1u,
  TerrainSlope = 1u << 2u,
  DepthBelowSurface = 1u << 3u,
  TerrainBlend = 1u << 4u,
  Auxiliary = 1u << 5u,
};

inline constexpr uint32_t kVoxelMaterialKnownContextFields =
    static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface) |
    static_cast<uint32_t>(VoxelMaterialContextField::MicroSlope) |
    static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope) |
    static_cast<uint32_t>(VoxelMaterialContextField::DepthBelowSurface) |
    static_cast<uint32_t>(VoxelMaterialContextField::TerrainBlend) |
    static_cast<uint32_t>(VoxelMaterialContextField::Auxiliary);

inline constexpr uint32_t kVoxelMaterialAuxiliaryContextFields =
    static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope) |
    static_cast<uint32_t>(VoxelMaterialContextField::DepthBelowSurface) |
    static_cast<uint32_t>(VoxelMaterialContextField::TerrainBlend) |
    static_cast<uint32_t>(VoxelMaterialContextField::Auxiliary);

struct VoxelMaterialContextRequirements
{
  uint32_t fields = 0u;

  bool contains(const VoxelMaterialContextField field) const
  {
    return (fields & static_cast<uint32_t>(field)) != 0u;
  }

  bool operator==(const VoxelMaterialContextRequirements &other) const
  {
    return fields == other.fields;
  }

  bool operator!=(const VoxelMaterialContextRequirements &other) const
  {
    return !(*this == other);
  }
};

// Page-local optional context. Index zero is the canonical all-zero default;
// pages without optional requirements carry neither indices nor pool storage.
struct VoxelAuxContext
{
  uint8_t terrainSlope = 0u;
  uint8_t depthBelowSurface = 0u;
  uint8_t terrainBlend = 0u;
  uint8_t auxiliary = 0u;

  bool operator==(const VoxelAuxContext &other) const
  {
    return terrainSlope == other.terrainSlope &&
        depthBelowSurface == other.depthBelowSurface &&
        terrainBlend == other.terrainBlend && auxiliary == other.auxiliary;
  }
};

inline constexpr uint32_t packVoxelAuxContext(const VoxelAuxContext value)
{
  return static_cast<uint32_t>(value.terrainSlope) |
      (static_cast<uint32_t>(value.depthBelowSurface) << 8u) |
      (static_cast<uint32_t>(value.terrainBlend) << 16u) |
      (static_cast<uint32_t>(value.auxiliary) << 24u);
}

inline constexpr VoxelAuxContext unpackVoxelAuxContext(const uint32_t value)
{
  return {
    static_cast<uint8_t>(value),
    static_cast<uint8_t>(value >> 8u),
    static_cast<uint8_t>(value >> 16u),
    static_cast<uint8_t>(value >> 24u),
  };
}

struct VoxelAuxContextMemory
{
  uint64_t voxelCount = 0u;
  uint64_t uniqueContextCount = 0u;
  uint64_t pooledBytes = 0u;
  uint64_t directBytes = 0u;

  bool withinDirectBudget() const { return pooledBytes <= directBytes; }
};

class VoxelAuxContextPool
{
public:
  // Enables the optional representation and reserves one index per occupied
  // voxel. Leaving a pool disabled has exactly zero logical memory cost.
  void begin(uint32_t voxelCount);
  bool append(const VoxelAuxContext &context, std::string *error = nullptr);
  VoxelAuxContext context(uint32_t voxelIndex) const;
  VoxelAuxContextMemory memory() const;
  bool enabled() const { return !indices_.empty(); }
  const std::vector<uint16_t> &indices() const { return indices_; }
  const std::vector<uint32_t> &values() const { return values_; }

private:
  uint32_t expectedVoxelCount_ = 0u;
  std::vector<uint16_t> indices_;
  std::vector<uint32_t> values_;
};

inline constexpr bool voxelContextFieldsRequireAuxiliaryPool(
    const uint32_t fields)
{
  return (fields & kVoxelMaterialAuxiliaryContextFields) != 0u;
}

struct VoxelPageContextState
{
  uint64_t pageId = 0u;
  uint32_t bakedFields = 0u;
  std::vector<uint16_t> materialHandles;
};

// Only newly requested, unavailable fields force authoritative page rebuilds.
// Dropped fields and geometry edits with the same layout keep the page valid.
bool voxelPageRequiresContextRebuild(
    const VoxelPageContextState &page, uint16_t changedMaterial,
    uint32_t previousFields, uint32_t nextFields);
std::vector<uint64_t> voxelPagesRequiringContextRebuild(
    const std::vector<VoxelPageContextState> &pages,
    uint16_t changedMaterial, uint32_t previousFields, uint32_t nextFields);
std::string voxelMaterialContextFieldNames(uint32_t fields);

// Optional per-hit context companion. The common surface classification stays
// in the resolved surface's spare byte; this word is allocated only by passes
// that request hit-dependent depth/blend/auxiliary inputs.
inline constexpr uint32_t kVoxelResolvedContextDepthShift = 0u;
inline constexpr uint32_t kVoxelResolvedContextTerrainBlendShift = 8u;
inline constexpr uint32_t kVoxelResolvedContextAuxiliaryShift = 16u;
inline constexpr uint32_t kVoxelResolvedContextFlagsShift = 24u;
inline constexpr uint32_t kVoxelResolvedContextChannelMask = 0xffu;

enum class VoxelResolvedContextFlag : uint32_t
{
  Valid = 1u << 0u,
  Exact = 1u << 1u,
  Fallback = 1u << 2u,
  SideOrRiser = 1u << 3u,
};

inline uint32_t voxelContextUNorm8(const float value)
{
  if (!std::isfinite(value))
    return 0u;
  return static_cast<uint32_t>(std::lround(
      std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

inline VoxelAuxContext makeVoxelAuxContext(
    const float terrainSlope, const float depthBelowSurface,
    const float terrainBlend, const float auxiliary)
{
  return {
    static_cast<uint8_t>(voxelContextUNorm8(terrainSlope)),
    static_cast<uint8_t>(voxelContextUNorm8(depthBelowSurface)),
    static_cast<uint8_t>(voxelContextUNorm8(terrainBlend)),
    static_cast<uint8_t>(voxelContextUNorm8(auxiliary)),
  };
}

inline uint32_t packVoxelResolvedShadingContext(
    const float normalizedDepth, const float terrainBlend,
    const float auxiliary, const uint32_t flags)
{
  return voxelContextUNorm8(normalizedDepth) |
      (voxelContextUNorm8(terrainBlend) <<
       kVoxelResolvedContextTerrainBlendShift) |
      (voxelContextUNorm8(auxiliary) <<
       kVoxelResolvedContextAuxiliaryShift) |
      ((flags & kVoxelResolvedContextChannelMask) <<
       kVoxelResolvedContextFlagsShift);
}

inline float unpackVoxelResolvedContextChannel(
    const uint32_t packed, const uint32_t shift)
{
  return static_cast<float>(
      (packed >> shift) & kVoxelResolvedContextChannelMask) / 255.0f;
}

inline uint32_t unpackVoxelResolvedContextFlags(const uint32_t packed)
{
  return packed >> kVoxelResolvedContextFlagsShift;
}

} // namespace voxel::material
