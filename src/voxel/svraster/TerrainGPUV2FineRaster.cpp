#include "voxel/svraster/TerrainGPUV2FineRaster.hpp"

#include <algorithm>
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

uint32_t floatBits(const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

bool checkedAdd(const uint64_t left, const uint64_t right, uint64_t &value)
{
  value = left + right;
  return value >= left && value <= std::numeric_limits<uint32_t>::max();
}

uint32_t packCoordinate(const uint32_t x, const uint32_t y, const uint32_t z)
{
  return x | (y << 8u) | (z << 16u);
}

std::array<uint32_t, 3u> unpackCoordinate(const uint32_t packed)
{
  return {packed & 0xffu, (packed >> 8u) & 0xffu, (packed >> 16u) & 0xffu};
}

uint32_t clipmapLOD(uint32_t canonicalStep)
{
  uint32_t lod = 0u;
  while (canonicalStep > 1u)
  {
    canonicalStep >>= 1u;
    ++lod;
  }
  return lod;
}

int64_t floorDivide(const int64_t numerator, const int64_t denominator)
{
  const int64_t quotient = numerator / denominator;
  const int64_t remainder = numerator % denominator;
  return quotient - (remainder < 0 ? 1 : 0);
}

std::array<uint32_t, 3u> denseBrickCoordinate(const TerrainGPUV2FineRasterConfigGPU &config, const uint32_t index)
{
  const uint32_t yz = config.bricksY * config.bricksZ;
  const uint32_t x = index / yz;
  const uint32_t remainder = index - x * yz;
  const uint32_t y = remainder / config.bricksZ;
  return {x, y, remainder - y * config.bricksZ};
}

void setPackedMaterial(std::vector<uint32_t> &words, const uint32_t address, const uint32_t material)
{
  const uint32_t shift = (address & 3u) * 8u;
  words[address >> 2u] = (words[address >> 2u] & ~(0xffu << shift)) | ((material & 0xffu) << shift);
}

bool classifyWord(
    const TerrainGPUV2FineRasterConfigGPU &config,
    const std::vector<TerrainGPUV2ColumnGPU> &columns,
    const uint32_t brickIndex,
    const uint32_t wordIndex,
    TerrainGPUV2FineRasterWordGPU &summary,
    std::array<uint8_t, 32u> *wordMaterials,
    std::string *error)
{
  summary = {};
  summary.minimumPacked = 0xffffffffu;
  if (brickIndex >= config.denseBrickCount || wordIndex >= kTerrainGPUV2FineRasterWordsPerBrick)
    return fail(error, "terrain GPU V2 fine raster word address is invalid");
  const auto brick = denseBrickCoordinate(config, brickIndex);
  std::array<uint32_t, 3u> minimum{7u, 7u, 7u};
  std::array<uint32_t, 3u> maximum{};
  for (uint32_t bit = 0u; bit < 32u; ++bit)
  {
    const uint32_t cell = wordIndex * 32u + bit;
    const uint32_t x = cell & 7u;
    const uint32_t y = (cell >> 3u) & 7u;
    const uint32_t z = cell >> 6u;
    const uint32_t voxelX = brick[0] * 8u + x;
    const uint32_t voxelY = brick[1] * 8u + y;
    const uint32_t voxelZ = brick[2] * 8u + z;
    const uint64_t columnAddress = uint64_t{voxelZ + config.halo} * config.columnStride + voxelX + config.halo;
    if (columnAddress >= columns.size())
      return fail(error, "terrain GPU V2 fine raster column address is invalid");
    const auto &column = columns[columnAddress];
    if (column.material > 255u)
      return fail(error, "terrain GPU V2 fine raster material exceeds the byte palette");
    const int64_t canonicalY = int64_t{config.chunkMinimumCanonicalY} + uint64_t{voxelY} * config.canonicalStep;
    // Match the production CPU height-field ownership exactly. The lower
    // corner is rounded down to this chunk's voxel lattice, while a perfectly
    // flat cell still owns the voxel immediately below its surface boundary.
    // Subtracting one whole step unconditionally emitted an extra layer when
    // a sloped cell's lower corner was already lattice aligned.
    const int64_t relativeMinimum =
        int64_t{column.minimumHeightUnits} - config.chunkMinimumCanonicalY;
    const int64_t relativeMaximum =
        int64_t{column.maximumHeightUnits} - config.chunkMinimumCanonicalY;
    const int64_t roundedSurfaceFloor =
        int64_t{config.chunkMinimumCanonicalY} +
        floorDivide(relativeMinimum, config.canonicalStep) *
            config.canonicalStep;
    const int64_t surfaceTop = int64_t{config.chunkMinimumCanonicalY} +
        (-floorDivide(-relativeMaximum, config.canonicalStep) - 1) *
            config.canonicalStep;
    // Exact lattice-aligned flat columns own the voxel immediately below the
    // plane. Without this clamp their interval is inverted and the generated
    // raster/Tree64 pair agrees on a hole rather than on solid geometry.
    const int64_t surfaceFloor =
        std::min(roundedSurfaceFloor, surfaceTop);
    // Match the production exterior-shell closure.  A column owns its top
    // voxel and all cells crossed by its four corner heights.  When an
    // adjacent column is lower it additionally owns the side-face cells down
    // to one cell above that neighbour.  The column pass includes a halo, so
    // this rule is identical at page boundaries and cannot expose a crack
    // while an adjacent page is replaced.
    int64_t lowestNeighbour = surfaceTop;
    constexpr std::array<std::array<int32_t, 2u>, 4u> offsets{{
        {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}}}};
    for (const auto &offset : offsets)
    {
      const int64_t neighbourX = int64_t{voxelX + config.halo} + offset[0];
      const int64_t neighbourZ = int64_t{voxelZ + config.halo} + offset[1];
      if (neighbourX < 0 || neighbourZ < 0 ||
          neighbourX >= config.columnStride)
        continue;
      const uint64_t neighbourAddress =
          static_cast<uint64_t>(neighbourZ) * config.columnStride +
          static_cast<uint64_t>(neighbourX);
      if (neighbourAddress >= columns.size() ||
          (columns[neighbourAddress].flags & TerrainGPUV2ColumnValid) == 0u)
        continue;
      const int64_t neighbourMaximum =
          int64_t{columns[neighbourAddress].maximumHeightUnits} -
          config.chunkMinimumCanonicalY;
      const int64_t neighbourTop = int64_t{config.chunkMinimumCanonicalY} +
          (-floorDivide(-neighbourMaximum, config.canonicalStep) - 1) *
              config.canonicalStep;
      lowestNeighbour = std::min(lowestNeighbour, neighbourTop);
    }
    const int64_t firstExposedSide = lowestNeighbour < surfaceTop
        ? lowestNeighbour + config.canonicalStep
        : surfaceTop;
    const int64_t closure =
        int64_t{config.surfaceShellDepthVoxels} * config.canonicalStep;
    const int64_t cliffClosureFloor = std::max(
        int64_t{config.chunkMinimumCanonicalY}, firstExposedSide - closure);
    const int64_t shellMinimum =
        std::min(surfaceFloor, cliffClosureFloor);
    const bool occupied =
        (column.flags & TerrainGPUV2ColumnValid) != 0u &&
        canonicalY >= shellMinimum && canonicalY <= surfaceTop;
    if (wordMaterials != nullptr)
      (*wordMaterials)[bit] = static_cast<uint8_t>(column.material);
    if (!occupied)
      continue;
    summary.bits |= 1u << bit;
    ++summary.count;
    minimum = {std::min(minimum[0], x), std::min(minimum[1], y), std::min(minimum[2], z)};
    maximum = {std::max(maximum[0], x), std::max(maximum[1], y), std::max(maximum[2], z)};
    const uint32_t coarse = (x >> 1u) + 4u * ((y >> 1u) + 4u * (z >> 1u));
    if (coarse < 32u)
      summary.coarseLow |= 1u << coarse;
    else
      summary.coarseHigh |= 1u << (coarse - 32u);
  }
  if (summary.count != 0u)
  {
    summary.minimumPacked = packCoordinate(minimum[0], minimum[1], minimum[2]);
    summary.maximumPacked = packCoordinate(maximum[0], maximum[1], maximum[2]);
  }
  return true;
}

uint32_t occupiedBounds(const std::vector<TerrainGPUV2FineRasterWordGPU> &words, const uint32_t brickIndex)
{
  std::array<uint32_t, 3u> minimum{7u, 7u, 7u};
  std::array<uint32_t, 3u> maximum{};
  for (uint32_t word = 0u; word < kTerrainGPUV2FineRasterWordsPerBrick; ++word)
  {
    const auto &summary = words[uint64_t{brickIndex} * kTerrainGPUV2FineRasterWordsPerBrick + word];
    if (summary.count == 0u)
      continue;
    const auto a = unpackCoordinate(summary.minimumPacked);
    const auto b = unpackCoordinate(summary.maximumPacked);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      minimum[axis] = std::min(minimum[axis], a[axis]);
      maximum[axis] = std::max(maximum[axis], b[axis]);
    }
  }
  return minimum[0] | (minimum[1] << 3u) | (minimum[2] << 6u) | (maximum[0] << 9u) | (maximum[1] << 12u) | (maximum[2] << 15u);
}

} // namespace

bool terrainGPUV2FineRasterScratchLayout(const uint32_t denseBrickCount, TerrainGPUV2FineRasterScratchLayout &layout, std::string *error)
{
  layout = {};
  if (denseBrickCount == 0u || denseBrickCount > kTerrainGPUV2FineRasterMaximumDenseBricks)
    return fail(error, "terrain GPU V2 fine raster dense brick count is invalid");
  const uint64_t level0 = (denseBrickCount + 255u) / 256u;
  const uint64_t level1 = (level0 + 255u) / 256u;
  uint64_t cursor = 0u;
  auto append = [&](const uint64_t words, uint32_t &base)
  {
    base = static_cast<uint32_t>(cursor);
    return checkedAdd(cursor, words, cursor);
  };
  if (!append(uint64_t{denseBrickCount} * kTerrainGPUV2FineRasterWordsPerBrick * 8u, layout.wordBase) || !append(uint64_t{denseBrickCount} * 2u, layout.countBase) ||
      !append(uint64_t{denseBrickCount} * 2u, layout.offsetBase) || !append(level0 * 2u, layout.level0SumsBase) || !append(level0 * 2u, layout.level0OffsetsBase) || !append(level1 * 2u, layout.level1SumsBase) ||
      !append(level1 * 2u, layout.level1OffsetsBase) || !append(denseBrickCount, layout.compactBase))
    return fail(error, "terrain GPU V2 fine raster scratch exceeds the 32-bit shader domain");
  layout.totalWords = static_cast<uint32_t>(cursor);
  return true;
}

bool terrainGPUV2SelectFineRasterWorkgroup(
    const std::vector<TerrainGPUV2FineRasterWorkgroupCandidate> &measurements,
    const uint32_t maximumInvocations,
    TerrainGPUV2FineRasterWorkgroupCandidate &selected,
    std::string *error)
{
  selected = {};
  bool found = false;
  for (const auto &candidate : measurements)
  {
    const bool compiled = candidate.invocations == 64u || candidate.invocations == 128u || candidate.invocations == 256u;
    if (!compiled || candidate.invocations > maximumInvocations || candidate.elapsedMicroseconds == 0u)
      continue;
    if (!found || std::tie(candidate.elapsedMicroseconds, candidate.invocations) < std::tie(selected.elapsedMicroseconds, selected.invocations))
    {
      selected = candidate;
      found = true;
    }
  }
  return found || fail(error, "terrain GPU V2 has no qualified fine raster workgroup measurement");
}

bool planTerrainGPUV2FineRaster(const TerrainGPUV2FineRasterSettings &settings, TerrainGPUV2FineRasterConfigGPU &config, std::string *error)
{
  config = {};
  TerrainGPUChunkShapeCounts counts{};
  TerrainGPUChunkBounds bounds{};
  TerrainGPUV2FineRasterScratchLayout scratch{};
  if (!terrainGPUChunkShapeCounts(settings.shape, counts, error) || !terrainGPUChunkBounds(settings.key, settings.shape, bounds, error))
    return false;
  if (settings.shape.brickSide != kSparseVoxelBrickSide || counts.denseBrickCount == 0u || counts.denseBrickCount > kTerrainGPUV2FineRasterMaximumDenseBricks || settings.capacity.nodes == 0u ||
      settings.capacity.nodes > counts.denseBrickCount || settings.capacity.materials == 0u || uint64_t{settings.capacity.materials} > uint64_t{counts.denseBrickCount} * 512u || settings.regionMappingId == 0u ||
      settings.surfaceShellDepthVoxels > 32u ||
      settings.boundaryStitchSubdivisions == 0u ||
      settings.boundaryStitchSubdivisions > 8u ||
      !(settings.finestVoxelSizeMetres > 0.0) || !std::isfinite(settings.finestVoxelSizeMetres))
    return fail(error, "terrain GPU V2 fine raster settings are invalid");
  if (!terrainGPUV2FineRasterScratchLayout(counts.denseBrickCount, scratch, error))
    return false;
  const uint64_t step = uint64_t{1u} << settings.key.clipmapLOD;
  const int64_t minimumX = int64_t{bounds.minimum[0]} * step;
  const int64_t minimumY = int64_t{bounds.minimum[1]} * step;
  const int64_t minimumZ = int64_t{bounds.minimum[2]} * step;
  const int64_t maximumX = int64_t{bounds.maximum[0]} * step;
  const int64_t maximumY = int64_t{bounds.maximum[1]} * step;
  const int64_t maximumZ = int64_t{bounds.maximum[2]} * step;
  const auto &columns = settings.columns;
  if (step > std::numeric_limits<uint32_t>::max() || minimumX < std::numeric_limits<int32_t>::min() || maximumX > std::numeric_limits<int32_t>::max() || minimumY < std::numeric_limits<int32_t>::min() ||
      maximumY > std::numeric_limits<int32_t>::max() || minimumZ < std::numeric_limits<int32_t>::min() || maximumZ > std::numeric_limits<int32_t>::max() || columns.columnsX != counts.voxelDimensions[0] ||
      columns.columnsZ != counts.voxelDimensions[2] || columns.canonicalStep != step || columns.tileMinimumCanonicalX != minimumX || columns.tileMinimumCanonicalZ != minimumZ ||
      columns.expectedTerrainGeneration != settings.key.expectedGeneration || columns.halo == 0u || !(columns.heightUnitMetres > 0.0f) ||
      std::abs(static_cast<double>(columns.heightUnitMetres) - settings.finestVoxelSizeMetres) > settings.finestVoxelSizeMetres * 1.0e-6)
    return fail(error, "terrain GPU V2 fine raster and column contracts do not describe the same X/Z tile");
  const double voxelSize = settings.finestVoxelSizeMetres * step;
  const std::array<double, 3u> minimumWorld{
    settings.worldOriginMetres[0] + minimumX * settings.finestVoxelSizeMetres,
    settings.worldOriginMetres[1] + minimumY * settings.finestVoxelSizeMetres,
    settings.worldOriginMetres[2] + minimumZ * settings.finestVoxelSizeMetres};
  if (!std::isfinite(voxelSize) || voxelSize <= 0.0 || !std::isfinite(minimumWorld[0]) || !std::isfinite(minimumWorld[1]) || !std::isfinite(minimumWorld[2]))
    return fail(error, "terrain GPU V2 fine raster world transform is invalid");

  config.bricksX = settings.shape.bricksX;
  config.bricksY = settings.shape.bricksY;
  config.bricksZ = settings.shape.bricksZ;
  config.brickSide = settings.shape.brickSide;
  config.columnsX = columns.columnsX;
  config.columnsZ = columns.columnsZ;
  config.columnStride = columns.columnsX + 2u * columns.halo;
  config.halo = columns.halo;
  config.denseBrickCount = counts.denseBrickCount;
  config.nodeCapacity = settings.capacity.nodes;
  config.materialCapacity = settings.capacity.materials;
  config.materialWordCapacity = (settings.capacity.materials + 3u) / 4u;
  config.canonicalStep = static_cast<uint32_t>(step);
  config.chunkMinimumCanonicalX = static_cast<int32_t>(minimumX);
  config.chunkMinimumCanonicalY = static_cast<int32_t>(minimumY);
  config.chunkMinimumCanonicalZ = static_cast<int32_t>(minimumZ);
  config.chunkMinimumWorldX = static_cast<float>(minimumWorld[0]);
  config.chunkMinimumWorldY = static_cast<float>(minimumWorld[1]);
  config.chunkMinimumWorldZ = static_cast<float>(minimumWorld[2]);
  config.voxelSizeMetres = static_cast<float>(voxelSize);
  config.expectedTerrainGeneration = settings.key.expectedGeneration;
  config.regionMappingId = settings.regionMappingId;
  config.paletteBase = settings.paletteBase;
  config.generatorRevision = kTerrainGPUV2FineRasterGeneratorRevision;
  config.wordBase = scratch.wordBase;
  config.countBase = scratch.countBase;
  config.offsetBase = scratch.offsetBase;
  config.level0SumsBase = scratch.level0SumsBase;
  config.level0OffsetsBase = scratch.level0OffsetsBase;
  config.level1SumsBase = scratch.level1SumsBase;
  config.level1OffsetsBase = scratch.level1OffsetsBase;
  config.compactBase = scratch.compactBase;
  config.surfaceShellDepthVoxels = settings.surfaceShellDepthVoxels;
  config.boundaryStitchSubdivisions =
      settings.boundaryStitchSubdivisions;
  return true;
}

bool terrainGPUV2FineRasterMemory(const TerrainGPUV2FineRasterConfigGPU &config, TerrainGPUV2FineRasterMemory &memory, std::string *error)
{
  memory = {};
  TerrainGPUV2FineRasterScratchLayout scratch{};
  if (!terrainGPUV2FineRasterScratchLayout(config.denseBrickCount, scratch, error) || config.nodeCapacity == 0u || config.materialWordCapacity != (config.materialCapacity + 3u) / 4u)
    return fail(error, "terrain GPU V2 fine raster memory contract is invalid");
  memory.scratchBytes = uint64_t{scratch.totalWords} * sizeof(uint32_t);
  memory.outputBytes = uint64_t{config.nodeCapacity} * (sizeof(SparseVoxelBrickNodeGPU) + sizeof(SparseVoxelBrickOccupancyGPU) + sizeof(uint32_t)) + uint64_t{config.materialWordCapacity} * sizeof(uint32_t) +
                       sizeof(TerrainGPUV2FineRasterResultGPU);
  memory.microGeometryBytes = 0u;
  return true;
}

bool buildTerrainGPUV2FineRasterOracle(
    const TerrainGPUV2FineRasterConfigGPU &config,
    const TerrainGPUV2ColumnResultGPU &columnResult,
    const std::vector<TerrainGPUV2ColumnGPU> &columns,
    TerrainGPUV2FineRasterOutput &output,
    std::string *error)
{
  output = {};
  output.result.status = static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::Building);
  output.result.flags = TerrainGPUV2FineRasterLeafOnly | TerrainGPUV2FineRasterOccupancyOnly | TerrainGPUV2FineRasterNoMicroGeometry;
  output.result.denseBrickCount = config.denseBrickCount;
  output.result.terrainGeneration = config.expectedTerrainGeneration;
  output.result.generatorRevision = config.generatorRevision;
  TerrainGPUV2FineRasterScratchLayout scratch{};
  if (!terrainGPUV2FineRasterScratchLayout(config.denseBrickCount, scratch, error) || config.wordBase != scratch.wordBase || config.countBase != scratch.countBase || config.offsetBase != scratch.offsetBase ||
      config.level0SumsBase != scratch.level0SumsBase || config.level0OffsetsBase != scratch.level0OffsetsBase || config.level1SumsBase != scratch.level1SumsBase ||
      config.level1OffsetsBase != scratch.level1OffsetsBase || config.compactBase != scratch.compactBase || config.brickSide != 8u || config.regionMappingId == 0u)
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::InvalidInput);
    return fail(error, "terrain GPU V2 fine raster oracle config is invalid");
  }
  output.result.scratchWords = scratch.totalWords;
  if (columnResult.status != static_cast<uint32_t>(TerrainGPUV2ColumnStatus::Ready) || columnResult.terrainGeneration != config.expectedTerrainGeneration)
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::StaleColumns);
    return true;
  }
  const uint64_t requiredColumns = uint64_t{config.columnsZ + 2u * config.halo} * config.columnStride;
  if (columns.size() < requiredColumns)
    return fail(error, "terrain GPU V2 fine raster oracle column buffer is too small");

  std::vector<TerrainGPUV2FineRasterWordGPU> words(uint64_t{config.denseBrickCount} * kTerrainGPUV2FineRasterWordsPerBrick);
  std::vector<uint32_t> counts(config.denseBrickCount);
  std::vector<uint32_t> nodeOffsets(config.denseBrickCount);
  std::vector<uint32_t> materialOffsets(config.denseBrickCount);
  uint64_t nodeCount = 0u;
  uint64_t materialCount = 0u;
  for (uint32_t brick = 0u; brick < config.denseBrickCount; ++brick)
  {
    uint32_t occupied = 0u;
    for (uint32_t word = 0u; word < kTerrainGPUV2FineRasterWordsPerBrick; ++word)
    {
      auto &summary = words[uint64_t{brick} * kTerrainGPUV2FineRasterWordsPerBrick + word];
      if (!classifyWord(config, columns, brick, word, summary, nullptr, error))
        return false;
      occupied += summary.count;
    }
    counts[brick] = occupied;
    nodeOffsets[brick] = static_cast<uint32_t>(nodeCount);
    materialOffsets[brick] = static_cast<uint32_t>(materialCount);
    nodeCount += occupied != 0u ? 1u : 0u;
    materialCount += occupied;
  }
  if (nodeCount > config.nodeCapacity)
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::Overflow);
    output.result.overflowStage = static_cast<uint32_t>(TerrainGPUV2FineRasterOverflowStage::Nodes);
    return true;
  }
  if (materialCount > config.materialCapacity)
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::Overflow);
    output.result.overflowStage = static_cast<uint32_t>(TerrainGPUV2FineRasterOverflowStage::Materials);
    return true;
  }
  output.nodes.resize(nodeCount);
  output.occupancy.resize(nodeCount);
  output.rootIndices.resize(nodeCount);
  output.materialWords.assign((materialCount + 3u) / 4u, 0u);
  for (uint32_t brick = 0u; brick < config.denseBrickCount; ++brick)
  {
    if (counts[brick] == 0u)
      continue;
    const uint32_t nodeIndex = nodeOffsets[brick];
    const auto coordinate = denseBrickCoordinate(config, brick);
    auto &node = output.nodes[nodeIndex];
    node.minimumVoxelSize = {
      config.chunkMinimumWorldX + static_cast<float>(coordinate[0] * 8u) * config.voxelSizeMetres,
      config.chunkMinimumWorldY + static_cast<float>(coordinate[1] * 8u) * config.voxelSizeMetres,
      config.chunkMinimumWorldZ + static_cast<float>(coordinate[2] * 8u) * config.voxelSizeMetres,
      config.voxelSizeMetres};
    node.packedOccupiedBounds = occupiedBounds(words, brick);
    for (uint32_t word = 0u; word < kTerrainGPUV2FineRasterWordsPerBrick; ++word)
    {
      const auto &summary = words[uint64_t{brick} * kTerrainGPUV2FineRasterWordsPerBrick + word];
      node.coarseOccupancyLow |= summary.coarseLow;
      node.coarseOccupancyHigh |= summary.coarseHigh;
    }
    node.occupancyIndex = nodeIndex;
    node.materialBase = materialOffsets[brick];
    node.parentIndex = kSparseVoxelBrickInvalidIndex;
    node.childCount = 0u;
    node.level = kSparseVoxelBrickForceLeafRaster | (clipmapLOD(config.canonicalStep) << 8u);
    node.occupiedCount = counts[brick];
    node.firstChild = 0u;
    node.instanceIndex = kSparseVoxelBrickInvalidIndex;
    node.paletteBase = config.paletteBase;
    node.regionMappingId = config.regionMappingId;
    node.selfError = config.voxelSizeMetres * kSparseVoxelBrickErrorFloor;
    node.parentError = std::numeric_limits<float>::max();
    node.reservedTailPadding = 0u;
    node.canonicalGridOriginScale = {
      config.chunkMinimumCanonicalX + static_cast<int32_t>(coordinate[0] * 8u * config.canonicalStep),
      config.chunkMinimumCanonicalY + static_cast<int32_t>(coordinate[1] * 8u * config.canonicalStep),
      config.chunkMinimumCanonicalZ + static_cast<int32_t>(coordinate[2] * 8u * config.canonicalStep),
      static_cast<int32_t>(config.canonicalStep)};
    output.rootIndices[nodeIndex] = nodeIndex;
    uint32_t localPrefix = 0u;
    for (uint32_t word = 0u; word < kTerrainGPUV2FineRasterWordsPerBrick; ++word)
    {
      const auto &summary = words[uint64_t{brick} * kTerrainGPUV2FineRasterWordsPerBrick + word];
      output.occupancy[nodeIndex].cells[word] = {summary.bits, localPrefix};
      std::array<uint8_t, 32u> materials{};
      TerrainGPUV2FineRasterWordGPU ignored{};
      if (!classifyWord(config, columns, brick, word, ignored, &materials, error))
        return false;
      for (uint32_t bit = 0u; bit < 32u; ++bit)
        if ((summary.bits & (1u << bit)) != 0u)
          setPackedMaterial(output.materialWords, materialOffsets[brick] + localPrefix++, materials[bit]);
    }
  }
  output.result.status = static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::Ready);
  output.result.nodeCount = static_cast<uint32_t>(nodeCount);
  output.result.materialCount = static_cast<uint32_t>(materialCount);
  output.result.rootCount = static_cast<uint32_t>(nodeCount);
  output.result.occupiedVoxelCount = static_cast<uint32_t>(materialCount);
  output.result.reserved = floatBits(config.chunkMinimumWorldY);
  return validateTerrainGPUV2FineRaster(config, output, error);
}

bool terrainGPUV2PackFineBrick(const std::array<int32_t, kSparseVoxelBrickVoxelCount> &materials, SparseVoxelBrickOccupancyGPU &occupancy, std::vector<uint32_t> &materialWords, std::string *error)
{
  occupancy = {};
  uint32_t prefix = 0u;
  for (uint32_t word = 0u; word < kSparseVoxelBrickOccupancyWords; ++word)
  {
    uint32_t bits = 0u;
    for (uint32_t bit = 0u; bit < 32u; ++bit)
    {
      const int32_t material = materials[word * 32u + bit];
      if (material < -1 || material > 255)
        return fail(error, "terrain GPU V2 fine brick fixture material is invalid");
      if (material >= 0)
        bits |= 1u << bit;
    }
    occupancy.cells[word] = {bits, prefix};
    prefix += static_cast<uint32_t>(__builtin_popcount(bits));
  }
  materialWords.assign((prefix + 3u) / 4u, 0u);
  uint32_t address = 0u;
  for (uint32_t cell = 0u; cell < materials.size(); ++cell)
    if (materials[cell] >= 0)
      setPackedMaterial(materialWords, address++, static_cast<uint32_t>(materials[cell]));
  return true;
}

uint8_t terrainGPUV2FineRasterMaterial(const std::vector<uint32_t> &materialWords, const uint32_t address)
{
  if ((address >> 2u) >= materialWords.size())
    return 0u;
  return static_cast<uint8_t>((materialWords[address >> 2u] >> ((address & 3u) * 8u)) & 0xffu);
}

bool validateTerrainGPUV2FineRaster(const TerrainGPUV2FineRasterConfigGPU &config, const TerrainGPUV2FineRasterOutput &output, std::string *error)
{
  const auto &result = output.result;
  if (result.status != static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::Ready))
    return true;
  if (output.nodes.size() != result.nodeCount || output.occupancy.size() != result.nodeCount || output.rootIndices.size() != result.rootCount || result.nodeCount != result.rootCount ||
      result.materialCount != result.occupiedVoxelCount || output.materialWords.size() != (uint64_t{result.materialCount} + 3u) / 4u || result.perNodeVoxelScratchWords != 0u || result.microSurfaceBytes != 0u ||
      result.flags != (TerrainGPUV2FineRasterLeafOnly | TerrainGPUV2FineRasterOccupancyOnly | TerrainGPUV2FineRasterNoMicroGeometry))
    return fail(error, "terrain GPU V2 fine raster descriptor is inconsistent");
  uint32_t materialEnd = 0u;
  for (uint32_t index = 0u; index < result.nodeCount; ++index)
  {
    const auto &node = output.nodes[index];
    if (output.rootIndices[index] != index || node.occupancyIndex != index || node.parentIndex != kSparseVoxelBrickInvalidIndex || node.childCount != 0u || (node.level & 0xffu) != 0u ||
        (node.level & kSparseVoxelBrickForceLeafRaster) == 0u || ((node.level >> 8u) & 0xffu) != clipmapLOD(config.canonicalStep) || node.regionMappingId == 0u ||
        node.canonicalGridOriginScale[3] != static_cast<int32_t>(config.canonicalStep))
      return fail(error, "terrain GPU V2 fine raster is not a leaf-only occupancy descriptor");
    uint32_t prefix = 0u;
    for (const auto &cell : output.occupancy[index].cells)
    {
      if (cell.prefix != prefix)
        return fail(error, "terrain GPU V2 fine raster occupancy prefix is invalid");
      prefix += static_cast<uint32_t>(__builtin_popcount(cell.bits));
    }
    if (prefix != node.occupiedCount || node.materialBase != materialEnd || uint64_t{node.materialBase} + prefix > result.materialCount)
      return fail(error, "terrain GPU V2 fine raster material address is invalid");
    materialEnd += prefix;
  }
  return materialEnd == result.materialCount || fail(error, "terrain GPU V2 fine raster material total is invalid");
}

} // namespace voxel::svraster
