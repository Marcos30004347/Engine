#pragma once

#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelBLAS
{
  std::array<float, 3> rootCenter{};
  float rootExtent = 0.0f;
  uint32_t maxLevel = 0u;
  uint32_t flags = 0u;
  uint32_t firstRoot = 0u;
  uint32_t rootCount = 0u;
  std::vector<SparseVoxelLODNodeGPU> hierarchyNodes;
  std::vector<uint64_t> octreePaths;
  std::vector<uint8_t> octreeLevels;
  std::vector<SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> sourceIds;
};

struct SparseVoxelInstance
{
  uint32_t blasIndex = 0u;
  uint32_t flags = 0u;
  std::array<float, 16> modelToWorld{};
};

struct SparseVoxelSourceInfo
{
  uint32_t importerVersion = 1u;
  std::string sourceRelativePath;
  std::string normalizedBuildOptions;
};

// Stable material identity persisted independently from the runtime handle.
// sourceMaterialId is the ID stored by source voxels/material records, while
// assetId is resolved through VoxelMaterialManager when a scene is loaded.
struct SparseVoxelMaterialAssetReference
{
  uint32_t sourceMaterialId = 0u;
  uint32_t contextFields = 0u;
  uint64_t assetHash = 0u;
  std::string assetId;

  bool operator==(const SparseVoxelMaterialAssetReference &other) const
  {
    return sourceMaterialId == other.sourceMaterialId &&
        contextFields == other.contextFields && assetHash == other.assetHash &&
        assetId == other.assetId;
  }
};

struct SparseVoxelScene
{
  std::vector<SparseVoxelBLAS> models;
  std::vector<SparseVoxelInstance> instances;
  SparseVoxelSourceInfo sourceInfo{};
  std::vector<SparseVoxelMaterialAssetReference> voxelMaterialReferences;
  uint64_t sourceContentHash = 0u;
  uint64_t buildSettingsHash = 0u;
  uint32_t tlasFanout = 8u;
  uint32_t tlasLeafSize = 4u;
};

bool validateSparseVoxelSceneSource(const SparseVoxelScene &scene, std::string *error = nullptr);
SparseVoxelScene makeExteriorShellScene(uint64_t materialSeed);

} // namespace voxel::svraster
