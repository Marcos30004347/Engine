#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace voxel::svraster
{
namespace
{

using BrickKey = std::array<int32_t, 3>;

struct BrickKeyHash
{
  size_t operator()(const BrickKey &key) const
  {
    size_t hash = 1469598103934665603ull;
    for (const int32_t value : key)
    {
      hash ^= static_cast<uint32_t>(value);
      hash *= 1099511628211ull;
    }
    return hash;
  }
};

struct TerrainBrick
{
  BrickKey key{};
  std::array<uint32_t, kSparseVoxelBrickOccupancyWords> words{};
  // Low byte is the source material, high byte the normalized terrain
  // context. UINT16_MAX is empty. A fixed array replaces 512 ordered-map
  // allocations and lets emission walk occupancy bit order directly.
  std::array<uint16_t, kSparseVoxelBrickVoxelCount> cells{};
  float selfError = 0.0f;

  TerrainBrick()
  {
    cells.fill(std::numeric_limits<uint16_t>::max());
  }
};

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

int32_t floorDivide(const int32_t value, const int32_t divisor)
{
  const int32_t quotient = value / divisor;
  return quotient - ((value % divisor != 0 && (value < 0) != (divisor < 0)) ? 1 : 0);
}

int32_t positiveModulo(const int32_t value, const int32_t divisor)
{
  const int32_t remainder = value % divisor;
  return remainder < 0 ? remainder + divisor : remainder;
}

uint32_t cellIndex(const uint32_t x, const uint32_t y, const uint32_t z)
{
  return x + kSparseVoxelBrickSide * (y + kSparseVoxelBrickSide * z);
}

bool occupied(const TerrainBrick &brick, const uint32_t cell)
{
  return (brick.words[cell / 32u] & (1u << (cell % 32u))) != 0u;
}

uint32_t contextRank(const uint16_t cell)
{
  const uint32_t context = cell >> 8u;
  if (context >= static_cast<uint32_t>(SparseVoxelMicroContext::WallNegativeX) && context <= static_cast<uint32_t>(SparseVoxelMicroContext::WallPositiveZ))
    return 4u;
  if (sparseVoxelMicroContextIsSurface(context))
    return 3u;
  return context == static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface) ? 1u : 0u;
}

void setFineCell(TerrainBrick &brick, const uint32_t cell, const uint16_t materialContext)
{
  if (occupied(brick, cell))
    return;
  brick.words[cell / 32u] |= 1u << (cell % 32u);
  brick.cells[cell] = materialContext;
}

void setParentCell(TerrainBrick &brick, const uint32_t cell, const uint16_t materialContext)
{
  if (!occupied(brick, cell))
  {
    setFineCell(brick, cell, materialContext);
    return;
  }
  if (contextRank(materialContext) > contextRank(brick.cells[cell]))
    brick.cells[cell] = materialContext;
}

auto brickLess()
{
  return [](const TerrainBrick &left, const TerrainBrick &right)
  {
    return left.key < right.key;
  };
}

const TerrainBrick *findBrick(const std::vector<TerrainBrick> &level, const BrickKey &key)
{
  const auto found = std::lower_bound(
      level.begin(),
      level.end(),
      key,
      [](const TerrainBrick &brick, const BrickKey &candidate)
      {
        return brick.key < candidate;
      });
  return found != level.end() && found->key == key ? &*found : nullptr;
}

uint32_t findBrickIndex(const std::vector<TerrainBrick> &level, const BrickKey &key)
{
  const auto found = std::lower_bound(
      level.begin(),
      level.end(),
      key,
      [](const TerrainBrick &brick, const BrickKey &candidate)
      {
        return brick.key < candidate;
      });
  return found != level.end() && found->key == key ? static_cast<uint32_t>(std::distance(level.begin(), found)) : kSparseVoxelBrickInvalidIndex;
}

uint16_t columnCell(const WorldMapTerrainColumnGPU &column, const WorldMapTerrainColumnWallGPU &wall, const uint32_t y)
{
  uint32_t context = worldMapTerrainColumnSurfaceContext(column);
  if (y != column.topY)
  {
    context = static_cast<uint32_t>(SparseVoxelMicroContext::Interior);
    if (y < wall.firstSolidYWithoutBoundaryStitch)
      context = worldMapTerrainColumnBoundaryContext(wall);
    if (wall.lowestNeighbourY < y)
      context = worldMapTerrainColumnWallContext(wall);
  }
  return static_cast<uint16_t>(worldMapTerrainColumnMaterial(column) | (context << 8u));
}

} // namespace

bool buildWorldMapTerrainColumnHierarchy(const WorldMapTerrainColumnPage &page, const uint32_t maximumBricks, WorldMapTerrainColumnHierarchyPayload &payload, std::string *error)
{
  payload = {};
  if (error != nullptr)
    error->clear();
  if (!validateWorldMapTerrainColumns(page, error))
    return false;
  if (maximumBricks == 0u || (page.side & 1u) != 0u || page.side > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) || page.maximumVerticalVoxels > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      std::abs(page.edgeMetres - page.voxelSizeMetres * page.side) > std::max(1.0e-6, page.edgeMetres * 1.0e-6))
    return fail(error, "Direct terrain hierarchy requires an even, integral voxel page");

  std::vector<std::vector<TerrainBrick>> levels(1u);
  auto &fine = levels.front();
  std::unordered_map<BrickKey, uint32_t, BrickKeyHash> fineLookup;
  const int32_t halfSide = static_cast<int32_t>(page.side / 2u);
  const int32_t brickSide = static_cast<int32_t>(kSparseVoxelBrickSide);
  for (uint32_t z = 0u; z < page.side; ++z)
    for (uint32_t x = 0u; x < page.side; ++x)
    {
      const size_t columnIndex = static_cast<size_t>(z) * page.side + x;
      const auto &column = page.columns[columnIndex];
      const auto &wall = page.walls[columnIndex];
      payload.occupiedFineVoxelCount += uint64_t{column.topY} - column.firstSolidY + 1u;
      const int32_t gridX = static_cast<int32_t>(x) - halfSide;
      const int32_t gridZ = static_cast<int32_t>(z) - halfSide;
      for (uint32_t y = column.firstSolidY; y <= column.topY; ++y)
      {
        const BrickKey key{
          floorDivide(gridX, brickSide),
          floorDivide(static_cast<int32_t>(y), brickSide),
          floorDivide(gridZ, brickSide),
        };
        auto [found, inserted] = fineLookup.emplace(key, static_cast<uint32_t>(fine.size()));
        if (inserted)
        {
          fine.emplace_back();
          fine.back().key = key;
          if (fine.size() > maximumBricks)
            return fail(error, "Direct terrain hierarchy exceeded the brick budget");
        }
        const uint32_t cell =
            cellIndex(static_cast<uint32_t>(positiveModulo(gridX, brickSide)), static_cast<uint32_t>(positiveModulo(static_cast<int32_t>(y), brickSide)), static_cast<uint32_t>(positiveModulo(gridZ, brickSide)));
        setFineCell(fine[found->second], cell, columnCell(column, wall, y));
      }
    }
  std::sort(fine.begin(), fine.end(), brickLess());
  payload.fineBrickCount = static_cast<uint32_t>(fine.size());
  payload.peakTemporaryBrickBytes = uint64_t{fine.capacity()} * sizeof(TerrainBrick);

  while (levels.back().size() > 1u)
  {
    const auto &finer = levels.back();
    std::vector<TerrainBrick> coarser;
    std::unordered_map<BrickKey, uint32_t, BrickKeyHash> lookup;
    lookup.reserve(finer.size());
    for (const TerrainBrick &child : finer)
    {
      const BrickKey parentKey{floorDivide(child.key[0], 2), floorDivide(child.key[1], 2), floorDivide(child.key[2], 2)};
      auto [found, inserted] = lookup.emplace(parentKey, static_cast<uint32_t>(coarser.size()));
      if (inserted)
      {
        coarser.emplace_back();
        coarser.back().key = parentKey;
      }
      TerrainBrick &parent = coarser[found->second];
      const uint32_t offsetX = static_cast<uint32_t>(positiveModulo(child.key[0], 2)) * 4u;
      const uint32_t offsetY = static_cast<uint32_t>(positiveModulo(child.key[1], 2)) * 4u;
      const uint32_t offsetZ = static_cast<uint32_t>(positiveModulo(child.key[2], 2)) * 4u;
      for (uint32_t cell = 0u; cell < kSparseVoxelBrickVoxelCount; ++cell)
      {
        if (!occupied(child, cell))
          continue;
        const uint32_t x = cell & 7u;
        const uint32_t y = (cell >> 3u) & 7u;
        const uint32_t z = cell >> 6u;
        setParentCell(parent, cellIndex(offsetX + x / 2u, offsetY + y / 2u, offsetZ + z / 2u), child.cells[cell]);
      }
    }
    if (coarser.size() >= finer.size())
      break;
    std::sort(coarser.begin(), coarser.end(), brickLess());
    const float finerVoxelSize = static_cast<float>(std::ldexp(page.voxelSizeMetres, static_cast<int>(levels.size() - 1u)));
    for (TerrainBrick &parent : coarser)
    {
      float worstDeviation = 0.0f;
      for (uint32_t octant = 0u; octant < 8u; ++octant)
      {
        const BrickKey childKey{
          parent.key[0] * 2 + static_cast<int32_t>(octant & 1u),
          parent.key[1] * 2 + static_cast<int32_t>((octant >> 1u) & 1u),
          parent.key[2] * 2 + static_cast<int32_t>((octant >> 2u) & 1u),
        };
        const TerrainBrick *child = findBrick(finer, childKey);
        if (child != nullptr)
          worstDeviation = std::max(worstDeviation, child->selfError);
      }
      for (uint32_t cell = 0u; cell < kSparseVoxelBrickVoxelCount; ++cell)
      {
        if (!occupied(parent, cell))
          continue;
        const uint32_t x = cell & 7u;
        const uint32_t y = (cell >> 3u) & 7u;
        const uint32_t z = cell >> 6u;
        const BrickKey childKey{
          parent.key[0] * 2 + static_cast<int32_t>(x / 4u),
          parent.key[1] * 2 + static_cast<int32_t>(y / 4u),
          parent.key[2] * 2 + static_cast<int32_t>(z / 4u),
        };
        const TerrainBrick *child = findBrick(finer, childKey);
        if (child == nullptr)
          continue;
        const uint32_t baseX = (x % 4u) * 2u;
        const uint32_t baseY = (y % 4u) * 2u;
        const uint32_t baseZ = (z % 4u) * 2u;
        uint32_t lowest = 3u;
        uint32_t highest = 0u;
        for (uint32_t subZ = 0u; subZ < 2u; ++subZ)
          for (uint32_t subX = 0u; subX < 2u; ++subX)
          {
            uint32_t solid = 0u;
            for (uint32_t subY = 0u; subY < 2u; ++subY)
              solid += occupied(*child, cellIndex(baseX + subX, baseY + subY, baseZ + subZ));
            lowest = std::min(lowest, solid);
            highest = std::max(highest, solid);
          }
        worstDeviation = std::max(worstDeviation, static_cast<float>(highest - lowest) * finerVoxelSize);
      }
      parent.selfError = worstDeviation;
    }
    uint64_t allocatedBrickRecords = coarser.capacity();
    for (const auto &residentLevel : levels)
      allocatedBrickRecords += residentLevel.capacity();
    payload.peakTemporaryBrickBytes = std::max(payload.peakTemporaryBrickBytes, allocatedBrickRecords * sizeof(TerrainBrick));
    levels.push_back(std::move(coarser));
  }

  auto &hierarchy = payload.hierarchy;
  hierarchy.paletteStride = 256u;
  hierarchy.paletteInstanceCount = 1u;
  hierarchy.paletteExactInstances = 1u;
  hierarchy.palette.resize(hierarchy.paletteStride);
  hierarchy.paletteDescriptorIds.resize(hierarchy.paletteStride, 0u);
  std::vector<std::pair<std::array<uint32_t, 2>, uint8_t>> paletteRecords;
  paletteRecords.reserve(page.columns.size());
  for (const auto &column : page.columns)
  {
    const uint8_t materialId = worldMapTerrainColumnMaterial(column);
    const auto packed = packSparseVoxelBrickMaterial(planetVoxelBaseMaterial(materialId));
    paletteRecords.push_back({packed.packed, materialId});
  }
  std::sort(
      paletteRecords.begin(),
      paletteRecords.end(),
      [](const auto &left, const auto &right)
      {
        return left.first < right.first;
      });
  paletteRecords.erase(
      std::unique(
          paletteRecords.begin(),
          paletteRecords.end(),
          [](const auto &left, const auto &right)
          {
            return left.first == right.first;
          }),
      paletteRecords.end());
  if (paletteRecords.empty() || paletteRecords.size() > hierarchy.paletteStride)
    return fail(error, "Direct terrain hierarchy palette is invalid");
  std::array<uint8_t, 256u> materialPalette{};
  for (uint32_t entry = 0u; entry < paletteRecords.size(); ++entry)
  {
    hierarchy.palette[entry].packed = paletteRecords[entry].first;
    for (const auto &column : page.columns)
      if (packSparseVoxelBrickMaterial(planetVoxelBaseMaterial(worldMapTerrainColumnMaterial(column))).packed == paletteRecords[entry].first)
        materialPalette[worldMapTerrainColumnMaterial(column)] = static_cast<uint8_t>(entry);
  }

  payload.surfaceMaterials.resize(page.columns.size() * 2u);
  for (size_t index = 0u; index < page.columns.size(); ++index)
  {
    const auto packed = packSparseVoxelBrickMaterial(planetVoxelBaseMaterial(worldMapTerrainColumnMaterial(page.columns[index])));
    payload.surfaceMaterials[index * 2u] = packed.packed[0];
    payload.surfaceMaterials[index * 2u + 1u] = packed.packed[1];
  }

  std::vector<uint32_t> levelBases(levels.size(), 0u);
  for (uint32_t levelIndex = 0u; levelIndex < levels.size(); ++levelIndex)
  {
    if (levelIndex >= 31u)
      return fail(error, "Direct terrain hierarchy canonical scale exceeds 32 bits");
    levelBases[levelIndex] = static_cast<uint32_t>(hierarchy.nodes.size());
    const float levelVoxelSize = static_cast<float>(std::ldexp(page.voxelSizeMetres, static_cast<int>(levelIndex)));
    const int64_t finestScale = int64_t{1} << levelIndex;
    for (const TerrainBrick &brick : levels[levelIndex])
    {
      SparseVoxelBrickNodeGPU node{};
      node.minimumVoxelSize = {
        static_cast<float>(brick.key[0] * brickSide) * levelVoxelSize,
        static_cast<float>(brick.key[1] * brickSide) * levelVoxelSize,
        static_cast<float>(brick.key[2] * brickSide) * levelVoxelSize,
        levelVoxelSize,
      };
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const int64_t origin = static_cast<int64_t>(brick.key[axis]) * brickSide * finestScale;
        if (origin < std::numeric_limits<int32_t>::min() || origin > std::numeric_limits<int32_t>::max())
          return fail(error, "Direct terrain hierarchy origin exceeds 32 bits");
        node.canonicalGridOriginScale[axis] = static_cast<int32_t>(origin);
      }
      node.canonicalGridOriginScale[3] = static_cast<int32_t>(finestScale);
      node.level = levelIndex;
      node.selfError = std::max(brick.selfError, levelVoxelSize * kSparseVoxelBrickErrorFloor);
      node.occupancyIndex = static_cast<uint32_t>(hierarchy.occupancy.size());
      node.materialBase = static_cast<uint32_t>(hierarchy.materialIndices.size());

      std::array<uint32_t, 3> minimum{7u, 7u, 7u};
      std::array<uint32_t, 3> maximum{0u, 0u, 0u};
      SparseVoxelBrickOccupancyGPU occupancyRecord{};
      uint32_t prefix = 0u;
      for (uint32_t word = 0u; word < kSparseVoxelBrickOccupancyWords; ++word)
      {
        occupancyRecord.cells[word] = {brick.words[word], prefix};
        prefix += static_cast<uint32_t>(__builtin_popcount(brick.words[word]));
      }
      node.occupiedCount = prefix;
      hierarchy.occupancy.push_back(occupancyRecord);
      uint64_t coarse = 0u;
      for (uint32_t cell = 0u; cell < kSparseVoxelBrickVoxelCount; ++cell)
      {
        if (!occupied(brick, cell))
          continue;
        const uint32_t x = cell & 7u;
        const uint32_t y = (cell >> 3u) & 7u;
        const uint32_t z = cell >> 6u;
        minimum[0] = std::min(minimum[0], x);
        minimum[1] = std::min(minimum[1], y);
        minimum[2] = std::min(minimum[2], z);
        maximum[0] = std::max(maximum[0], x);
        maximum[1] = std::max(maximum[1], y);
        maximum[2] = std::max(maximum[2], z);
        coarse |= uint64_t{1u} << (x / 2u + 4u * (y / 2u + 4u * (z / 2u)));
        const uint16_t materialContext = brick.cells[cell];
        hierarchy.materialIndices.push_back(materialPalette[materialContext & 0xffu]);
        hierarchy.microSurfaceData.push_back(packSparseVoxelMicroContextData(materialContext >> 8u, 0u));
      }
      node.packedOccupiedBounds = minimum[0] | (minimum[1] << 3u) | (minimum[2] << 6u) | (maximum[0] << 9u) | (maximum[1] << 12u) | (maximum[2] << 15u);
      node.coarseOccupancyLow = static_cast<uint32_t>(coarse);
      node.coarseOccupancyHigh = static_cast<uint32_t>(coarse >> 32u);
      if (levelIndex > 0u)
      {
        node.firstChild = static_cast<uint32_t>(hierarchy.childIndices.size());
        for (uint32_t octant = 0u; octant < 8u; ++octant)
        {
          const BrickKey childKey{
            brick.key[0] * 2 + static_cast<int32_t>(octant & 1u),
            brick.key[1] * 2 + static_cast<int32_t>((octant >> 1u) & 1u),
            brick.key[2] * 2 + static_cast<int32_t>((octant >> 2u) & 1u),
          };
          const uint32_t childOffset = findBrickIndex(levels[levelIndex - 1u], childKey);
          if (childOffset == kSparseVoxelBrickInvalidIndex)
            continue;
          const uint32_t childIndex = levelBases[levelIndex - 1u] + childOffset;
          hierarchy.childIndices.push_back(childIndex);
          hierarchy.nodes[childIndex].parentIndex = static_cast<uint32_t>(hierarchy.nodes.size());
          hierarchy.nodes[childIndex].parentError = node.selfError;
          ++node.childCount;
        }
      }
      hierarchy.nodes.push_back(node);
    }
  }
  hierarchy.paletteEmittedVoxels = hierarchy.materialIndices.size();
  hierarchy.levelCount = static_cast<uint32_t>(levels.size());
  hierarchy.finestVoxelSize = static_cast<float>(page.voxelSizeMetres);
  const uint32_t rootBase = levelBases.back();
  for (uint32_t root = 0u; root < levels.back().size(); ++root)
    hierarchy.rootIndices.push_back(rootBase + root);
  hierarchy.rootCount = static_cast<uint32_t>(hierarchy.rootIndices.size());
  if (hierarchy.nodes.empty())
    return fail(error, "Direct terrain hierarchy produced no nodes");
  return true;
}

} // namespace voxel::svraster
