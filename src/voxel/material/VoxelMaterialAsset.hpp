#pragma once

#include "voxel/material/VoxelMaterial.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace voxel::material
{

inline constexpr uint32_t kVoxelMaterialAssetVersion = 3u;
inline constexpr uint32_t kVoxelMaterialFileVersion = 1u;

// Authored appearance defaults remain separate from the compact legacy
// descriptor.  The brick palette supplies these values today; keeping them in
// the asset makes the material self-contained for asset loading and editor
// preview without changing the shipping palette ABI.
struct VoxelMaterialSurfaceDefaults
{
  uint32_t primaryAlbedoRGBA8 = 0xffffffffu;
  uint32_t secondaryAlbedoRGBA8 = 0xffffffffu;
  uint32_t tertiaryAlbedoRGBA8 = 0xffffffffu;
  uint32_t quaternaryAlbedoRGBA8 = 0xffffffffu;
  float roughness = 1.0f;
  float metallic = 0.0f;
  float emissive = 0.0f;

  bool operator==(const VoxelMaterialSurfaceDefaults &other) const;
};

struct VoxelMaterialAsset
{
  uint32_t version = kVoxelMaterialAssetVersion;
  std::string assetId;
  std::string name;
  VoxelMaterialSurfaceDefaults surface{};
  svraster::SparseVoxelMicroDescriptor geometry{};
  svraster::SparseVoxelMicroShadingDescriptor legacyShading{};
  VoxelMaterialContextRequirements contextRequirements{};
  VoxelMaterialSlotPolicy slotPolicy = VoxelMaterialSlotPolicy::UniqueEditable;
  VoxelShadingProgram shadingProgram{};
  VoxelShadingParameterBlockGPU shadingParameters{};
  VoxelGeometryProgram geometryProgram{};
  VoxelGeometryParameterBlockGPU geometryParameters{};
  VoxelMaterialFieldBindings fieldBindings{};

  bool validate(std::string *error = nullptr) const;
  bool definition(VoxelMaterialDefinition &result,
                  std::string *error = nullptr) const;
  uint64_t contentHash() const;
};

struct VoxelMaterialCompiledAsset
{
  uint64_t sourceHash = 0u;
  VoxelMaterialDefinition definition{};
  VoxelGeometryCompilation geometry{};
  std::vector<VoxelShadingInstructionGPU> shadingInstructions;
};

class VoxelMaterialCompilerCache
{
public:
  bool compile(
      const VoxelMaterialAsset &asset,
      const VoxelMaterialCompiledAsset *&result,
      std::string *error = nullptr);
  size_t entryCount() const { return entries_.size(); }
  uint64_t hitCount() const { return hitCount_; }
  uint64_t missCount() const { return missCount_; }
  void clear();

private:
  std::map<uint64_t, VoxelMaterialCompiledAsset> entries_;
  uint64_t hitCount_ = 0u;
  uint64_t missCount_ = 0u;
};

bool serializeVoxelMaterialAsset(
    const VoxelMaterialAsset &asset, std::vector<uint8_t> &bytes,
    std::string *error = nullptr);
bool deserializeVoxelMaterialAsset(
    const std::vector<uint8_t> &bytes, VoxelMaterialAsset &asset,
    std::string *error = nullptr);
bool writeVoxelMaterialAsset(
    const std::string &path, const VoxelMaterialAsset &asset,
    std::string *error = nullptr);
bool readVoxelMaterialAsset(
    const std::string &path, VoxelMaterialAsset &asset,
    std::string *error = nullptr);

// Compatibility adapter for version-zero scenes that carry only the existing
// packed descriptor. The generated asset deliberately keeps empty programs so
// it executes through the specialized legacy path.
bool adaptLegacyVoxelMaterialAsset(
    const std::string &assetId, const std::string &name,
    const svraster::SparseVoxelMicroMaterialGPU &legacy,
    VoxelMaterialAsset &asset, std::string *error = nullptr);

bool loadVoxelMaterialAsset(
    VoxelMaterialManager &manager, VoxelMaterialCompilerCache &cache,
    const VoxelMaterialAsset &asset, VoxelMaterialHandle &handle,
    std::string *error = nullptr);
bool hotReloadVoxelMaterialAsset(
    VoxelMaterialManager &manager, VoxelMaterialCompilerCache &cache,
    VoxelMaterialHandle handle, const VoxelMaterialAsset &asset,
    VoxelMaterialChange &changes, std::string *error = nullptr);

} // namespace voxel::material
