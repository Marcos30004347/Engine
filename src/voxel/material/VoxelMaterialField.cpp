#include "voxel/material/VoxelMaterialField.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace voxel::material
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

} // namespace

bool VoxelMaterialFieldBinding::operator==(
    const VoxelMaterialFieldBinding &other) const
{
  return semantic == other.semantic && source == other.source &&
      mapping == other.mapping && filter == other.filter &&
      addressMode == other.addressMode &&
      defaultValue == other.defaultValue && scale == other.scale &&
      bias == other.bias && textureAssetId == other.textureAssetId;
}

bool VoxelMaterialFieldBinding::operator<(
    const VoxelMaterialFieldBinding &other) const
{
  return std::tie(
             semantic, source, mapping, filter, addressMode, defaultValue,
             scale, bias, textureAssetId) <
      std::tie(
             other.semantic, other.source, other.mapping, other.filter,
             other.addressMode, other.defaultValue, other.scale, other.bias,
             other.textureAssetId);
}

VoxelMaterialFieldDefaultsGPU compileVoxelMaterialFieldDefaults(
    const VoxelMaterialFieldBindings &bindings)
{
  VoxelMaterialFieldDefaultsGPU result{};
  for (size_t slot = 0u; slot < bindings.size(); ++slot)
    result.values[slot] = static_cast<uint32_t>(std::lround(
        std::clamp(bindings[slot].defaultValue, 0.0f, 1.0f) * 65536.0f));
  return result;
}

std::array<int32_t, kVoxelMaterialFieldCount>
voxelMaterialFieldDefaultsFixed(
    const VoxelMaterialFieldDefaultsGPU &defaults)
{
  std::array<int32_t, kVoxelMaterialFieldCount> result{};
  for (size_t slot = 0u; slot < result.size(); ++slot)
    result[slot] = static_cast<int32_t>(
        std::min(defaults.values[slot], 65536u));
  return result;
}

std::array<float, kVoxelMaterialFieldCount>
voxelMaterialFieldDefaultsFloat(
    const VoxelMaterialFieldDefaultsGPU &defaults)
{
  std::array<float, kVoxelMaterialFieldCount> result{};
  for (size_t slot = 0u; slot < result.size(); ++slot)
    result[slot] = static_cast<float>(
        std::min(defaults.values[slot], 65536u)) / 65536.0f;
  return result;
}

bool validateVoxelMaterialFieldBindings(
    const VoxelMaterialFieldBindings &bindings, std::string *error)
{
  if (error != nullptr)
    error->clear();
  for (size_t index = 0u; index < bindings.size(); ++index)
  {
    const VoxelMaterialFieldBinding &binding = bindings[index];
    const uint32_t semantic = static_cast<uint32_t>(binding.semantic);
    if (semantic > static_cast<uint32_t>(VoxelMaterialFieldSemantic::Erosion))
      return fail(error, "Voxel material field " + std::to_string(index) +
          " has an invalid semantic");
    if (static_cast<uint32_t>(binding.source) >
        static_cast<uint32_t>(VoxelMaterialFieldSource::PlanetRegion))
      return fail(error, "Voxel material field " + std::to_string(index) +
          " has an invalid source");
    if (static_cast<uint32_t>(binding.mapping) >
        static_cast<uint32_t>(VoxelMaterialFieldMapping::ParentLocal))
      return fail(error, "Voxel material field " + std::to_string(index) +
          " has an invalid mapping");
    if (static_cast<uint32_t>(binding.filter) >
        static_cast<uint32_t>(VoxelMaterialFieldFilter::Linear) ||
        static_cast<uint32_t>(binding.addressMode) >
        static_cast<uint32_t>(VoxelMaterialFieldAddressMode::Repeat))
      return fail(error, "Voxel material field " + std::to_string(index) +
          " has invalid sampling options");
    if (!std::isfinite(binding.defaultValue) ||
        binding.defaultValue < 0.0f || binding.defaultValue > 1.0f ||
        !std::isfinite(binding.scale) || !std::isfinite(binding.bias))
      return fail(error, "Voxel material field " + std::to_string(index) +
          " has invalid scalar values");
    if (binding.semantic == VoxelMaterialFieldSemantic::Unused)
    {
      if (binding.source != VoxelMaterialFieldSource::Constant ||
          !binding.textureAssetId.empty())
        return fail(error, "Unused voxel material field " +
            std::to_string(index) + " must be constant and unbound");
      continue;
    }
    if (binding.source == VoxelMaterialFieldSource::MaterialTexture &&
        binding.textureAssetId.empty())
      return fail(error, "Material-texture voxel field has no texture asset");
    if (binding.source != VoxelMaterialFieldSource::MaterialTexture &&
        !binding.textureAssetId.empty())
      return fail(error, "Only material-texture fields may name a texture asset");
  }
  return true;
}

} // namespace voxel::material
