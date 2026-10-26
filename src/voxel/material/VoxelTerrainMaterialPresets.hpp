#pragma once

#include "voxel/material/VoxelMaterialAsset.hpp"

#include <array>
#include <cstddef>

namespace voxel::material
{

inline constexpr size_t kDefaultTerrainVoxelMaterialCount = 21u;

enum class VoxelMaterialGraphPreset : uint32_t
{
  LegacyCompatibility = 0u,
  GrassOverSoil = 1u,
  SnowRetention = 2u,
  PorousRock = 3u,
  FoliageEnvelope = 4u,
};

struct TerrainVoxelMaterialPreset
{
  bool enabled = true;
  VoxelMaterialGraphPreset graphPreset =
      VoxelMaterialGraphPreset::LegacyCompatibility;
  svraster::SparseVoxelMicroDescriptor geometry{};
  svraster::SparseVoxelMicroShadingDescriptor shading{};
  bool programmableGrassSoil = false;
  float programmableSoilDepthStart = 0.15f;
  float programmableSoilDepthEnd = 0.55f;
  float programmableSoilSlopeStartDegrees = 18.0f;
  float programmableSoilSlopeEndDegrees = 42.0f;
  float programmableTopRoughness = 0.78f;
  float programmableSoilRoughness = 0.94f;
  float programmableHueVariation = 0.0f;
  float programmableBrightnessVariation = 0.0f;
  std::array<float, 4> geometryParameters{0.18f, 0.78f, 0.0f, 0.0f};
};

const std::array<const char *, kDefaultTerrainVoxelMaterialCount> &
defaultTerrainVoxelMaterialNames();

std::array<TerrainVoxelMaterialPreset, kDefaultTerrainVoxelMaterialCount>
defaultTerrainVoxelMaterialPresets(bool volumePreset = false);

VoxelMaterialAsset terrainVoxelMaterialAsset(
    size_t materialIndex, const TerrainVoxelMaterialPreset &preset);

VoxelGeometryProgram snowRetentionVoxelGeometryProgram();
VoxelGeometryProgram porousRockVoxelGeometryProgram();
VoxelGeometryProgram foliageEnvelopeVoxelGeometryProgram();

} // namespace voxel::material
