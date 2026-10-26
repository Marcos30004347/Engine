#pragma once

#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

constexpr uint64_t kDefaultExteriorShellMaterialSeed = 0x4d595df4d0f33173ull;
constexpr float kBootstrapRootExtent = 2.0f;

struct BootstrapSparseVoxelModel
{
  SparseVoxelBLASGPU blas{};
  std::vector<SparseVoxelPathLevelGPU> pathsLevels;
  std::vector<SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<SparseVoxelMaterialGPU> materials;
  SparseVoxelInstanceGPU instance{};
};

struct BootstrapSparseVoxelLOD
{
  std::vector<SparseVoxelLODNodeGPU> nodes;
  std::vector<SparseVoxelPathLevelGPU> pathsLevels;
  std::vector<SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<SparseVoxelMaterialGPU> materials;
  uint32_t firstRoot = 0u;
  uint32_t rootCount = 0u;
};

uint64_t splitmix64(uint64_t value);
uint64_t encodeOctreePath(uint32_t x, uint32_t y, uint32_t z, uint32_t level);
std::array<uint32_t, 3> decodeOctreePath(uint64_t path, uint32_t level);
bool isCanonicalOctreePath(uint64_t path, uint32_t level);

uint16_t floatToHalfRoundToNearestEven(float value);
float halfToFloat(uint16_t value);

SparseVoxelMaterialGPU generateExteriorShellMaterial(uint64_t path, uint64_t materialSeed);
SparseVoxelMaterialGPU reduceSparseVoxelLODMaterials(
    const std::vector<SparseVoxelMaterialGPU> &children,
    uint32_t materialId);
BootstrapSparseVoxelModel generateExteriorShell128(uint64_t materialSeed = kDefaultExteriorShellMaterialSeed);
BootstrapSparseVoxelLOD generateExteriorShellLOD128(uint64_t materialSeed = kDefaultExteriorShellMaterialSeed);
bool validateBootstrapSparseVoxelModel(const BootstrapSparseVoxelModel &model, std::string *error = nullptr);
bool validateBootstrapSparseVoxelLOD(const BootstrapSparseVoxelLOD &hierarchy, std::string *error = nullptr);

} // namespace voxel::svraster
