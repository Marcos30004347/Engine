#include "voxel/svraster/TerrainGPUV2Tree64.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

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

bool appendWords(uint64_t words, uint64_t &cursor, uint32_t &base)
{
  base = static_cast<uint32_t>(cursor);
  cursor += words;
  return cursor <= std::numeric_limits<uint32_t>::max();
}

uint32_t nextPowerOfFourBase4(const uint32_t required)
{
  uint32_t side = 4u;
  while (side < required && side <= std::numeric_limits<uint32_t>::max() / 4u)
    side *= 4u;
  return side >= required ? side : 0u;
}

uint32_t morton4(uint32_t x, uint32_t y, uint32_t z, uint32_t axis)
{
  uint32_t result = 0u;
  uint32_t shift = 0u;
  while (axis > 1u)
  {
    result |= (x & 3u) << shift;
    result |= (y & 3u) << (shift + 2u);
    result |= (z & 3u) << (shift + 4u);
    x >>= 2u;
    y >>= 2u;
    z >>= 2u;
    axis >>= 2u;
    shift += 6u;
  }
  return result;
}

uint32_t populationCount(const uint64_t mask)
{
  return static_cast<uint32_t>(__builtin_popcountll(mask));
}

void setMask(SparseVoxelTree64NodeGPU &node, const uint64_t mask)
{
  node.populationMaskLo = static_cast<uint32_t>(mask);
  node.populationMaskHi = static_cast<uint32_t>(mask >> 32u);
}

uint32_t clipmapLOD(uint32_t step)
{
  uint32_t result = 0u;
  while (step > 1u)
  {
    step >>= 1u;
    ++result;
  }
  return result;
}

bool configValid(const TerrainGPUV2Tree64ConfigGPU &config, std::string *error)
{
  if (config.modelSide < 4u || !sparseVoxelTree64SupportedModelSide(config.modelSide) || config.levelCount == 0u || config.levelCount > kTerrainGPUV2Tree64MaximumLevels || config.candidateCount == 0u ||
      config.candidateCount > kTerrainGPUV2Tree64MaximumCandidates || config.nodeCapacity == 0u || config.canonicalStep == 0u || config.rasterNodeCapacity == 0u ||
      config.generatorRevision != kTerrainGPUV2Tree64GeneratorRevision)
    return fail(error, "terrain GPU V2 Tree64 config header is invalid");
  uint64_t expectedCandidates = 0u;
  uint32_t expectedCount = 1u;
  for (uint32_t level = config.levelCount; level-- > 0u;)
  {
    if (config.levelOffsets[level] != expectedCandidates || config.levelCounts[level] != expectedCount)
      return fail(error, "terrain GPU V2 Tree64 level layout is invalid");
    expectedCandidates += expectedCount;
    if (level != 0u)
    {
      if (expectedCount > std::numeric_limits<uint32_t>::max() / 64u)
        return fail(error, "terrain GPU V2 Tree64 level count overflowed");
      expectedCount *= 64u;
    }
  }
  if (expectedCandidates != config.candidateCount)
    return fail(error, "terrain GPU V2 Tree64 candidate total is invalid");
  TerrainGPUV2Tree64ScratchLayout scratch{};
  if (!terrainGPUV2Tree64ScratchLayout(config.candidateCount, scratch, error) || config.flagBase != scratch.flagBase || config.offsetBase != scratch.offsetBase || config.level0SumsBase != scratch.level0SumsBase ||
      config.level0OffsetsBase != scratch.level0OffsetsBase || config.level1SumsBase != scratch.level1SumsBase || config.level1OffsetsBase != scratch.level1OffsetsBase || config.scratchWords != scratch.totalWords)
    return fail(error, "terrain GPU V2 Tree64 scratch layout is invalid");
  return true;
}
} // namespace

bool terrainGPUV2Tree64ScratchLayout(const uint32_t candidateCount, TerrainGPUV2Tree64ScratchLayout &layout, std::string *error)
{
  layout = {};
  if (candidateCount == 0u || candidateCount > kTerrainGPUV2Tree64MaximumCandidates)
    return fail(error, "terrain GPU V2 Tree64 candidate count is invalid");
  const uint64_t level0 = (uint64_t{candidateCount} + kTerrainGPUV2Tree64ScanBlockSize - 1u) / kTerrainGPUV2Tree64ScanBlockSize;
  const uint64_t level1 = (level0 + kTerrainGPUV2Tree64ScanBlockSize - 1u) / kTerrainGPUV2Tree64ScanBlockSize;
  uint64_t cursor = 0u;
  if (!appendWords(candidateCount, cursor, layout.flagBase) || !appendWords(candidateCount, cursor, layout.offsetBase) || !appendWords(level0, cursor, layout.level0SumsBase) ||
      !appendWords(level0, cursor, layout.level0OffsetsBase) || !appendWords(level1, cursor, layout.level1SumsBase) || !appendWords(level1, cursor, layout.level1OffsetsBase))
    return fail(error, "terrain GPU V2 Tree64 scratch exceeds the shader address domain");
  layout.totalWords = static_cast<uint32_t>(cursor);
  return true;
}

bool planTerrainGPUV2Tree64(const TerrainGPUV2FineRasterConfigGPU &raster, const TerrainGPUV2Tree64Settings &settings, TerrainGPUV2Tree64ConfigGPU &config, std::string *error)
{
  config = {};
  if (raster.brickSide != kSparseVoxelBrickSide || raster.bricksX == 0u || raster.bricksY == 0u || raster.bricksZ == 0u || raster.nodeCapacity == 0u || raster.canonicalStep == 0u || settings.capacity.candidates == 0u ||
      settings.capacity.nodes == 0u)
    return fail(error, "terrain GPU V2 Tree64 planner input is invalid");
  const uint64_t vx = uint64_t{raster.bricksX} * raster.brickSide;
  const uint64_t vy = uint64_t{raster.bricksY} * raster.brickSide;
  const uint64_t vz = uint64_t{raster.bricksZ} * raster.brickSide;
  if (std::max({vx, vy, vz}) > std::numeric_limits<uint32_t>::max())
    return fail(error, "terrain GPU V2 Tree64 voxel dimensions overflow");
  const uint32_t side = nextPowerOfFourBase4(static_cast<uint32_t>(std::max({vx, vy, vz})));
  if (side == 0u || !sparseVoxelTree64SupportedModelSide(side))
    return fail(error, "terrain GPU V2 Tree64 model side is unsupported");

  std::array<uint32_t, kTerrainGPUV2Tree64MaximumLevels> counts{};
  const uint64_t terminalAxis = side / 4u;
  const uint64_t terminalCount = terminalAxis * terminalAxis * terminalAxis;
  if (terminalCount > kTerrainGPUV2Tree64MaximumCandidates)
    return fail(error, "terrain GPU V2 Tree64 terminal candidate budget is unsupported");
  counts[0] = static_cast<uint32_t>(terminalCount);
  uint32_t levels = 1u;
  while (counts[levels - 1u] > 1u)
  {
    if (levels == kTerrainGPUV2Tree64MaximumLevels || counts[levels - 1u] % 64u != 0u)
      return fail(error, "terrain GPU V2 Tree64 hierarchy exceeds its frozen level ABI");
    counts[levels] = counts[levels - 1u] / 64u;
    ++levels;
  }
  uint64_t candidateCount = 0u;
  std::array<uint32_t, kTerrainGPUV2Tree64MaximumLevels> offsets{};
  for (uint32_t level = levels; level-- > 0u;)
  {
    offsets[level] = static_cast<uint32_t>(candidateCount);
    candidateCount += counts[level];
  }
  if (candidateCount > kTerrainGPUV2Tree64MaximumCandidates || candidateCount > settings.capacity.candidates)
    return fail(error, "terrain GPU V2 Tree64 candidate budget is too small");
  TerrainGPUV2Tree64ScratchLayout scratch{};
  if (!terrainGPUV2Tree64ScratchLayout(static_cast<uint32_t>(candidateCount), scratch, error))
    return false;

  config.modelSide = side;
  config.levelCount = levels;
  config.candidateCount = static_cast<uint32_t>(candidateCount);
  config.nodeCapacity = settings.capacity.nodes;
  config.voxelsX = static_cast<uint32_t>(vx);
  config.voxelsY = static_cast<uint32_t>(vy);
  config.voxelsZ = static_cast<uint32_t>(vz);
  config.rasterNodeCapacity = raster.nodeCapacity;
  config.chunkMinimumCanonicalX = raster.chunkMinimumCanonicalX;
  config.chunkMinimumCanonicalY = raster.chunkMinimumCanonicalY;
  config.chunkMinimumCanonicalZ = raster.chunkMinimumCanonicalZ;
  config.canonicalStep = raster.canonicalStep;
  config.expectedTerrainGeneration = raster.expectedTerrainGeneration;
  config.terrainSlot = settings.terrainSlot;
  config.nodePageSlot = settings.nodePageSlot;
  config.nodePageGeneration = settings.nodePageGeneration;
  config.internalLOD = clipmapLOD(raster.canonicalStep);
  std::memcpy(&config.voxelSizeBits, &raster.voxelSizeMetres, sizeof(config.voxelSizeBits));
  config.geometryRevisionLow = static_cast<uint32_t>(settings.geometryRevision);
  config.geometryRevisionHigh = static_cast<uint32_t>(settings.geometryRevision >> 32u);
  config.flagBase = scratch.flagBase;
  config.offsetBase = scratch.offsetBase;
  config.level0SumsBase = scratch.level0SumsBase;
  config.level0OffsetsBase = scratch.level0OffsetsBase;
  config.level1SumsBase = scratch.level1SumsBase;
  config.level1OffsetsBase = scratch.level1OffsetsBase;
  config.scratchWords = scratch.totalWords;
  config.levelOffsets = offsets;
  config.levelCounts = counts;
  return true;
}

bool terrainGPUV2Tree64Memory(const TerrainGPUV2Tree64ConfigGPU &config, TerrainGPUV2Tree64Memory &memory, std::string *error)
{
  memory = {};
  if (!configValid(config, error))
    return false;
  memory.maskBytes = uint64_t{config.candidateCount} * sizeof(uint64_t);
  memory.scratchBytes = uint64_t{config.scratchWords} * sizeof(uint32_t);
  memory.outputBytes = uint64_t{config.nodeCapacity} * sizeof(SparseVoxelTree64NodeGPU) + sizeof(TerrainGPUV2Tree64ResultGPU);
  return true;
}

bool buildTerrainGPUV2Tree64Oracle(
    const TerrainGPUV2Tree64ConfigGPU &config,
    const TerrainGPUV2FineRasterResultGPU &rasterResult,
    const std::vector<SparseVoxelBrickNodeGPU> &rasterNodes,
    const std::vector<SparseVoxelBrickOccupancyGPU> &rasterOccupancy,
    TerrainGPUV2Tree64Output &output,
    std::string *error)
{
  output = {};
  output.result.status = static_cast<uint32_t>(TerrainGPUV2Tree64Status::Building);
  output.result.candidateCount = config.candidateCount;
  output.result.levelCount = config.levelCount;
  output.result.generatorRevision = kTerrainGPUV2Tree64GeneratorRevision;
  output.result.flags = TerrainGPUV2Tree64ExactOccupancy | TerrainGPUV2Tree64ParallelValidation | TerrainGPUV2Tree64NoPayloadHash | TerrainGPUV2Tree64MortonCompaction;
  if (!configValid(config, error))
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2Tree64Status::InvalidInput);
    return false;
  }
  if (rasterResult.status != static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::Ready) || rasterResult.terrainGeneration != config.expectedTerrainGeneration)
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2Tree64Status::StaleRaster);
    return true;
  }
  if (rasterResult.nodeCount > config.rasterNodeCapacity || rasterNodes.size() < rasterResult.nodeCount || rasterOccupancy.size() < rasterResult.nodeCount)
    return fail(error, "terrain GPU V2 Tree64 raster input is truncated");

  std::vector<uint64_t> masks(config.candidateCount, 0u);
  const uint32_t terminalOffset = config.levelOffsets[0];
  const uint32_t terminalAxis = config.modelSide / 4u;
  for (uint32_t index = 0u; index < rasterResult.nodeCount; ++index)
  {
    const auto &node = rasterNodes[index];
    if (node.occupancyIndex >= rasterOccupancy.size() || node.canonicalGridOriginScale[3] != static_cast<int32_t>(config.canonicalStep))
      return fail(error, "terrain GPU V2 Tree64 raster node contract is invalid");
    std::array<int64_t, 3u> base{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t minimum = axis == 0u ? config.chunkMinimumCanonicalX : axis == 1u ? config.chunkMinimumCanonicalY : config.chunkMinimumCanonicalZ;
      const int64_t delta = int64_t{node.canonicalGridOriginScale[axis]} - minimum;
      if (delta < 0 || delta % config.canonicalStep != 0)
        return fail(error, "terrain GPU V2 Tree64 raster node is off the canonical lattice");
      base[axis] = delta / config.canonicalStep;
    }
    const auto &occupancy = rasterOccupancy[node.occupancyIndex];
    for (uint32_t word = 0u; word < kSparseVoxelBrickOccupancyWords; ++word)
    {
      uint32_t bits = occupancy.cells[word].bits;
      while (bits != 0u)
      {
        const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(bits));
        bits &= bits - 1u;
        const uint32_t cell = word * 32u + bit;
        const uint32_t x = static_cast<uint32_t>(base[0]) + (cell & 7u);
        const uint32_t y = static_cast<uint32_t>(base[1]) + ((cell >> 3u) & 7u);
        const uint32_t z = static_cast<uint32_t>(base[2]) + (cell >> 6u);
        if (x >= config.voxelsX || y >= config.voxelsY || z >= config.voxelsZ)
          return fail(error, "terrain GPU V2 Tree64 occupied raster voxel is outside the chunk");
        const uint32_t candidate = terminalOffset + morton4(x >> 2u, y >> 2u, z >> 2u, terminalAxis);
        const uint32_t child = (x & 3u) + 4u * (y & 3u) + 16u * (z & 3u);
        masks[candidate] |= uint64_t{1u} << child;
      }
    }
  }

  for (uint32_t level = 1u; level < config.levelCount; ++level)
  {
    const uint32_t childOffset = config.levelOffsets[level - 1u];
    const uint32_t parentOffset = config.levelOffsets[level];
    for (uint32_t parent = 0u; parent < config.levelCounts[level]; ++parent)
      for (uint32_t child = 0u; child < 64u; ++child)
        if (masks[childOffset + parent * 64u + child] != 0u)
          masks[parentOffset + parent] |= uint64_t{1u} << child;
  }

  std::vector<uint32_t> offsets(config.candidateCount);
  uint64_t nodeCount = 0u;
  const uint32_t rootCandidate = config.levelOffsets[config.levelCount - 1u];
  for (uint32_t candidate = 0u; candidate < config.candidateCount; ++candidate)
  {
    offsets[candidate] = static_cast<uint32_t>(nodeCount);
    nodeCount += masks[candidate] != 0u || candidate == rootCandidate ? 1u : 0u;
  }
  if (nodeCount > config.nodeCapacity)
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2Tree64Status::Overflow);
    output.result.overflowStage = static_cast<uint32_t>(TerrainGPUV2Tree64OverflowStage::Nodes);
    return true;
  }
  output.nodes.resize(static_cast<size_t>(nodeCount));
  for (uint32_t level = 0u; level < config.levelCount; ++level)
    for (uint32_t local = 0u; local < config.levelCounts[level]; ++local)
    {
      const uint32_t candidate = config.levelOffsets[level] + local;
      if (masks[candidate] == 0u && candidate != rootCandidate)
        continue;
      auto &node = output.nodes[offsets[candidate]];
      setMask(node, masks[candidate]);
      if (level == 0u)
        node.childBaseAndFlags = kSparseVoxelTree64TerminalFlag;
    }
  for (uint32_t level = config.levelCount; level-- > 1u;)
    for (uint32_t parent = 0u; parent < config.levelCounts[level]; ++parent)
    {
      const uint32_t candidate = config.levelOffsets[level] + parent;
      if (masks[candidate] == 0u)
        continue;
      output.nodes[offsets[candidate]].childBaseAndFlags = offsets[config.levelOffsets[level - 1u] + parent * 64u] << 1u;
    }

  uint32_t occupied = 0u;
  uint32_t maximumChildren = 0u;
  for (uint32_t terminal = 0u; terminal < config.levelCounts[0]; ++terminal)
    occupied += populationCount(masks[terminalOffset + terminal]);
  for (const auto &node : output.nodes)
    maximumChildren = std::max(maximumChildren, populationCount(sparseVoxelTree64PopulationMask(node)));

  output.result.status = static_cast<uint32_t>(TerrainGPUV2Tree64Status::Ready);
  output.result.nodeCount = static_cast<uint32_t>(nodeCount);
  output.result.rootNodeOffset = 0u;
  output.result.occupiedVoxelCount = occupied;
  output.result.rasterOccupiedVoxelCount = rasterResult.occupiedVoxelCount;
  output.result.terrainGeneration = rasterResult.terrainGeneration;
  output.result.visitedNodeCount = static_cast<uint32_t>(nodeCount);
  output.result.maximumChildCount = maximumChildren;
  auto &descriptor = output.result.descriptor;
  descriptor.terrainSlot = config.terrainSlot;
  descriptor.terrainGeneration = rasterResult.terrainGeneration;
  descriptor.internalLOD = config.internalLOD;
  descriptor.flags = kSparseVoxelTree64TerrainPageValid;
  descriptor.nodePageSlot = config.nodePageSlot;
  descriptor.nodePageGeneration = config.nodePageGeneration;
  descriptor.rootNodeOffset = 0u;
  descriptor.nodeCount = static_cast<uint32_t>(nodeCount);
  descriptor.geometryRevisionLo = config.geometryRevisionLow;
  descriptor.geometryRevisionHi = config.geometryRevisionHigh;
  descriptor.modelSide = config.modelSide;
  descriptor.modelCellSizeBits = config.voxelSizeBits;
  if (!validateTerrainGPUV2Tree64(config, output, error))
  {
    output.result.status = static_cast<uint32_t>(TerrainGPUV2Tree64Status::ValidationFailed);
    output.result.validationErrors |= TerrainGPUV2Tree64InvalidNode;
  }
  return true;
}

bool validateTerrainGPUV2Tree64(const TerrainGPUV2Tree64ConfigGPU &config, const TerrainGPUV2Tree64Output &output, std::string *error)
{
  if (output.result.status != static_cast<uint32_t>(TerrainGPUV2Tree64Status::Ready))
    return true;
  if (!configValid(config, error) || output.nodes.size() != output.result.nodeCount || output.nodes.empty() || output.result.rootNodeOffset != 0u ||
      output.result.occupiedVoxelCount != output.result.rasterOccupiedVoxelCount || output.result.fullPayloadHashWords != 0u ||
      output.result.flags != (TerrainGPUV2Tree64ExactOccupancy | TerrainGPUV2Tree64ParallelValidation | TerrainGPUV2Tree64NoPayloadHash | TerrainGPUV2Tree64MortonCompaction))
    return fail(error, "terrain GPU V2 Tree64 result contract is invalid");
  const auto &descriptor = output.result.descriptor;
  if (descriptor.flags != kSparseVoxelTree64TerrainPageValid || descriptor.rootNodeOffset != 0u || descriptor.nodeCount != output.nodes.size() || descriptor.modelSide != config.modelSide ||
      descriptor.modelCellSizeBits != config.voxelSizeBits || descriptor.terrainGeneration != config.expectedTerrainGeneration)
    return fail(error, "terrain GPU V2 Tree64 descriptor is invalid");
  SparseVoxelTree64Chunk tree{config.modelSide, output.nodes};
  return validateSparseVoxelTree64Model(tree, error);
}

} // namespace voxel::svraster
