#include "voxel/svraster/TerrainGPURasterBuild.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr) *error = message;
  return false;
}

int32_t floorDivide(const int32_t value, const int32_t divisor)
{
  const int32_t quotient = value / divisor;
  const int32_t remainder = value % divisor;
  return quotient - (remainder < 0 ? 1 : 0);
}

uint64_t candidateCount(const TerrainGPURasterLevelGPU &level)
{
  return uint64_t{level.sizeX} * level.sizeY * level.sizeZ;
}

void mix32(uint32_t &hash, const uint32_t value)
{
  hash ^= value;
  hash *= 16777619u;
}

void mix64(uint32_t &low, uint32_t &high, const uint32_t value)
{
  mix32(low, value);
  mix32(high, value ^ low ^ 0x9e3779b9u);
}

uint32_t floatBits(const float value)
{
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
} // namespace

bool planTerrainGPURasterBuild(
    const WorldMapTerrainColumnPage &page,
    const TerrainGPURasterCapacity &capacity,
    const uint32_t expectedTerrainGeneration,
    const uint64_t buildRevision,
    const uint32_t paletteCount,
    TerrainGPURasterConfigGPU &config,
    std::string *error)
{
  if (error != nullptr) error->clear();
  if (!validateWorldMapTerrainColumns(page, error)) return false;
  return planTerrainGPURasterBuild(
      TerrainGPURasterPageDescription{
        page.side, page.maximumVerticalVoxels,
        page.voxelSizeMetres, page.edgeMetres},
      capacity, expectedTerrainGeneration, buildRevision, paletteCount,
      config, error);
}

bool planTerrainGPURasterBuild(
    const TerrainGPURasterPageDescription &page,
    const TerrainGPURasterCapacity &capacity,
    const uint32_t expectedTerrainGeneration,
    const uint64_t buildRevision,
    const uint32_t paletteCount,
    TerrainGPURasterConfigGPU &config,
    std::string *error)
{
  config = {};
  if (error != nullptr) error->clear();
  if (page.side == 0u || (page.side & 1u) != 0u || page.side > 1024u ||
      page.maximumVerticalVoxels == 0u ||
      page.maximumVerticalVoxels > 4096u ||
      !(page.voxelSizeMetres > 0.0) || !(page.edgeMetres > 0.0) ||
      !std::isfinite(page.voxelSizeMetres) ||
      !std::isfinite(page.edgeMetres) ||
      std::abs(page.edgeMetres - page.voxelSizeMetres * page.side) >
          std::max(1.0e-6, page.edgeMetres * 1.0e-6))
    return fail(error,
        "GPU raster Phase 3 requires an even, centred integral voxel page");
  if (capacity.denseCandidates == 0u || capacity.nodes == 0u ||
      capacity.children == 0u || capacity.materials == 0u ||
      capacity.roots == 0u || paletteCount == 0u || paletteCount > 256u)
    return fail(error, "GPU raster Phase 3 capacities are invalid");

  config.side = page.side;
  config.maximumVerticalVoxels = page.maximumVerticalVoxels;
  const uint64_t columnCount = uint64_t{page.side} * page.side;
  if (columnCount > std::numeric_limits<uint32_t>::max())
    return fail(error, "GPU raster Phase 3 column count is not representable");
  config.columnCount = static_cast<uint32_t>(columnCount);
  config.nodeCapacity = capacity.nodes;
  config.childCapacity = capacity.children;
  config.materialCapacity = capacity.materials;
  config.rootCapacity = capacity.roots;
  config.denseCandidateCapacity = capacity.denseCandidates;
  config.materialWordCapacity = (capacity.materials + 3u) / 4u;
  config.paletteCount = paletteCount;
  config.expectedTerrainGeneration = expectedTerrainGeneration;
  config.buildRevisionLow = static_cast<uint32_t>(buildRevision);
  config.buildRevisionHigh = static_cast<uint32_t>(buildRevision >> 32u);
  config.voxelSizeBits = floatBits(static_cast<float>(page.voxelSizeMetres));
  config.edgeBits = floatBits(static_cast<float>(page.edgeMetres));

  const int32_t minimumCell = -static_cast<int32_t>(page.side / 2u);
  const int32_t maximumCell = minimumCell + static_cast<int32_t>(page.side) - 1;
  TerrainGPURasterLevelGPU level{};
  level.minimumX = floorDivide(minimumCell, 8);
  level.minimumY = 0;
  level.minimumZ = level.minimumX;
  const int32_t maximumX = floorDivide(maximumCell, 8);
  const int32_t maximumY = floorDivide(
      static_cast<int32_t>(page.maximumVerticalVoxels) - 1, 8);
  level.sizeX = static_cast<uint32_t>(maximumX - level.minimumX + 1);
  level.sizeY = static_cast<uint32_t>(maximumY + 1);
  level.sizeZ = level.sizeX;

  uint64_t denseCount = 0u;
  for (uint32_t index = 0u; index < kTerrainGPURasterMaximumLevels; ++index)
  {
    level.candidateBase = static_cast<uint32_t>(denseCount);
    const uint64_t count = candidateCount(level);
    if (count == 0u || denseCount + count > capacity.denseCandidates ||
        denseCount + count > std::numeric_limits<uint32_t>::max())
      return fail(error,
          "GPU raster Phase 3 dense candidate capacity is insufficient");
    config.levels[index] = level;
    denseCount += count;
    ++config.maximumLevelCount;
    // Centred even-sized pages straddle signed grid zero. Their negative and
    // positive brick roots (-1 and 0) intentionally never merge under floor
    // division, so completion is a 2x1x2 root forest rather than one root.
    // The vertical axis is the only dimension that can still require another
    // reduction once X/Z are at that stable centred extent.
    if (level.sizeX <= 2u && level.sizeY == 1u && level.sizeZ <= 2u)
      break;

    const int32_t maximumLevelX = level.minimumX +
        static_cast<int32_t>(level.sizeX) - 1;
    const int32_t maximumLevelY = level.minimumY +
        static_cast<int32_t>(level.sizeY) - 1;
    const int32_t maximumLevelZ = level.minimumZ +
        static_cast<int32_t>(level.sizeZ) - 1;
    level.minimumX = floorDivide(level.minimumX, 2);
    level.minimumY = floorDivide(level.minimumY, 2);
    level.minimumZ = floorDivide(level.minimumZ, 2);
    level.sizeX = static_cast<uint32_t>(
        floorDivide(maximumLevelX, 2) - level.minimumX + 1);
    level.sizeY = static_cast<uint32_t>(
        floorDivide(maximumLevelY, 2) - level.minimumY + 1);
    level.sizeZ = static_cast<uint32_t>(
        floorDivide(maximumLevelZ, 2) - level.minimumZ + 1);
  }
  if (config.levels[config.maximumLevelCount - 1u].sizeY != 1u)
    return fail(error,
        "GPU raster Phase 3 level capacity cannot cover the vertical datum");
  return true;
}

bool buildTerrainGPURasterMaterialTable(
    const WorldMapTerrainColumnPage &page,
    const SparseVoxelBrickHierarchy &reference,
    std::array<TerrainGPURasterMaterialGPU,
        kTerrainGPURasterMaterialTableSize> &table,
    std::string *error)
{
  table = {};
  for (auto &entry : table) entry.paletteEntry = 0xffffffffu;
  if (error != nullptr) error->clear();
  if (!validateWorldMapTerrainColumns(page, error)) return false;
  if (reference.palette.empty() || reference.palette.size() > 256u)
    return fail(error, "GPU raster Phase 3 needs one exact terrain palette");

  std::set<uint8_t> used;
  for (const auto &column : page.columns)
    used.insert(worldMapTerrainColumnMaterial(column));
  for (const uint8_t materialId : used)
  {
    const SparseVoxelBrickMaterialGPU packed = packSparseVoxelBrickMaterial(
        planetVoxelBaseMaterial(materialId));
    const auto match = std::find_if(reference.palette.begin(),
        reference.palette.end(), [&](const SparseVoxelBrickMaterialGPU &entry)
        {
          return entry.packed == packed.packed;
        });
    if (match == reference.palette.end())
      return fail(error,
          "GPU raster Phase 3 material is absent from the exact palette");
    const uint32_t entry = static_cast<uint32_t>(
        std::distance(reference.palette.begin(), match));
    table[materialId] = {entry, packed.packed[0], packed.packed[1], 0u};
  }
  return true;
}

bool buildTerrainGPURasterMaterialTable(
    const SparseVoxelBrickHierarchy &reference,
    std::array<TerrainGPURasterMaterialGPU,
        kTerrainGPURasterMaterialTableSize> &table,
    std::string *error)
{
  table = {};
  for (auto &entry : table) entry.paletteEntry = 0xffffffffu;
  if (error != nullptr) error->clear();
  if (reference.palette.empty() || reference.palette.size() > 256u)
    return fail(error, "GPU raster Phase 3 needs one exact terrain palette");

  for (uint32_t materialId = 0u;
       materialId < kTerrainGPURasterMaterialTableSize; ++materialId)
  {
    const SparseVoxelBrickMaterialGPU packed = packSparseVoxelBrickMaterial(
        planetVoxelBaseMaterial(static_cast<uint8_t>(materialId)));
    const auto match = std::find_if(reference.palette.begin(),
        reference.palette.end(), [&](const SparseVoxelBrickMaterialGPU &entry)
        {
          return entry.packed == packed.packed;
        });
    if (match == reference.palette.end())
      continue;
    table[materialId] = {
      static_cast<uint32_t>(
          std::distance(reference.palette.begin(), match)),
      packed.packed[0], packed.packed[1], 0u};
  }
  return true;
}

bool validateTerrainGPURasterHierarchy(
    const SparseVoxelBrickHierarchy &hierarchy, std::string *error)
{
  if (error != nullptr) error->clear();
  if (hierarchy.nodes.empty() ||
      hierarchy.occupancy.size() != hierarchy.nodes.size() ||
      hierarchy.materialIndices.size() != hierarchy.microSurfaceData.size() ||
      hierarchy.rootIndices.empty() ||
      hierarchy.rootCount != hierarchy.rootIndices.size())
    return fail(error, "GPU raster hierarchy has inconsistent array sizes");

  std::vector<uint8_t> reachable(hierarchy.nodes.size(), 0u);
  std::vector<uint32_t> pending = hierarchy.rootIndices;
  for (const uint32_t root : hierarchy.rootIndices)
    if (root >= hierarchy.nodes.size() ||
        hierarchy.nodes[root].parentIndex != kSparseVoxelBrickInvalidIndex)
      return fail(error, "GPU raster hierarchy has an invalid root");
  while (!pending.empty())
  {
    const uint32_t index = pending.back();
    pending.pop_back();
    if (index >= hierarchy.nodes.size() || reachable[index] != 0u)
      return fail(error, "GPU raster hierarchy has a cycle or invalid child");
    reachable[index] = 1u;
    const auto &node = hierarchy.nodes[index];
    if (node.occupancyIndex >= hierarchy.occupancy.size() ||
        uint64_t{node.materialBase} + node.occupiedCount >
            hierarchy.materialIndices.size() ||
        uint64_t{node.firstChild} + node.childCount >
            hierarchy.childIndices.size())
      return fail(error, "GPU raster hierarchy has an invalid node range");
    uint32_t prefix = 0u;
    for (const auto &word : hierarchy.occupancy[node.occupancyIndex].cells)
    {
      if (word.prefix != prefix)
        return fail(error, "GPU raster occupancy prefix is invalid");
      prefix += static_cast<uint32_t>(__builtin_popcount(word.bits));
    }
    if (prefix != node.occupiedCount)
      return fail(error, "GPU raster occupied count is invalid");
    for (uint32_t child = 0u; child < node.childCount; ++child)
    {
      const uint32_t childIndex =
          hierarchy.childIndices[node.firstChild + child];
      if (childIndex >= hierarchy.nodes.size() ||
          hierarchy.nodes[childIndex].parentIndex != index ||
          hierarchy.nodes[childIndex].level + 1u != node.level ||
          hierarchy.nodes[childIndex].parentError != node.selfError ||
          node.selfError < hierarchy.nodes[childIndex].selfError)
        return fail(error, "GPU raster parent/child invariant is invalid");
      pending.push_back(childIndex);
    }
  }
  if (std::find(reachable.begin(), reachable.end(), uint8_t{0u}) !=
      reachable.end())
    return fail(error, "GPU raster hierarchy contains unreachable nodes");
  return true;
}

uint64_t terrainGPURasterSemanticHash(
    const SparseVoxelBrickHierarchy &hierarchy)
{
  uint32_t low = 2166136261u;
  uint32_t high = 2166136261u ^ 0x9e3779b9u;
  const auto mix = [&](const uint32_t value)
  {
    mix64(low, high, value);
  };
  mix(static_cast<uint32_t>(hierarchy.nodes.size()));
  mix(static_cast<uint32_t>(hierarchy.childIndices.size()));
  mix(static_cast<uint32_t>(hierarchy.materialIndices.size()));
  mix(static_cast<uint32_t>(hierarchy.rootIndices.size()));
  for (const auto &node : hierarchy.nodes)
  {
    for (const float value : node.minimumVoxelSize)
      mix(floatBits(value));
    mix(node.packedOccupiedBounds);
    mix(node.coarseOccupancyLow);
    mix(node.coarseOccupancyHigh);
    mix(node.occupancyIndex);
    mix(node.materialBase);
    mix(node.parentIndex);
    mix(node.childCount);
    mix(node.level);
    mix(node.occupiedCount);
    mix(node.firstChild);
    mix(node.instanceIndex);
    mix(node.paletteBase);
    mix(node.regionMappingId);
    mix(floatBits(node.selfError));
    mix(floatBits(node.parentError));
    for (const int32_t value : node.canonicalGridOriginScale)
      mix(static_cast<uint32_t>(value));
  }
  for (const auto &occupancy : hierarchy.occupancy)
    for (const auto &word : occupancy.cells)
    {
      mix(word.bits);
      mix(word.prefix);
    }
  for (const uint8_t value : hierarchy.materialIndices) mix(value);
  for (const uint64_t value : hierarchy.microSurfaceData)
  {
    mix(static_cast<uint32_t>(value));
    mix(static_cast<uint32_t>(value >> 32u));
  }
  for (const uint32_t value : hierarchy.childIndices) mix(value);
  for (const uint32_t value : hierarchy.rootIndices) mix(value);
  return uint64_t{low} | (uint64_t{high} << 32u);
}

} // namespace voxel::svraster
