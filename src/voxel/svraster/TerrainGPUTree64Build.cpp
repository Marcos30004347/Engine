#include "voxel/svraster/TerrainGPUTree64Build.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr) *error = message;
  return false;
}

uint32_t floatBits(const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

void mix(uint32_t &low, uint32_t &high, const uint32_t value)
{
  low = (low ^ value) * 16777619u;
  high = (high ^ (value ^ low ^ 0x9e3779b9u)) * 16777619u;
}
} // namespace

bool planTerrainGPUTree64Build(
    const WorldMapTerrainColumnPage &columns,
    const TerrainGPUTree64Capacity &capacity,
    const uint32_t expectedTerrainGeneration,
    const uint64_t geometryRevision,
    const uint32_t terrainSlot,
    const uint32_t nodePageSlot,
    const uint32_t nodePageGeneration,
    TerrainGPUTree64ConfigGPU &config,
    std::string *error)
{
  if (error != nullptr) error->clear();
  if (!validateWorldMapTerrainColumns(columns, error)) return false;
  if (capacity.masks == 0u || capacity.nodes == 0u)
    return fail(error, "GPU terrain Tree64 capacities are invalid");

  uint32_t requiredCells = columns.side;
  for (const auto &column : columns.columns)
    requiredCells = std::max(requiredCells, column.topY + 1u);
  TerrainGPUTree64PageDescription page{
    columns.side, requiredCells, columns.internalLOD,
    columns.voxelSizeMetres, columns.edgeMetres};
  if (!planTerrainGPUTree64Build(
          page, capacity, expectedTerrainGeneration, geometryRevision,
          terrainSlot, nodePageSlot, nodePageGeneration, config, error))
    return false;
  uint64_t occupied = 0u;
  for (const auto &column : columns.columns)
    occupied += uint64_t{column.topY} - column.firstSolidY + 1u;
  config.occupiedVoxelCount = static_cast<uint32_t>(
      std::min<uint64_t>(occupied, std::numeric_limits<uint32_t>::max()));
  return true;
}

bool planTerrainGPUTree64Build(
    const TerrainGPUTree64PageDescription &page,
    const TerrainGPUTree64Capacity &capacity,
    const uint32_t expectedTerrainGeneration,
    const uint64_t geometryRevision,
    const uint32_t terrainSlot,
    const uint32_t nodePageSlot,
    const uint32_t nodePageGeneration,
    TerrainGPUTree64ConfigGPU &config,
    std::string *error)
{
  config = {};
  if (error != nullptr) error->clear();
  if (page.columnSide == 0u || page.maximumVerticalVoxels == 0u ||
      !(page.voxelSizeMetres > 0.0) || !(page.edgeMetres > 0.0) ||
      capacity.masks == 0u || capacity.nodes == 0u)
    return fail(error, "GPU terrain Tree64 page description is invalid");

  const uint32_t requiredCells = std::max(
      page.columnSide, page.maximumVerticalVoxels);
  const uint32_t side = sparseVoxelTerrainTree64SideForCells(requiredCells);
  const uint32_t depth = sparseVoxelTree64ModelDepth(side);
  if (side == 0u || depth == 0u || depth > kTerrainGPUTree64MaximumLevels)
    return fail(error, "GPU terrain Tree64 side is not representable");

  config.side = side;
  config.levelCount = depth;
  config.columnSide = page.columnSide;
  const uint64_t columnCount = uint64_t{page.columnSide} * page.columnSide;
  if (columnCount > std::numeric_limits<uint32_t>::max())
    return fail(error, "GPU terrain Tree64 column count is not representable");
  config.columnCount = static_cast<uint32_t>(columnCount);
  config.maskCapacity = capacity.masks;
  config.nodeCapacity = capacity.nodes;
  config.expectedTerrainGeneration = expectedTerrainGeneration;
  config.internalLOD = page.internalLOD;
  config.terrainSlot = terrainSlot;
  config.nodePageSlot = nodePageSlot;
  config.nodePageGeneration = nodePageGeneration;
  // The full-GPU validator derives this from the generated columns. The CPU
  // oracle overload overwrites it so differential tests retain their exact
  // expected config.
  config.occupiedVoxelCount = 0u;
  config.geometryRevisionLow = static_cast<uint32_t>(geometryRevision);
  config.geometryRevisionHigh = static_cast<uint32_t>(geometryRevision >> 32u);
  config.voxelSizeBits = floatBits(
      static_cast<float>(page.voxelSizeMetres));
  config.edgeBits = floatBits(static_cast<float>(page.edgeMetres));

  uint64_t maskCount = 0u;
  uint32_t horizontalAxis =
      (page.columnSide + kSparseVoxelTree64Branching - 1u) /
      kSparseVoxelTree64Branching;
  uint32_t verticalAxis =
      (page.maximumVerticalVoxels + kSparseVoxelTree64Branching - 1u) /
      kSparseVoxelTree64Branching;
  for (uint32_t level = 0u; level < depth; ++level)
  {
    const uint64_t count =
        uint64_t{horizontalAxis} * verticalAxis * horizontalAxis;
    if (count == 0u || maskCount + count > capacity.masks ||
        maskCount + count > std::numeric_limits<uint32_t>::max())
      return fail(error, "GPU terrain Tree64 mask capacity is insufficient");
    config.levelOffsets[level] = static_cast<uint32_t>(maskCount);
    config.levelCounts[level] = static_cast<uint32_t>(count);
    config.levelHorizontalAxes[level] = horizontalAxis;
    config.levelVerticalAxes[level] = verticalAxis;
    maskCount += count;
    if (horizontalAxis == 1u && verticalAxis == 1u) break;
    horizontalAxis =
        (horizontalAxis + kSparseVoxelTree64Branching - 1u) /
        kSparseVoxelTree64Branching;
    verticalAxis =
        (verticalAxis + kSparseVoxelTree64Branching - 1u) /
        kSparseVoxelTree64Branching;
  }
  if (config.levelCounts[depth - 1u] != 1u)
    return fail(error, "GPU terrain Tree64 root mask is missing");
  return true;
}

bool validateTerrainGPUTree64Chunk(
    const SparseVoxelTree64Chunk &tree, std::string *error)
{
  return validateSparseVoxelTree64Model(tree, error);
}

uint64_t terrainGPUTree64SemanticHash(const SparseVoxelTree64Chunk &tree)
{
  uint32_t low = 2166136261u;
  uint32_t high = 2166136261u ^ 0x9e3779b9u;
  mix(low, high, tree.side);
  mix(low, high, static_cast<uint32_t>(tree.nodes.size()));
  for (const auto &node : tree.nodes)
  {
    mix(low, high, node.childBaseAndFlags);
    mix(low, high, node.populationMaskLo);
    mix(low, high, node.populationMaskHi);
  }
  return uint64_t{low} | (uint64_t{high} << 32u);
}

} // namespace voxel::svraster
