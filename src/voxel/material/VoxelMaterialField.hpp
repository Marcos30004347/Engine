#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace voxel::material
{

// Material programs address a fixed set of logical scalar fields. Texture
// residency and atlas addressing resolve into these slots outside the IR.
inline constexpr uint32_t kVoxelMaterialFieldCount = 4u;

enum class VoxelMaterialFieldSemantic : uint32_t
{
  Unused = 0u,
  Grass = 1u,
  Snow = 2u,
  Dirt = 3u,
  Variation = 4u,
  Carve = 5u,
  Moisture = 6u,
  Erosion = 7u,
};

enum class VoxelMaterialFieldSource : uint32_t
{
  Constant = 0u,
  MaterialTexture = 1u,
  PlanetRegion = 2u,
};

enum class VoxelMaterialFieldMapping : uint32_t
{
  CanonicalWorld = 0u,
  PlanetSurface = 1u,
  ParentLocal = 2u,
};

enum class VoxelMaterialFieldFilter : uint32_t
{
  Nearest = 0u,
  Linear = 1u,
};

enum class VoxelMaterialFieldAddressMode : uint32_t
{
  Clamp = 0u,
  Repeat = 1u,
};

struct VoxelMaterialFieldBinding
{
  VoxelMaterialFieldSemantic semantic = VoxelMaterialFieldSemantic::Unused;
  VoxelMaterialFieldSource source = VoxelMaterialFieldSource::Constant;
  VoxelMaterialFieldMapping mapping =
      VoxelMaterialFieldMapping::CanonicalWorld;
  VoxelMaterialFieldFilter filter = VoxelMaterialFieldFilter::Nearest;
  VoxelMaterialFieldAddressMode addressMode =
      VoxelMaterialFieldAddressMode::Clamp;
  // Missing or non-resident sources resolve to this normalized value.
  float defaultValue = 0.0f;
  // Applied by the resolver before the normalized value enters the IR.
  float scale = 1.0f;
  float bias = 0.0f;
  // Logical asset identity only. A later residency phase maps this identity to
  // a shared atlas layer rather than binding one descriptor per material.
  std::string textureAssetId;

  bool operator==(const VoxelMaterialFieldBinding &other) const;
  bool operator<(const VoxelMaterialFieldBinding &other) const;
};

using VoxelMaterialFieldBindings =
    std::array<VoxelMaterialFieldBinding, kVoxelMaterialFieldCount>;

// One canonical 16-byte upload per stable material slot. Values are unsigned
// Q16.16 in [0, 1], allowing Geometry IR to consume them without conversion;
// Shading IR converts the same bits to normalized float.
struct alignas(16) VoxelMaterialFieldDefaultsGPU
{
  std::array<uint32_t, kVoxelMaterialFieldCount> values{};

  bool operator==(const VoxelMaterialFieldDefaultsGPU &other) const
  {
    return values == other.values;
  }
};
static_assert(sizeof(VoxelMaterialFieldDefaultsGPU) == 16u);

VoxelMaterialFieldDefaultsGPU compileVoxelMaterialFieldDefaults(
    const VoxelMaterialFieldBindings &bindings);
std::array<int32_t, kVoxelMaterialFieldCount>
voxelMaterialFieldDefaultsFixed(
    const VoxelMaterialFieldDefaultsGPU &defaults);
std::array<float, kVoxelMaterialFieldCount>
voxelMaterialFieldDefaultsFloat(
    const VoxelMaterialFieldDefaultsGPU &defaults);

bool validateVoxelMaterialFieldBindings(
    const VoxelMaterialFieldBindings &bindings,
    std::string *error = nullptr);

} // namespace voxel::material
