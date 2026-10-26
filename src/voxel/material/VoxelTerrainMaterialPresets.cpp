#include "voxel/material/VoxelTerrainMaterialPresets.hpp"

#include "voxel/material/VoxelBuiltInShading.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace voxel::material
{
namespace
{

// Matches procedural::PolygonTerrainMaterial without coupling the runtime
// material module to terrain generation. The static assertion lives in the
// sample, where both modules are visible.
enum TerrainMaterialIndex : size_t
{
  Ocean, Lake, Beach, Grass, Rock, Snow, River, Marsh, Ice, Tundra, Bare,
  Scorched, Taiga, Shrubland, TemperateDesert, TemperateRainForest,
  TemperateDeciduousForest, Grassland, TropicalRainForest,
  TropicalSeasonalForest, SubtropicalDesert,
};

constexpr std::array<const char *, kDefaultTerrainVoxelMaterialCount> kNames{
  "Ocean", "Lake", "Beach", "Grass", "Rock", "Snow", "River",
  "Marsh", "Ice", "Tundra", "Bare", "Scorched", "Taiga",
  "Shrubland", "Temperate desert", "Temperate rain forest",
  "Temperate deciduous forest", "Grassland", "Tropical rain forest",
  "Tropical seasonal forest", "Subtropical desert",
};

} // namespace

const std::array<const char *, kDefaultTerrainVoxelMaterialCount> &
defaultTerrainVoxelMaterialNames()
{
  return kNames;
}

std::array<TerrainVoxelMaterialPreset, kDefaultTerrainVoxelMaterialCount>
defaultTerrainVoxelMaterialPresets(const bool volumePreset)
{
  std::array<TerrainVoxelMaterialPreset,
             kDefaultTerrainVoxelMaterialCount> materials{};
  const uint32_t staircaseFaces = svraster::kSparseVoxelStaircaseFaceMask;
  for (size_t index = 0u; index < materials.size(); ++index)
  {
    auto &material = materials[index];
    material.geometry.subdivision = 5u;
    material.geometry.maximumCarveDepth = 1u;
    // Terrain geometry always reconstructs the authored height field. Material
    // variation belongs to shading: random column carving becomes kilometre-
    // scale holes and pillars when the same preset is used by coarse world-map
    // pages.
    material.geometry.faceMask = staircaseFaces;
    material.geometry.geometryProfile =
        svraster::SparseVoxelMicroGeometryProfile::Staircase;
    material.geometry.materialSeed = static_cast<uint32_t>(0x40u + index);
    material.shading.profile =
        svraster::SparseVoxelMicroShadingProfile::NoisyLinear;
    material.shading.variationStrength = 24u;
    material.shading.lodHandoff = 128u;

    const bool water = index == Ocean || index == Lake || index == River;
    const bool vegetation =
        index == Grass || index == Marsh || index == Taiga ||
        index == Shrubland || index == TemperateRainForest ||
        index == TemperateDeciduousForest || index == Grassland ||
        index == TropicalRainForest || index == TropicalSeasonalForest;
    const bool hard =
        index == Rock || index == Snow || index == Ice || index == Tundra ||
        index == Bare || index == Scorched;
    const bool rockySurface =
        index == Rock || index == Bare || index == Scorched;
    if (water)
    {
      material.geometry.geometryProfile =
          svraster::SparseVoxelMicroGeometryProfile::Constant;
      // Water stays a flat parent surface. Carving a constant layer only
      // lowers the entire sheet and creates a visible seam against banks.
      material.geometry.maximumCarveDepth = 0u;
      material.shading.profile =
          svraster::SparseVoxelMicroShadingProfile::Linear;
      material.shading.variationStrength = 8u;
    }
    else if (vegetation)
    {
      material.geometry.faceMask = staircaseFaces;
      material.geometry.geometryProfile =
          svraster::SparseVoxelMicroGeometryProfile::Staircase;
      material.shading.profile =
          svraster::SparseVoxelMicroShadingProfile::ExposedSides;
      material.shading.variationStrength = 64u;
    }
    else if (hard)
    {
      material.shading.profile =
          svraster::SparseVoxelMicroShadingProfile::HardCrust;
      material.shading.parameter0 = 96u;
    }

    if (index == Beach)
    {
      material.programmableHueVariation = 0.10f;
      material.programmableBrightnessVariation = 0.06f;
    }
    if (index == Grass || index == Grassland)
    {
      material.programmableGrassSoil = true;
      material.graphPreset = VoxelMaterialGraphPreset::GrassOverSoil;
      material.programmableHueVariation = 0.08f;
      material.programmableBrightnessVariation = 0.04f;
    }
    if (volumePreset)
    {
      material.graphPreset = VoxelMaterialGraphPreset::LegacyCompatibility;
      material.programmableGrassSoil = false;
      material.geometry.maximumCarveDepth = 2u;
      material.geometry.faceMask = 0x3fu;
      material.geometry.geometryProfile =
          svraster::SparseVoxelMicroGeometryProfile::VolumeDensity;
      material.shading.profile =
          svraster::SparseVoxelMicroShadingProfile::FoliageTint;
      material.shading.variationStrength = 64u;
    }
    if (rockySurface)
    {
      // Geological surfaces use microcells only to reconstruct the authored
      // height-field contour. A flat parent remains completely full: there is
      // no hash-column or porous displacement. Rock, bare ground, and scorched
      // ground share this rule because the planet atlas commonly exposes the
      // latter two as the visible gray mountain surface.
      material.graphPreset = VoxelMaterialGraphPreset::LegacyCompatibility;
      material.programmableGrassSoil = false;
      material.geometry.subdivision = 5u;
      material.geometry.maximumCarveDepth = 1u;
      material.geometry.faceMask = staircaseFaces;
      material.geometry.geometryProfile =
          svraster::SparseVoxelMicroGeometryProfile::Staircase;
      material.shading.profile =
          svraster::SparseVoxelMicroShadingProfile::NoisyLinear;
      material.shading.variationStrength = 24u;
      material.programmableHueVariation = 0.035f;
      material.programmableBrightnessVariation = 0.075f;
    }
  }
  return materials;
}

VoxelMaterialAsset terrainVoxelMaterialAsset(
    const size_t materialIndex, const TerrainVoxelMaterialPreset &preset)
{
  VoxelMaterialAsset asset{};
  const size_t index = materialIndex < kNames.size() ? materialIndex : 0u;
  asset.assetId = "terrain/biome/" + std::to_string(materialIndex);
  asset.name = kNames[index];
  asset.geometry = preset.geometry;
  asset.legacyShading = preset.shading;
  asset.contextRequirements = {
    static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface) |
    static_cast<uint32_t>(VoxelMaterialContextField::MicroSlope) |
    static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope),
  };
  asset.slotPolicy = VoxelMaterialSlotPolicy::UniqueEditable;
  const bool useGrassSoilProgram =
      preset.graphPreset == VoxelMaterialGraphPreset::GrassOverSoil ||
      (preset.graphPreset == VoxelMaterialGraphPreset::LegacyCompatibility &&
       preset.programmableGrassSoil);
  asset.shadingProgram = useGrassSoilProgram
      ? grassOverDirtFieldVoxelShadingProgram(
            preset.programmableSoilDepthStart,
            preset.programmableSoilDepthEnd,
            preset.programmableSoilSlopeStartDegrees,
            preset.programmableSoilSlopeEndDegrees,
            preset.programmableTopRoughness,
            preset.programmableSoilRoughness,
            preset.programmableHueVariation,
            preset.programmableBrightnessVariation)
      : preset.shading.profile ==
                svraster::SparseVoxelMicroShadingProfile::NoisyLinear
          ? noisyLinearVoxelShadingProgram(
                preset.shading.variationStrength,
                preset.programmableHueVariation,
                preset.programmableBrightnessVariation)
          : builtInVoxelShadingProgram(preset.shading);
  if (useGrassSoilProgram)
  {
    asset.fieldBindings[0] = {
      VoxelMaterialFieldSemantic::Grass,
      VoxelMaterialFieldSource::PlanetRegion,
      VoxelMaterialFieldMapping::PlanetSurface,
      VoxelMaterialFieldFilter::Linear,
      VoxelMaterialFieldAddressMode::Clamp,
      1.0f, 1.0f, 0.0f, {}};
    asset.fieldBindings[1] = {
      VoxelMaterialFieldSemantic::Dirt,
      VoxelMaterialFieldSource::PlanetRegion,
      VoxelMaterialFieldMapping::PlanetSurface,
      VoxelMaterialFieldFilter::Linear,
      VoxelMaterialFieldAddressMode::Clamp,
      0.0f, 1.0f, 0.0f, {}};
    asset.fieldBindings[2] = {
      VoxelMaterialFieldSemantic::Variation,
      VoxelMaterialFieldSource::PlanetRegion,
      VoxelMaterialFieldMapping::PlanetSurface,
      VoxelMaterialFieldFilter::Linear,
      VoxelMaterialFieldAddressMode::Clamp,
      1.0f, 1.0f, 0.0f, {}};
  }
  switch (preset.graphPreset)
  {
  case VoxelMaterialGraphPreset::LegacyCompatibility:
  case VoxelMaterialGraphPreset::GrassOverSoil:
    break;
  case VoxelMaterialGraphPreset::SnowRetention:
    asset.geometryProgram = snowRetentionVoxelGeometryProgram();
    asset.geometryParameters.values[0] = voxelGeometryFixedFromDouble(
        preset.geometryParameters[0]);
    break;
  case VoxelMaterialGraphPreset::PorousRock:
    asset.geometryProgram = porousRockVoxelGeometryProgram();
    asset.geometryParameters.values[0] = voxelGeometryFixedFromDouble(
        preset.geometryParameters[0]);
    asset.geometryParameters.values[1] = voxelGeometryFixedFromDouble(
        preset.geometryParameters[1]);
    break;
  case VoxelMaterialGraphPreset::FoliageEnvelope:
    asset.geometryProgram = foliageEnvelopeVoxelGeometryProgram();
    asset.geometryParameters.values[0] = voxelGeometryFixedFromDouble(
        preset.geometryParameters[0]);
    break;
  }
  if ((preset.graphPreset == VoxelMaterialGraphPreset::LegacyCompatibility ||
       preset.graphPreset == VoxelMaterialGraphPreset::GrassOverSoil) &&
      preset.geometry.geometryProfile ==
      svraster::SparseVoxelMicroGeometryProfile::VolumeDensity)
    asset.geometryProgram = volumeDensityVoxelGeometryProgram(
        voxelGeometryFixedFromDouble(0.52));
  asset.contextRequirements.fields |=
      voxelGeometryRequiredContextFields(asset.geometryProgram);
  return asset;
}

VoxelGeometryProgram snowRetentionVoxelGeometryProgram()
{
  VoxelGeometryProgram result{};
  result.instructions = {
    {VoxelGeometryOpcode::LoadBaseSurface, 0u},
    {VoxelGeometryOpcode::LoadTerrainSlope, 1u},
    {VoxelGeometryOpcode::OneMinus, 1u, 1u},
    {VoxelGeometryOpcode::LoadParameter, 2u, 0u},
    {VoxelGeometryOpcode::Multiply, 1u, 1u, 2u},
    {VoxelGeometryOpcode::Add, 0u, 0u, 1u},
    {VoxelGeometryOpcode::Clamp01, 0u, 0u},
    {VoxelGeometryOpcode::OutputRetainedHeight, 0u, 0u},
  };
  return result;
}

VoxelGeometryProgram porousRockVoxelGeometryProgram()
{
  VoxelGeometryProgram result{};
  result.domain = VoxelGeometryDomain::Volume;
  result.executionHint = VoxelGeometryExecutionHint::Cached;
  result.instructions = {
    {VoxelGeometryOpcode::LoadWorldNoise, 0u, 0u, 0u, 0x4f1bbcdcu},
    {VoxelGeometryOpcode::LoadParameter, 1u, 0u},
    {VoxelGeometryOpcode::Subtract, 0u, 0u, 1u},
    {VoxelGeometryOpcode::Clamp01, 0u, 0u},
    {VoxelGeometryOpcode::LoadWorldNoise, 2u, 0u, 0u,
     static_cast<int32_t>(0x91e10da5u)},
    {VoxelGeometryOpcode::LoadParameter, 3u, 1u},
    {VoxelGeometryOpcode::Subtract, 2u, 2u, 3u},
    {VoxelGeometryOpcode::Clamp01, 2u, 2u},
    {VoxelGeometryOpcode::VolumeSubtract, 0u, 0u, 2u},
    {VoxelGeometryOpcode::Quantize, 0u, 0u, 0u,
     kVoxelGeometryFixedOne},
    {VoxelGeometryOpcode::OutputCoverage, 0u, 0u},
  };
  return result;
}

VoxelGeometryProgram foliageEnvelopeVoxelGeometryProgram()
{
  VoxelGeometryProgram result{};
  result.domain = VoxelGeometryDomain::Volume;
  result.executionHint = VoxelGeometryExecutionHint::Cached;
  result.instructions = {
    {VoxelGeometryOpcode::LoadWorldNoise, 0u, 0u, 0u, 0x243f6a88u},
    {VoxelGeometryOpcode::LoadParameter, 1u, 0u},
    {VoxelGeometryOpcode::Subtract, 0u, 0u, 1u},
    {VoxelGeometryOpcode::Clamp01, 0u, 0u},
    {VoxelGeometryOpcode::LoadTerrainBlend, 2u},
    {VoxelGeometryOpcode::VolumeIntersection, 0u, 0u, 2u},
    {VoxelGeometryOpcode::Quantize, 0u, 0u, 0u,
     kVoxelGeometryFixedOne},
    {VoxelGeometryOpcode::OutputCoverage, 0u, 0u},
  };
  return result;
}

} // namespace voxel::material
