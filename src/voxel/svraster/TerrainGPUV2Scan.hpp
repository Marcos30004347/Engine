#pragma once

#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

inline constexpr uint32_t kTerrainGPUV2ScanBlockSize = 256u;
inline constexpr uint32_t kTerrainGPUV2ScanMaximumElements = 1u << 20u;
inline constexpr uint32_t kTerrainGPUV2CompactWorkgroupSize = 64u;

struct TerrainGPUV2ScanStatusGPU
{
  uint32_t total = 0u;
  uint32_t overflow = 0u;
  uint32_t outputCapacity = 0u;
  uint32_t reserved = 0u;
};

static_assert(sizeof(TerrainGPUV2ScanStatusGPU) == 16u);

// CPU differential oracle. On arithmetic overflow, output is left unchanged.
bool terrainGPUV2ExclusiveScanCPU(const std::vector<uint32_t> &input, std::vector<uint32_t> &output, TerrainGPUV2ScanStatusGPU &status, std::string *error = nullptr);

// Stable compaction of indices whose flags equal one. Flags other than zero or
// one are invalid. Capacity/arithmetic overflow is reported without modifying
// compactedIndices.
bool terrainGPUV2CompactCPU(
    const std::vector<uint32_t> &flags,
    uint32_t outputCapacity,
    std::vector<uint32_t> &offsets,
    std::vector<uint32_t> &compactedIndices,
    TerrainGPUV2ScanStatusGPU &status,
    IndirectDispatchArgsGPU &indirect,
    std::string *error = nullptr);

} // namespace voxel::svraster
