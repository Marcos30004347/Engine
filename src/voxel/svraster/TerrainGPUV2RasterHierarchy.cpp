#include "voxel/svraster/TerrainGPUV2RasterHierarchy.hpp"

#include <algorithm>
#include <string>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

float floatFromBits(const uint32_t bits)
{
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

int32_t resolvedChunkMinimumCanonicalY(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    const TerrainGPUV2FineRasterResultGPU &rasterResult)
{
  if (rasterResult.reserved == 0u || config.canonicalStep == 0u ||
      !(config.voxelSizeMetres > 0.0f))
    return config.chunkMinimumCanonicalY;
  const float worldY = floatFromBits(rasterResult.reserved);
  if (!std::isfinite(worldY))
    return config.chunkMinimumCanonicalY;
  const float finestVoxelSize =
      config.voxelSizeMetres / static_cast<float>(config.canonicalStep);
  return static_cast<int32_t>(std::lround(worldY / finestVoxelSize));
}

float resolvedChunkMinimumWorldY(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    const TerrainGPUV2FineRasterResultGPU &rasterResult)
{
  if (rasterResult.reserved == 0u)
    return config.chunkMinimumWorldY;
  const float worldY = floatFromBits(rasterResult.reserved);
  return std::isfinite(worldY) ? worldY : config.chunkMinimumWorldY;
}

bool appendWords(const uint64_t words, uint64_t &cursor, uint32_t &base)
{
  base = static_cast<uint32_t>(cursor);
  cursor += words;
  return cursor <= std::numeric_limits<uint32_t>::max();
}

uint32_t denseIndex(const uint32_t x, const uint32_t y, const uint32_t z, const uint32_t axesY, const uint32_t axesZ)
{
  return x * axesY * axesZ + y * axesZ + z;
}

std::array<uint32_t, 3u> denseCoordinate(const uint32_t index, const uint32_t axesY, const uint32_t axesZ)
{
  const uint32_t yz = axesY * axesZ;
  const uint32_t x = index / yz;
  const uint32_t remainder = index - x * yz;
  const uint32_t y = remainder / axesZ;
  return {x, y, remainder - y * axesZ};
}

uint32_t clipmapLOD(uint32_t step)
{
  uint32_t lod = 0u;
  while (step > 1u)
  {
    step >>= 1u;
    ++lod;
  }
  return lod;
}

uint8_t materialByte(const std::vector<uint32_t> &words, const uint32_t address)
{
  return (address >> 2u) < words.size() ? static_cast<uint8_t>((words[address >> 2u] >> ((address & 3u) * 8u)) & 0xffu) : 0u;
}

void setMaterialByte(std::vector<uint32_t> &words, const uint32_t address, const uint8_t material)
{
  const uint32_t shift = (address & 3u) * 8u;
  words[address >> 2u] = (words[address >> 2u] & ~(0xffu << shift)) | (uint32_t{material} << shift);
}

bool occupiedMaterial(
    const TerrainGPUV2RasterHierarchyOutput &output,
    const uint32_t nodeIndex,
    const uint32_t cell,
    uint8_t &material)
{
  if (nodeIndex >= output.nodes.size() || cell >= kSparseVoxelBrickVoxelCount)
    return false;
  const auto &node = output.nodes[nodeIndex];
  if (node.occupancyIndex >= output.occupancy.size())
    return false;
  const uint32_t word = cell >> 5u;
  const uint32_t bit = cell & 31u;
  const auto &occupancyWord = output.occupancy[node.occupancyIndex].cells[word];
  if ((occupancyWord.bits & (1u << bit)) == 0u)
    return false;
  const uint32_t before = occupancyWord.bits & ((bit == 0u) ? 0u : ((1u << bit) - 1u));
  const uint32_t address = node.materialBase + occupancyWord.prefix + static_cast<uint32_t>(__builtin_popcount(before));
  material = materialByte(output.materialWords, address);
  return true;
}

uint8_t dominantMaterial(const std::array<uint8_t, 8u> &materials, const uint32_t count)
{
  uint8_t selected = 0u;
  uint32_t selectedCount = 0u;
  for (uint32_t index = 0u; index < count; ++index)
  {
    uint32_t frequency = 0u;
    for (uint32_t other = 0u; other < count; ++other)
      frequency += materials[index] == materials[other] ? 1u : 0u;
    if (frequency > selectedCount || (frequency == selectedCount && materials[index] < selected))
    {
      selected = materials[index];
      selectedCount = frequency;
    }
  }
  return selected;
}

float terrainRepresentativeCellDeviationMetres(
    const TerrainGPUV2RasterHierarchyOutput &output,
    const std::vector<uint32_t> &mapping,
    const uint32_t previousOffset,
    const uint32_t previousAxesX,
    const uint32_t previousAxesY,
    const uint32_t previousAxesZ,
    const std::array<uint32_t, 3u> &parentCoordinate,
    const uint32_t cell,
    const float finerVoxelSizeMetres)
{
  const uint32_t px = cell & 7u;
  const uint32_t py = (cell >> 3u) & 7u;
  const uint32_t pz = cell >> 6u;
  std::array<uint32_t, 4u> solidSubcolumnSamples{};
  std::array<uint8_t, 8u> materials{};
  uint32_t materialCount = 0u;
  uint32_t validSamples = 0u;
  uint32_t occupiedSamples = 0u;

  for (uint32_t sub = 0u; sub < 8u; ++sub)
  {
    const uint32_t gx = px * 2u + (sub & 1u);
    const uint32_t gy = py * 2u + ((sub >> 1u) & 1u);
    const uint32_t gz = pz * 2u + ((sub >> 2u) & 1u);
    const uint32_t childX = parentCoordinate[0] * 2u + gx / 8u;
    const uint32_t childY = parentCoordinate[1] * 2u + gy / 8u;
    const uint32_t childZ = parentCoordinate[2] * 2u + gz / 8u;
    if (childX >= previousAxesX || childY >= previousAxesY || childZ >= previousAxesZ)
      continue;
    ++validSamples;
    const uint32_t child = mapping[previousOffset + denseIndex(childX, childY, childZ, previousAxesY, previousAxesZ)];
    if (child == kSparseVoxelBrickInvalidIndex)
      continue;
    const uint32_t childCell = (gx & 7u) + 8u * ((gy & 7u) + 8u * (gz & 7u));
    uint8_t material = 0u;
    if (!occupiedMaterial(output, child, childCell, material))
      continue;
    ++occupiedSamples;
    solidSubcolumnSamples[(gx & 1u) + 2u * (gz & 1u)] += 1u;
    materials[materialCount++] = material;
  }
  if (occupiedSamples == 0u)
    return 0.0f;

  uint32_t lowest = solidSubcolumnSamples[0];
  uint32_t highest = solidSubcolumnSamples[0];
  for (uint32_t subcolumn = 1u; subcolumn < solidSubcolumnSamples.size(); ++subcolumn)
  {
    lowest = std::min(lowest, solidSubcolumnSamples[subcolumn]);
    highest = std::max(highest, solidSubcolumnSamples[subcolumn]);
  }
  float deviation =
      static_cast<float>(highest - lowest) * finerVoxelSizeMetres;

  // If the parent representative changes occupancy inside this cell, keep a
  // small conservative silhouette/coverage penalty. This is intentionally
  // weaker than the old "any mixed => half the coarse voxel" proxy; cliffs and
  // steep height changes are measured above, while flat partial coverage stays
  // close to the floor.
  if (validSamples != 0u && occupiedSamples != validSamples)
    deviation = std::max(deviation, finerVoxelSizeMetres * 0.5f);

  if (materialCount > 1u)
  {
    const uint8_t selected = dominantMaterial(materials, materialCount);
    const bool materialDiverged =
        std::any_of(
            materials.begin(),
            materials.begin() + materialCount,
            [selected](const uint8_t material)
            {
              return material != selected;
            });
    if (materialDiverged)
      deviation = std::max(deviation, finerVoxelSizeMetres * 0.25f);
  }
  return deviation;
}

uint32_t hierarchyHeightSampleHalo(const TerrainGPUV2RasterHierarchyConfigGPU &config)
{
  return config.reserved & 0xffffu;
}

uint32_t hierarchyHeightSampleStride(const TerrainGPUV2RasterHierarchyConfigGPU &config)
{
  return config.reserved >> 16u;
}

float terrainRepresentativeSurfaceDeviationMetres(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    const std::vector<TerrainGPUV2HeightSampleGPU> &heightSamples,
    const int32_t resolvedMinimumCanonicalY,
    const std::array<uint32_t, 3u> &parentCoordinate,
    const uint32_t cell,
    const uint32_t level)
{
  const uint32_t halo = hierarchyHeightSampleHalo(config);
  const uint32_t stride = hierarchyHeightSampleStride(config);
  const uint32_t sampleZ =
      stride == 0u ? 0u : config.bricksZ * config.brickSide + 2u * halo + 1u;
  if (heightSamples.empty() || stride == 0u || halo == 0u ||
      heightSamples.size() < uint64_t{stride} * sampleZ ||
      config.canonicalStep == 0u || !(config.voxelSizeMetres > 0.0f))
    return 0.0f;

  const uint32_t scale = 1u << level;
  const uint32_t px = cell & 7u;
  const uint32_t py = (cell >> 3u) & 7u;
  const uint32_t pz = cell >> 6u;
  const uint32_t localX =
      (parentCoordinate[0] * config.brickSide + px) * scale;
  const uint32_t localY =
      (parentCoordinate[1] * config.brickSide + py) * scale;
  const uint32_t localZ =
      (parentCoordinate[2] * config.brickSide + pz) * scale;
  if (localX > config.bricksX * config.brickSide ||
      localZ > config.bricksZ * config.brickSide)
    return 0.0f;

  const double heightUnitMetres =
      static_cast<double>(config.voxelSizeMetres) /
      static_cast<double>(config.canonicalStep);
  const double cellTopCanonical =
      static_cast<double>(resolvedMinimumCanonicalY) +
      static_cast<double>((localY + scale) * config.canonicalStep);
  const double cellBottomCanonical =
      static_cast<double>(resolvedMinimumCanonicalY) +
      static_cast<double>(localY * config.canonicalStep);
  float deviation = 0.0f;
  bool crossedSurface = false;

  // Compare against the pre-voxel terrain surface at a deterministic 3x3
  // lattice in the represented XZ footprint: four corners, edge midpoints, and
  // centre. The old occupancy-derived term remains below as conservative side
  // closure, while this term tracks the actual terrain height before the
  // half-open voxel shell snapped it to a cell.
  for (uint32_t iz = 0u; iz < 3u; ++iz)
  {
    const uint32_t offsetZ = (iz * scale + 1u) / 2u;
    for (uint32_t ix = 0u; ix < 3u; ++ix)
    {
      const uint32_t offsetX = (ix * scale + 1u) / 2u;
      const uint32_t sampleX = localX + std::min(offsetX, scale);
      const uint32_t sampleZIndex = localZ + std::min(offsetZ, scale);
      const uint64_t address =
          uint64_t{sampleZIndex + halo} * stride + sampleX + halo;
      if (address >= heightSamples.size())
        continue;
      const auto &sample = heightSamples[address];
      if ((terrainGPUV2HeightSampleFlags(sample) &
              TerrainGPUV2ColumnValid) == 0u)
        continue;
      const double heightMetres =
          terrainGPUV2HeightSampleMetres(sample, heightUnitMetres);
      const double heightCanonical = heightMetres / heightUnitMetres;
      if (heightCanonical > cellBottomCanonical &&
          heightCanonical <= cellTopCanonical)
      {
        crossedSurface = true;
        deviation = std::max(deviation,
            static_cast<float>(
                std::abs(cellTopCanonical - heightCanonical) *
                heightUnitMetres));
      }
    }
  }
  return crossedSurface ? deviation : 0.0f;
}

uint32_t packedBounds(const SparseVoxelBrickOccupancyGPU &occupancy)
{
  std::array<uint32_t, 3u> minimum{7u, 7u, 7u};
  std::array<uint32_t, 3u> maximum{};
  bool any = false;
  for (uint32_t cell = 0u; cell < 512u; ++cell)
    if ((occupancy.cells[cell >> 5u].bits & (1u << (cell & 31u))) != 0u)
    {
      const std::array<uint32_t, 3u> coordinate{cell & 7u, (cell >> 3u) & 7u, cell >> 6u};
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        minimum[axis] = std::min(minimum[axis], coordinate[axis]);
        maximum[axis] = std::max(maximum[axis], coordinate[axis]);
      }
      any = true;
    }
  if (!any)
    return 0u;
  return minimum[0] | (minimum[1] << 3u) | (minimum[2] << 6u) | (maximum[0] << 9u) | (maximum[1] << 12u) | (maximum[2] << 15u);
}

std::pair<uint32_t, uint32_t> coarseMask(const SparseVoxelBrickOccupancyGPU &occupancy)
{
  uint64_t mask = 0u;
  for (uint32_t cell = 0u; cell < 512u; ++cell)
    if ((occupancy.cells[cell >> 5u].bits & (1u << (cell & 31u))) != 0u)
    {
      const uint32_t coarse = ((cell & 7u) >> 1u) + 4u * (((cell >> 3u) & 7u) >> 1u) + 16u * ((cell >> 6u) >> 1u);
      mask |= uint64_t{1u} << coarse;
    }
  return {static_cast<uint32_t>(mask), static_cast<uint32_t>(mask >> 32u)};
}

bool configValid(const TerrainGPUV2RasterHierarchyConfigGPU &config, std::string *error)
{
  if (config.brickSide != kSparseVoxelBrickSide || config.levelCount < 2u || config.levelCount > kTerrainGPUV2RasterHierarchyMaximumLevels || config.maximumParentCandidates == 0u ||
      config.totalDenseCandidates == 0u || config.leafNodeCapacity == 0u || config.leafMaterialCapacity == 0u || config.nodeCapacity < config.leafNodeCapacity ||
      config.materialCapacity < config.leafMaterialCapacity || config.childCapacity == 0u || config.rootCapacity == 0u || config.canonicalStep == 0u || config.regionMappingId == 0u ||
      config.generatorRevision != kTerrainGPUV2RasterHierarchyGeneratorRevision || config.selectedMode != static_cast<uint32_t>(TerrainGPUV2RasterMode::Hierarchy))
    return fail(error, "terrain GPU V2 raster hierarchy config header is invalid");
  uint64_t denseTotal = 0u;
  for (uint32_t level = 0u; level < config.levelCount; ++level)
  {
    const uint64_t count = uint64_t{config.levelAxesX[level]} * config.levelAxesY[level] * config.levelAxesZ[level];
    if (count == 0u || count != config.levelDenseCounts[level] || config.levelDenseOffsets[level] != denseTotal)
      return fail(error, "terrain GPU V2 raster hierarchy level layout is invalid");
    denseTotal += count;
    if (level != 0u && (config.levelAxesX[level] != (config.levelAxesX[level - 1u] + 1u) / 2u || config.levelAxesY[level] != (config.levelAxesY[level - 1u] + 1u) / 2u ||
                            config.levelAxesZ[level] != (config.levelAxesZ[level - 1u] + 1u) / 2u))
      return fail(error, "terrain GPU V2 raster hierarchy parent axes are invalid");
  }
  TerrainGPUV2RasterHierarchyScratchLayout scratch{};
  if (denseTotal != config.totalDenseCandidates || !terrainGPUV2RasterHierarchyScratchLayout(config.maximumParentCandidates, config.totalDenseCandidates, config.levelCount, scratch, error) ||
      config.summaryBase != scratch.summaryBase || config.countBase != scratch.countBase || config.offsetBase != scratch.offsetBase || config.level0SumsBase != scratch.level0SumsBase ||
      config.level0OffsetsBase != scratch.level0OffsetsBase || config.level1SumsBase != scratch.level1SumsBase || config.level1OffsetsBase != scratch.level1OffsetsBase ||
      config.mappingBase != scratch.mappingBase || config.levelStatsBase != scratch.levelStatsBase || config.scratchWords != scratch.totalWords)
    return fail(error, "terrain GPU V2 raster hierarchy scratch layout is invalid");
  return true;
}
} // namespace

bool terrainGPUV2RasterHierarchyScratchLayout(
    const uint32_t maximumParentCandidates,
    const uint32_t totalDenseCandidates,
    const uint32_t levelCount,
    TerrainGPUV2RasterHierarchyScratchLayout &layout,
    std::string *error)
{
  layout = {};
  if (maximumParentCandidates == 0u || totalDenseCandidates == 0u || levelCount < 2u || levelCount > kTerrainGPUV2RasterHierarchyMaximumLevels)
    return fail(error, "terrain GPU V2 raster hierarchy scratch dimensions are invalid");
  const uint64_t level0 = (uint64_t{maximumParentCandidates} + 255u) / 256u;
  const uint64_t level1 = (level0 + 255u) / 256u;
  uint64_t cursor = 0u;
  // Sixteen independent 32-cell classifiers write occupancy/count/mixed plus
  // pre-reduced bounds and coarse occupancy.  Emit therefore consumes sixteen
  // summaries instead of walking all 512 cells on one invocation.
  if (!appendWords(uint64_t{maximumParentCandidates} * 16u * 8u, cursor, layout.summaryBase) || !appendWords(uint64_t{maximumParentCandidates} * 4u, cursor, layout.countBase) ||
      !appendWords(uint64_t{maximumParentCandidates} * 4u, cursor, layout.offsetBase) || !appendWords(level0 * 4u, cursor, layout.level0SumsBase) ||
      !appendWords(level0 * 4u, cursor, layout.level0OffsetsBase) || !appendWords(level1 * 4u, cursor, layout.level1SumsBase) ||
      !appendWords(level1 * 4u, cursor, layout.level1OffsetsBase) || !appendWords(totalDenseCandidates, cursor, layout.mappingBase) ||
      !appendWords(uint64_t{levelCount} * 4u, cursor, layout.levelStatsBase))
    return fail(error, "terrain GPU V2 raster hierarchy scratch exceeds the shader address domain");
  layout.totalWords = static_cast<uint32_t>(cursor);
  return true;
}

bool planTerrainGPUV2RasterHierarchy(
    const TerrainGPUV2FineRasterConfigGPU &raster,
    const TerrainGPUV2RasterHierarchySettings &settings,
    TerrainGPUV2RasterHierarchyConfigGPU &config,
    std::string *error)
{
  config = {};
  if (raster.brickSide != kSparseVoxelBrickSide || raster.bricksX == 0u || raster.bricksY == 0u || raster.bricksZ == 0u || raster.nodeCapacity == 0u || raster.materialCapacity == 0u ||
      raster.canonicalStep == 0u || raster.regionMappingId == 0u || settings.maximumLevels < 2u || settings.maximumLevels > kTerrainGPUV2RasterHierarchyMaximumLevels)
    return fail(error, "terrain GPU V2 raster hierarchy planner input is invalid");
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> axesX{};
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> axesY{};
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> axesZ{};
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> counts{};
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> offsets{};
  axesX[0] = raster.bricksX;
  axesY[0] = raster.bricksY;
  axesZ[0] = raster.bricksZ;
  uint32_t levels = 1u;
  uint64_t denseTotal = 0u;
  uint64_t parentTotal = 0u;
  uint64_t maximumParent = 0u;
  while (true)
  {
    const uint64_t count = uint64_t{axesX[levels - 1u]} * axesY[levels - 1u] * axesZ[levels - 1u];
    if (count > std::numeric_limits<uint32_t>::max())
      return fail(error, "terrain GPU V2 raster hierarchy dense level overflows");
    offsets[levels - 1u] = static_cast<uint32_t>(denseTotal);
    counts[levels - 1u] = static_cast<uint32_t>(count);
    denseTotal += count;
    if (levels > 1u)
    {
      parentTotal += count;
      maximumParent = std::max(maximumParent, count);
    }
    if ((axesX[levels - 1u] == 1u && axesY[levels - 1u] == 1u && axesZ[levels - 1u] == 1u) || levels == settings.maximumLevels)
      break;
    const uint32_t nextScale = 1u << levels;
    const uint64_t voxelsX = uint64_t{raster.bricksX} * raster.brickSide;
    const uint64_t voxelsY = uint64_t{raster.bricksY} * raster.brickSide;
    const uint64_t voxelsZ = uint64_t{raster.bricksZ} * raster.brickSide;
    // Stop before a coarse voxel would straddle the chunk's half-open maximum.
    // Multiple roots are preferable to an overlapping padded boundary voxel.
    if ((voxelsX % nextScale) != 0u || (voxelsY % nextScale) != 0u || (voxelsZ % nextScale) != 0u)
      break;
    axesX[levels] = (axesX[levels - 1u] + 1u) / 2u;
    axesY[levels] = (axesY[levels - 1u] + 1u) / 2u;
    axesZ[levels] = (axesZ[levels - 1u] + 1u) / 2u;
    ++levels;
  }
  if (levels < 2u || denseTotal > std::numeric_limits<uint32_t>::max() || maximumParent == 0u)
    return fail(error, "terrain GPU V2 raster hierarchy shape has no exact parent level");
  const uint64_t requiredNodes = uint64_t{raster.nodeCapacity} + parentTotal;
  // Parent material occupancy is surface proportional, not dense-volume
  // proportional. Reserving 512 entries for every possible dense parent made
  // a 128x128 height-field page require hundreds of thousands of entries even
  // though only its thin exterior shell is emitted. Keep the deterministic
  // output budget supplied by the page class and let the GPU's prefix sums
  // reject the exceptional page that actually exceeds it.
  const uint64_t requiredChildren = parentTotal * 8u;
  const uint64_t requiredRoots = counts[levels - 1u];
  if (requiredNodes > settings.capacity.nodes ||
      raster.materialCapacity > settings.capacity.materials ||
      requiredChildren > settings.capacity.children ||
      requiredRoots > settings.capacity.roots)
    return fail(
        error,
        "terrain GPU V2 raster hierarchy worst-case output budget is too small: nodes " +
            std::to_string(requiredNodes) + "/" + std::to_string(settings.capacity.nodes) +
            " materials " + std::to_string(raster.materialCapacity) + "/" + std::to_string(settings.capacity.materials) +
            " children " + std::to_string(requiredChildren) + "/" + std::to_string(settings.capacity.children) +
            " roots " + std::to_string(requiredRoots) + "/" + std::to_string(settings.capacity.roots) +
            " levels " + std::to_string(levels));
  TerrainGPUV2RasterHierarchyScratchLayout scratch{};
  if (!terrainGPUV2RasterHierarchyScratchLayout(static_cast<uint32_t>(maximumParent), static_cast<uint32_t>(denseTotal), levels, scratch, error))
    return false;

  config.bricksX = raster.bricksX;
  config.bricksY = raster.bricksY;
  config.bricksZ = raster.bricksZ;
  config.brickSide = raster.brickSide;
  config.levelCount = levels;
  config.totalDenseCandidates = static_cast<uint32_t>(denseTotal);
  config.maximumParentCandidates = static_cast<uint32_t>(maximumParent);
  config.expectedTerrainGeneration = raster.expectedTerrainGeneration;
  config.leafNodeCapacity = raster.nodeCapacity;
  config.leafMaterialCapacity = raster.materialCapacity;
  config.leafMaterialWordCapacity = raster.materialWordCapacity;
  config.nodeCapacity = settings.capacity.nodes;
  config.materialCapacity = settings.capacity.materials;
  config.materialWordCapacity = (settings.capacity.materials + 3u) / 4u;
  config.childCapacity = settings.capacity.children;
  config.rootCapacity = settings.capacity.roots;
  config.canonicalStep = raster.canonicalStep;
  config.chunkMinimumCanonicalX = raster.chunkMinimumCanonicalX;
  config.chunkMinimumCanonicalY = raster.chunkMinimumCanonicalY;
  config.chunkMinimumCanonicalZ = raster.chunkMinimumCanonicalZ;
  config.chunkMinimumWorldX = raster.chunkMinimumWorldX;
  config.chunkMinimumWorldY = raster.chunkMinimumWorldY;
  config.chunkMinimumWorldZ = raster.chunkMinimumWorldZ;
  config.voxelSizeMetres = raster.voxelSizeMetres;
  config.regionMappingId = raster.regionMappingId;
  config.paletteBase = raster.paletteBase;
  config.summaryBase = scratch.summaryBase;
  config.countBase = scratch.countBase;
  config.offsetBase = scratch.offsetBase;
  config.level0SumsBase = scratch.level0SumsBase;
  config.level0OffsetsBase = scratch.level0OffsetsBase;
  config.level1SumsBase = scratch.level1SumsBase;
  config.level1OffsetsBase = scratch.level1OffsetsBase;
  config.mappingBase = scratch.mappingBase;
  config.levelStatsBase = scratch.levelStatsBase;
  config.scratchWords = scratch.totalWords;
  const uint32_t heightSampleStride =
      raster.columnsX + 2u * raster.halo + 1u;
  if (raster.halo > 0xffffu || heightSampleStride > 0xffffu)
    return fail(error, "terrain GPU V2 raster hierarchy height sample layout exceeds packed config");
  config.reserved = raster.halo | (heightSampleStride << 16u);
  config.flags = TerrainGPUV2RasterHierarchyOccupancyOnly | TerrainGPUV2RasterHierarchyNoMicroGeometry | TerrainGPUV2RasterHierarchyMonotonicError | TerrainGPUV2RasterHierarchyExperimental;
  config.levelDenseOffsets = offsets;
  config.levelDenseCounts = counts;
  config.levelAxesX = axesX;
  config.levelAxesY = axesY;
  config.levelAxesZ = axesZ;
  return true;
}

bool terrainGPUV2RasterHierarchyMemory(const TerrainGPUV2RasterHierarchyConfigGPU &config, TerrainGPUV2RasterHierarchyMemory &memory, std::string *error)
{
  memory = {};
  if (!configValid(config, error))
    return false;
  memory.scratchBytes = uint64_t{config.scratchWords} * sizeof(uint32_t);
  memory.outputBytes = uint64_t{config.nodeCapacity} * (sizeof(SparseVoxelBrickNodeGPU) + sizeof(SparseVoxelBrickOccupancyGPU)) + uint64_t{config.materialWordCapacity} * sizeof(uint32_t) +
      uint64_t{config.childCapacity + config.rootCapacity} * sizeof(uint32_t) + sizeof(TerrainGPUV2RasterHierarchyResultGPU);
  return true;
}

TerrainGPUV2RasterMode selectTerrainGPUV2RasterMode(const TerrainGPUV2RasterHierarchyQualification &qualification)
{
  if (!qualification.experimentEnabled || !qualification.deviceQualified || qualification.minimumImprovementPermille >= 1000u || qualification.leafOnlyEndToEndP99Microseconds == 0u ||
      qualification.hierarchyEndToEndP99Microseconds == 0u)
    return TerrainGPUV2RasterMode::LeafOnly;
  const uint64_t hierarchy = uint64_t{qualification.hierarchyEndToEndP99Microseconds} * 1000u;
  const uint64_t required = uint64_t{qualification.leafOnlyEndToEndP99Microseconds} * (1000u - qualification.minimumImprovementPermille);
  return hierarchy <= required ? TerrainGPUV2RasterMode::Hierarchy : TerrainGPUV2RasterMode::LeafOnly;
}

TerrainGPUV2RasterHierarchyCaptureComparison compareTerrainGPUV2RasterHierarchyCaptures(
    const TerrainGPUV2RasterHierarchyCapture &leafOnly,
    const TerrainGPUV2RasterHierarchyCapture &hierarchy)
{
  TerrainGPUV2RasterHierarchyCaptureComparison comparison{};
  if (leafOnly.renderedNodesP99 != 0u && hierarchy.renderedNodesP99 < leafOnly.renderedNodesP99)
    comparison.renderNodeSavingsPermille = static_cast<uint32_t>(
        uint64_t{leafOnly.renderedNodesP99 - hierarchy.renderedNodesP99} * 1000u / leafOnly.renderedNodesP99);
  const auto clampSigned32 = [](const int64_t value)
  {
    return static_cast<int32_t>(std::clamp<int64_t>(value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
  };
  comparison.buildP99DeltaMicroseconds = clampSigned32(int64_t{hierarchy.buildP99Microseconds} - leafOnly.buildP99Microseconds);
  if (leafOnly.endToEndP99Microseconds != 0u)
    comparison.endToEndImprovementPermille = clampSigned32(
        (int64_t{leafOnly.endToEndP99Microseconds} - hierarchy.endToEndP99Microseconds) * 1000 / leafOnly.endToEndP99Microseconds);
  if (hierarchy.residentBytes >= leafOnly.residentBytes)
    comparison.residentByteDelta = static_cast<int64_t>(std::min<uint64_t>(hierarchy.residentBytes - leafOnly.residentBytes, std::numeric_limits<int64_t>::max()));
  else
    comparison.residentByteDelta = -static_cast<int64_t>(std::min<uint64_t>(leafOnly.residentBytes - hierarchy.residentBytes, uint64_t{std::numeric_limits<int64_t>::max()}));
  return comparison;
}

bool buildTerrainGPUV2RasterHierarchyOracle(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    const TerrainGPUV2FineRasterResultGPU &rasterResult,
    const std::vector<SparseVoxelBrickNodeGPU> &rasterNodes,
    const std::vector<SparseVoxelBrickOccupancyGPU> &rasterOccupancy,
    const std::vector<uint32_t> &rasterMaterialWords,
    const std::vector<TerrainGPUV2HeightSampleGPU> &heightSamples,
    TerrainGPUV2RasterHierarchyOutput &output,
    std::string *error)
{
  output = {};
  output.result.status = static_cast<uint32_t>(TerrainGPUV2RasterHierarchyStatus::Building);
  output.result.flags = config.flags;
  output.result.generatorRevision = kTerrainGPUV2RasterHierarchyGeneratorRevision;
  output.result.levelCount = config.levelCount;
  output.result.scratchWords = config.scratchWords;
  output.result.selectedMode = static_cast<uint32_t>(TerrainGPUV2RasterMode::Hierarchy);
  if (!configValid(config, error))
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2RasterHierarchyStatus::InvalidInput);
    return false;
  }
  if (rasterResult.status != static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::Ready) || rasterResult.terrainGeneration != config.expectedTerrainGeneration)
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2RasterHierarchyStatus::StaleRaster);
    return true;
  }
  if (rasterResult.nodeCount > config.leafNodeCapacity || rasterResult.materialCount > config.leafMaterialCapacity || rasterNodes.size() < rasterResult.nodeCount ||
      rasterOccupancy.size() < rasterResult.nodeCount || rasterMaterialWords.size() < (rasterResult.materialCount + 3u) / 4u)
    return fail(error, "terrain GPU V2 raster hierarchy leaf input is truncated");

  const int32_t resolvedMinimumCanonicalY =
      resolvedChunkMinimumCanonicalY(config, rasterResult);
  const float resolvedMinimumWorldY =
      resolvedChunkMinimumWorldY(config, rasterResult);
  output.nodes.assign(rasterNodes.begin(), rasterNodes.begin() + rasterResult.nodeCount);
  output.occupancy.assign(rasterOccupancy.begin(), rasterOccupancy.begin() + rasterResult.nodeCount);
  output.materialWords.assign((config.materialCapacity + 3u) / 4u, 0u);
  std::copy(rasterMaterialWords.begin(), rasterMaterialWords.begin() + (rasterResult.materialCount + 3u) / 4u, output.materialWords.begin());
  for (auto &node : output.nodes)
  {
    node.level &= ~kSparseVoxelBrickForceLeafRaster;
    node.parentIndex = kSparseVoxelBrickInvalidIndex;
    node.parentError = std::numeric_limits<float>::max();
  }
  std::vector<uint32_t> mapping(config.totalDenseCandidates, kSparseVoxelBrickInvalidIndex);
  for (uint32_t index = 0u; index < rasterResult.nodeCount; ++index)
  {
    const auto &node = output.nodes[index];
    if (node.occupancyIndex >= output.occupancy.size() || node.canonicalGridOriginScale[3] != static_cast<int32_t>(config.canonicalStep))
      return fail(error, "terrain GPU V2 raster hierarchy leaf canonical contract is invalid");
    std::array<uint32_t, 3u> coordinate{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t minimum = axis == 0u ? config.chunkMinimumCanonicalX : axis == 1u ? resolvedMinimumCanonicalY : config.chunkMinimumCanonicalZ;
      const int64_t delta = int64_t{node.canonicalGridOriginScale[axis]} - minimum;
      const int64_t divisor = int64_t{config.canonicalStep} * config.brickSide;
      if (delta < 0 || delta % divisor != 0)
        return fail(error, "terrain GPU V2 raster hierarchy leaf is off the brick lattice");
      coordinate[axis] = static_cast<uint32_t>(delta / divisor);
    }
    if (coordinate[0] >= config.bricksX || coordinate[1] >= config.bricksY || coordinate[2] >= config.bricksZ)
      return fail(error, "terrain GPU V2 raster hierarchy leaf coordinate is outside the chunk");
    mapping[denseIndex(coordinate[0], coordinate[1], coordinate[2], config.bricksY, config.bricksZ)] = index;
  }

  uint32_t materialCount = rasterResult.materialCount;
  for (uint32_t level = 1u; level < config.levelCount; ++level)
  {
    const uint32_t scale = 1u << level;
    const uint32_t previousOffset = config.levelDenseOffsets[level - 1u];
    const uint32_t currentOffset = config.levelDenseOffsets[level];
    for (uint32_t candidate = 0u; candidate < config.levelDenseCounts[level]; ++candidate)
    {
      const auto parentCoordinate = denseCoordinate(candidate, config.levelAxesY[level], config.levelAxesZ[level]);
      std::array<uint32_t, 8u> children{};
      uint32_t childCount = 0u;
      for (uint32_t octant = 0u; octant < 8u; ++octant)
      {
        const uint32_t x = parentCoordinate[0] * 2u + (octant & 1u);
        const uint32_t y = parentCoordinate[1] * 2u + ((octant >> 1u) & 1u);
        const uint32_t z = parentCoordinate[2] * 2u + ((octant >> 2u) & 1u);
        if (x >= config.levelAxesX[level - 1u] || y >= config.levelAxesY[level - 1u] || z >= config.levelAxesZ[level - 1u])
          continue;
        const uint32_t child = mapping[previousOffset + denseIndex(x, y, z, config.levelAxesY[level - 1u], config.levelAxesZ[level - 1u])];
        if (child != kSparseVoxelBrickInvalidIndex)
          children[childCount++] = child;
      }
      if (childCount == 0u)
        continue;
      if (output.nodes.size() >= config.nodeCapacity || uint64_t{output.childIndices.size()} + childCount > config.childCapacity)
        return fail(error, "terrain GPU V2 raster hierarchy violated its preflight node/child budget");
      SparseVoxelBrickOccupancyGPU parentOccupancy{};
      std::array<uint8_t, kSparseVoxelBrickVoxelCount> parentMaterials{};
      float representativeDeviation = 0.0f;
      uint32_t occupiedCount = 0u;
      const float finerVoxelSizeMetres =
          config.voxelSizeMetres * static_cast<float>(scale >> 1u);
      for (uint32_t cell = 0u; cell < 512u; ++cell)
      {
        const uint32_t px = cell & 7u;
        const uint32_t py = (cell >> 3u) & 7u;
        const uint32_t pz = cell >> 6u;
        std::array<uint8_t, 8u> samples{};
        uint32_t sampleCount = 0u;
        uint32_t validSamples = 0u;
        for (uint32_t sub = 0u; sub < 8u; ++sub)
        {
          const uint32_t gx = px * 2u + (sub & 1u);
          const uint32_t gy = py * 2u + ((sub >> 1u) & 1u);
          const uint32_t gz = pz * 2u + ((sub >> 2u) & 1u);
          const uint32_t childX = parentCoordinate[0] * 2u + gx / 8u;
          const uint32_t childY = parentCoordinate[1] * 2u + gy / 8u;
          const uint32_t childZ = parentCoordinate[2] * 2u + gz / 8u;
          if (childX >= config.levelAxesX[level - 1u] || childY >= config.levelAxesY[level - 1u] || childZ >= config.levelAxesZ[level - 1u])
            continue;
          ++validSamples;
          const uint32_t child = mapping[previousOffset + denseIndex(childX, childY, childZ, config.levelAxesY[level - 1u], config.levelAxesZ[level - 1u])];
          if (child == kSparseVoxelBrickInvalidIndex)
            continue;
          const uint32_t childCell = (gx & 7u) + 8u * ((gy & 7u) + 8u * (gz & 7u));
          uint8_t material = 0u;
          if (occupiedMaterial(output, child, childCell, material))
            samples[sampleCount++] = material;
        }
        if (sampleCount == 0u)
          continue;
        parentOccupancy.cells[cell >> 5u].bits |= 1u << (cell & 31u);
        parentMaterials[cell] = dominantMaterial(samples, sampleCount);
        representativeDeviation = std::max(
            representativeDeviation,
            std::max(
                terrainRepresentativeCellDeviationMetres(
                    output,
                    mapping,
                    previousOffset,
                    config.levelAxesX[level - 1u],
                    config.levelAxesY[level - 1u],
                    config.levelAxesZ[level - 1u],
                    parentCoordinate,
                    cell,
                    finerVoxelSizeMetres),
                terrainRepresentativeSurfaceDeviationMetres(
                    config,
                    heightSamples,
                    resolvedMinimumCanonicalY,
                    parentCoordinate,
                    cell,
                    level)));
        ++occupiedCount;
      }
      uint32_t prefix = 0u;
      for (auto &word : parentOccupancy.cells)
      {
        word.prefix = prefix;
        prefix += static_cast<uint32_t>(__builtin_popcount(word.bits));
      }
      if (prefix == 0u)
        continue;
      if (uint64_t{materialCount} + prefix > config.materialCapacity)
        return fail(error, "terrain GPU V2 raster hierarchy violated its preflight material budget");
      SparseVoxelBrickNodeGPU parent{};
      parent.minimumVoxelSize = {
        config.chunkMinimumWorldX + parentCoordinate[0] * 8u * scale * config.voxelSizeMetres,
        resolvedMinimumWorldY + parentCoordinate[1] * 8u * scale * config.voxelSizeMetres,
        config.chunkMinimumWorldZ + parentCoordinate[2] * 8u * scale * config.voxelSizeMetres,
        config.voxelSizeMetres * scale,
      };
      parent.packedOccupiedBounds = packedBounds(parentOccupancy);
      std::tie(parent.coarseOccupancyLow, parent.coarseOccupancyHigh) = coarseMask(parentOccupancy);
      parent.occupancyIndex = static_cast<uint32_t>(output.occupancy.size());
      parent.materialBase = materialCount;
      parent.parentIndex = kSparseVoxelBrickInvalidIndex;
      parent.childCount = childCount;
      parent.level = level | (clipmapLOD(config.canonicalStep) << 8u);
      parent.occupiedCount = occupiedCount;
      parent.firstChild = static_cast<uint32_t>(output.childIndices.size());
      parent.instanceIndex = kSparseVoxelBrickInvalidIndex;
      parent.paletteBase = config.paletteBase;
      parent.regionMappingId = config.regionMappingId;
      float childError = 0.0f;
      for (uint32_t child = 0u; child < childCount; ++child)
        childError = std::max(childError, output.nodes[children[child]].selfError);
      parent.selfError = std::max({childError, parent.minimumVoxelSize[3] * kSparseVoxelBrickErrorFloor, representativeDeviation});
      parent.parentError = std::numeric_limits<float>::max();
      parent.canonicalGridOriginScale = {
        config.chunkMinimumCanonicalX + static_cast<int32_t>(parentCoordinate[0] * 8u * scale * config.canonicalStep),
        resolvedMinimumCanonicalY + static_cast<int32_t>(parentCoordinate[1] * 8u * scale * config.canonicalStep),
        config.chunkMinimumCanonicalZ + static_cast<int32_t>(parentCoordinate[2] * 8u * scale * config.canonicalStep),
        static_cast<int32_t>(scale * config.canonicalStep),
      };
      const uint32_t parentIndex = static_cast<uint32_t>(output.nodes.size());
      for (uint32_t child = 0u; child < childCount; ++child)
      {
        output.childIndices.push_back(children[child]);
        output.nodes[children[child]].parentIndex = parentIndex;
        output.nodes[children[child]].parentError = parent.selfError;
      }
      output.nodes.push_back(parent);
      output.occupancy.push_back(parentOccupancy);
      for (uint32_t cell = 0u; cell < 512u; ++cell)
        if ((parentOccupancy.cells[cell >> 5u].bits & (1u << (cell & 31u))) != 0u)
          setMaterialByte(output.materialWords, materialCount++, parentMaterials[cell]);
      mapping[currentOffset + candidate] = parentIndex;
    }
  }
  const uint32_t rootOffset = config.levelDenseOffsets[config.levelCount - 1u];
  for (uint32_t candidate = 0u; candidate < config.levelDenseCounts[config.levelCount - 1u]; ++candidate)
    if (mapping[rootOffset + candidate] != kSparseVoxelBrickInvalidIndex)
      output.rootIndices.push_back(mapping[rootOffset + candidate]);
  output.materialWords.resize((materialCount + 3u) / 4u);
  output.result.status = static_cast<uint32_t>(TerrainGPUV2RasterHierarchyStatus::Ready);
  output.result.leafNodeCount = rasterResult.nodeCount;
  output.result.nodeCount = static_cast<uint32_t>(output.nodes.size());
  output.result.materialCount = materialCount;
  output.result.childCount = static_cast<uint32_t>(output.childIndices.size());
  output.result.rootCount = static_cast<uint32_t>(output.rootIndices.size());
  output.result.terrainGeneration = rasterResult.terrainGeneration;
  if (!validateTerrainGPUV2RasterHierarchy(config, output, error))
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2RasterHierarchyStatus::ValidationFailed);
    output.result.validationErrors = 1u;
  }
  return true;
}

bool buildTerrainGPUV2RasterHierarchyOracle(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    const TerrainGPUV2FineRasterResultGPU &rasterResult,
    const std::vector<SparseVoxelBrickNodeGPU> &rasterNodes,
    const std::vector<SparseVoxelBrickOccupancyGPU> &rasterOccupancy,
    const std::vector<uint32_t> &rasterMaterialWords,
    TerrainGPUV2RasterHierarchyOutput &output,
    std::string *error)
{
  static const std::vector<TerrainGPUV2HeightSampleGPU> noHeightSamples;
  return buildTerrainGPUV2RasterHierarchyOracle(config,
      rasterResult,
      rasterNodes,
      rasterOccupancy,
      rasterMaterialWords,
      noHeightSamples,
      output,
      error);
}

bool validateTerrainGPUV2RasterHierarchy(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    const TerrainGPUV2RasterHierarchyOutput &output,
    std::string *error)
{
  if (output.result.status != static_cast<uint32_t>(TerrainGPUV2RasterHierarchyStatus::Ready))
    return true;
  if (!configValid(config, error) || output.nodes.size() != output.result.nodeCount || output.occupancy.size() != output.nodes.size() || output.childIndices.size() != output.result.childCount ||
      output.rootIndices.size() != output.result.rootCount || output.materialWords.size() != (output.result.materialCount + 3u) / 4u || output.result.microSurfaceBytes != 0u ||
      output.result.flags != config.flags || output.result.selectedMode != static_cast<uint32_t>(TerrainGPUV2RasterMode::Hierarchy) || output.rootIndices.empty())
    return fail(error, "terrain GPU V2 raster hierarchy result contract is invalid");
  std::vector<uint8_t> reachable(output.nodes.size(), 0u);
  std::vector<uint32_t> pending = output.rootIndices;
  for (const uint32_t root : output.rootIndices)
    if (root >= output.nodes.size() || output.nodes[root].parentIndex != kSparseVoxelBrickInvalidIndex)
      return fail(error, "terrain GPU V2 raster hierarchy root is invalid");
  while (!pending.empty())
  {
    const uint32_t index = pending.back();
    pending.pop_back();
    if (index >= output.nodes.size() || reachable[index] != 0u)
      return fail(error, "terrain GPU V2 raster hierarchy has a cycle");
    reachable[index] = 1u;
    const auto &node = output.nodes[index];
    if (node.occupancyIndex >= output.occupancy.size() || uint64_t{node.materialBase} + node.occupiedCount > output.result.materialCount ||
        uint64_t{node.firstChild} + node.childCount > output.childIndices.size() || node.regionMappingId == 0u || (node.level & kSparseVoxelBrickForceLeafRaster) != 0u)
      return fail(error, "terrain GPU V2 raster hierarchy node range is invalid");
    uint32_t prefix = 0u;
    for (const auto &word : output.occupancy[node.occupancyIndex].cells)
    {
      if (word.prefix != prefix)
        return fail(error, "terrain GPU V2 raster hierarchy occupancy prefix is invalid");
      prefix += static_cast<uint32_t>(__builtin_popcount(word.bits));
    }
    if (prefix != node.occupiedCount)
      return fail(error, "terrain GPU V2 raster hierarchy occupied count is invalid");
    for (uint32_t child = 0u; child < node.childCount; ++child)
    {
      const uint32_t childIndex = output.childIndices[node.firstChild + child];
      if (childIndex >= output.nodes.size() || output.nodes[childIndex].parentIndex != index || (output.nodes[childIndex].level & 0xffu) + 1u != (node.level & 0xffu) ||
          output.nodes[childIndex].parentError != node.selfError || node.selfError < output.nodes[childIndex].selfError)
        return fail(error, "terrain GPU V2 raster hierarchy parent/child invariant is invalid");
      pending.push_back(childIndex);
    }
  }
  if (std::find(reachable.begin(), reachable.end(), uint8_t{0u}) != reachable.end())
    return fail(error, "terrain GPU V2 raster hierarchy contains unreachable nodes");
  return true;
}

std::vector<uint32_t> selectTerrainGPUV2RasterHierarchyCut(const TerrainGPUV2RasterHierarchyOutput &hierarchy, const float maximumWorldError)
{
  std::vector<uint32_t> selected;
  std::vector<uint32_t> pending = hierarchy.rootIndices;
  while (!pending.empty())
  {
    const uint32_t index = pending.back();
    pending.pop_back();
    if (index >= hierarchy.nodes.size())
      continue;
    const auto &node = hierarchy.nodes[index];
    if (node.childCount == 0u || (maximumWorldError >= 0.0f && node.selfError <= maximumWorldError))
    {
      selected.push_back(index);
      continue;
    }
    for (uint32_t child = node.childCount; child-- > 0u;)
      if (node.firstChild + child < hierarchy.childIndices.size())
        pending.push_back(hierarchy.childIndices[node.firstChild + child]);
  }
  std::sort(selected.begin(), selected.end());
  return selected;
}

} // namespace voxel::svraster
