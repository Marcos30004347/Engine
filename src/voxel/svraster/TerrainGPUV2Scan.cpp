#include "TerrainGPUV2Scan.hpp"

#include <limits>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}
} // namespace

bool terrainGPUV2ExclusiveScanCPU(const std::vector<uint32_t> &input, std::vector<uint32_t> &output, TerrainGPUV2ScanStatusGPU &status, std::string *error)
{
  status = {};
  if (input.size() > kTerrainGPUV2ScanMaximumElements)
  {
    status.overflow = 1u;
    return fail(error, "terrain GPU V2 scan exceeds maximum element count");
  }
  std::vector<uint32_t> scanned(input.size());
  uint64_t total = 0u;
  for (size_t index = 0u; index < input.size(); ++index)
  {
    if (total > std::numeric_limits<uint32_t>::max())
    {
      status.overflow = 1u;
      return fail(error, "terrain GPU V2 scan overflows uint32");
    }
    scanned[index] = static_cast<uint32_t>(total);
    total += input[index];
  }
  if (total > std::numeric_limits<uint32_t>::max())
  {
    status.overflow = 1u;
    return fail(error, "terrain GPU V2 scan overflows uint32");
  }
  status.total = static_cast<uint32_t>(total);
  status.outputCapacity = std::numeric_limits<uint32_t>::max();
  output = std::move(scanned);
  return true;
}

bool terrainGPUV2CompactCPU(
    const std::vector<uint32_t> &flags,
    const uint32_t outputCapacity,
    std::vector<uint32_t> &offsets,
    std::vector<uint32_t> &compactedIndices,
    TerrainGPUV2ScanStatusGPU &status,
    IndirectDispatchArgsGPU &indirect,
    std::string *error)
{
  status = {};
  status.outputCapacity = outputCapacity;
  indirect = {};
  for (const uint32_t flag : flags)
    if (flag > 1u)
    {
      status.overflow = 1u;
      return fail(error, "terrain GPU V2 compaction flags must be zero or one");
    }
  std::vector<uint32_t> scanned;
  if (!terrainGPUV2ExclusiveScanCPU(flags, scanned, status, error))
  {
    status.outputCapacity = outputCapacity;
    return false;
  }
  status.outputCapacity = outputCapacity;
  if (status.total > outputCapacity)
  {
    status.overflow = 1u;
    offsets = std::move(scanned);
    return true;
  }
  std::vector<uint32_t> compacted(status.total);
  for (uint32_t index = 0u; index < flags.size(); ++index)
    if (flags[index] != 0u)
      compacted[scanned[index]] = index;
  offsets = std::move(scanned);
  compactedIndices = std::move(compacted);
  indirect = {(status.total + kTerrainGPUV2CompactWorkgroupSize - 1u) / kTerrainGPUV2CompactWorkgroupSize, 1u, 1u, 0u};
  return true;
}

} // namespace voxel::svraster
