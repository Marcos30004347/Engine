#pragma once

#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/TerrainGPUV2FineRaster.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

inline constexpr uint32_t kTerrainGPUV2Tree64GeneratorRevision = 1u;
inline constexpr uint32_t kTerrainGPUV2Tree64MaximumLevels = 8u;
inline constexpr uint32_t kTerrainGPUV2Tree64MaximumCandidates = 1u << 20u;
inline constexpr uint32_t kTerrainGPUV2Tree64ScanBlockSize = 256u;

enum class TerrainGPUV2Tree64Status : uint32_t
{
  Empty,
  Building,
  Ready,
  Overflow,
  InvalidInput,
  StaleRaster,
  ValidationFailed,
};

enum class TerrainGPUV2Tree64OverflowStage : uint32_t
{
  None,
  Candidates,
  Nodes,
};

enum TerrainGPUV2Tree64Flags : uint32_t
{
  TerrainGPUV2Tree64ExactOccupancy = 1u << 0u,
  TerrainGPUV2Tree64ParallelValidation = 1u << 1u,
  TerrainGPUV2Tree64NoPayloadHash = 1u << 2u,
  TerrainGPUV2Tree64MortonCompaction = 1u << 3u,
};

enum TerrainGPUV2Tree64ValidationError : uint32_t
{
  TerrainGPUV2Tree64InvalidNode = 1u << 0u,
  TerrainGPUV2Tree64InvalidChildBase = 1u << 1u,
  TerrainGPUV2Tree64EmptyChild = 1u << 2u,
  TerrainGPUV2Tree64InvalidRoot = 1u << 3u,
  TerrainGPUV2Tree64OccupancyMismatch = 1u << 4u,
};

struct TerrainGPUV2Tree64Capacity
{
  uint32_t candidates = 0u;
  uint32_t nodes = 0u;
};

struct TerrainGPUV2Tree64ScratchLayout
{
  uint32_t flagBase = 0u;
  uint32_t offsetBase = 0u;
  uint32_t level0SumsBase = 0u;
  uint32_t level0OffsetsBase = 0u;
  uint32_t level1SumsBase = 0u;
  uint32_t level1OffsetsBase = 0u;
  uint32_t totalWords = 0u;
};

// Frozen 192-byte ABI. Levels are numbered finest-to-root, while their dense
// candidate ranges are laid out root-to-finest. A global stable scan therefore
// emits root node zero and keeps every parent's Morton-ordered children
// contiguous without a serial relocation pass.
struct alignas(16) TerrainGPUV2Tree64ConfigGPU
{
  uint32_t modelSide = 0u;
  uint32_t levelCount = 0u;
  uint32_t candidateCount = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t voxelsX = 0u;
  uint32_t voxelsY = 0u;
  uint32_t voxelsZ = 0u;
  uint32_t rasterNodeCapacity = 0u;
  int32_t chunkMinimumCanonicalX = 0;
  int32_t chunkMinimumCanonicalY = 0;
  int32_t chunkMinimumCanonicalZ = 0;
  uint32_t canonicalStep = 0u;
  uint32_t expectedTerrainGeneration = 0u;
  uint32_t terrainSlot = 0u;
  uint32_t nodePageSlot = 0u;
  uint32_t nodePageGeneration = 0u;
  uint32_t internalLOD = 0u;
  uint32_t voxelSizeBits = 0u;
  uint32_t geometryRevisionLow = 0u;
  uint32_t geometryRevisionHigh = 0u;
  uint32_t flagBase = 0u;
  uint32_t offsetBase = 0u;
  uint32_t level0SumsBase = 0u;
  uint32_t level0OffsetsBase = 0u;
  uint32_t level1SumsBase = 0u;
  uint32_t level1OffsetsBase = 0u;
  uint32_t scratchWords = 0u;
  uint32_t generatorRevision = kTerrainGPUV2Tree64GeneratorRevision;
  std::array<uint32_t, kTerrainGPUV2Tree64MaximumLevels> levelOffsets{};
  std::array<uint32_t, kTerrainGPUV2Tree64MaximumLevels> levelCounts{};
  std::array<uint32_t, 4u> reserved{};
};

struct TerrainGPUV2Tree64ResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPUV2Tree64Status::Empty);
  uint32_t overflowStage = static_cast<uint32_t>(TerrainGPUV2Tree64OverflowStage::None);
  uint32_t validationErrors = 0u;
  uint32_t flags = 0u;
  uint32_t candidateCount = 0u;
  uint32_t nodeCount = 0u;
  uint32_t levelCount = 0u;
  uint32_t rootNodeOffset = 0u;
  uint32_t occupiedVoxelCount = 0u;
  uint32_t rasterOccupiedVoxelCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t generatorRevision = 0u;
  uint32_t visitedNodeCount = 0u;
  uint32_t maximumChildCount = 0u;
  uint32_t fullPayloadHashWords = 0u;
  uint32_t selectedWorkgroup = 0u;
  SparseVoxelTerrainTree64DescriptorGPU descriptor{};
};

static_assert(sizeof(TerrainGPUV2Tree64ConfigGPU) == 192u);
static_assert(sizeof(TerrainGPUV2Tree64ResultGPU) == 128u);

struct TerrainGPUV2Tree64Settings
{
  TerrainGPUV2Tree64Capacity capacity{};
  uint32_t terrainSlot = 0u;
  uint32_t nodePageSlot = 0u;
  uint32_t nodePageGeneration = 0u;
  uint64_t geometryRevision = 0u;
};

struct TerrainGPUV2Tree64Output
{
  TerrainGPUV2Tree64ResultGPU result{};
  std::vector<SparseVoxelTree64NodeGPU> nodes{};
};

struct TerrainGPUV2Tree64Memory
{
  uint64_t maskBytes = 0u;
  uint64_t scratchBytes = 0u;
  uint64_t outputBytes = 0u;
};

bool terrainGPUV2Tree64ScratchLayout(uint32_t candidateCount, TerrainGPUV2Tree64ScratchLayout &layout, std::string *error = nullptr);
bool planTerrainGPUV2Tree64(const TerrainGPUV2FineRasterConfigGPU &raster, const TerrainGPUV2Tree64Settings &settings, TerrainGPUV2Tree64ConfigGPU &config, std::string *error = nullptr);
bool terrainGPUV2Tree64Memory(const TerrainGPUV2Tree64ConfigGPU &config, TerrainGPUV2Tree64Memory &memory, std::string *error = nullptr);
bool buildTerrainGPUV2Tree64Oracle(
    const TerrainGPUV2Tree64ConfigGPU &config,
    const TerrainGPUV2FineRasterResultGPU &rasterResult,
    const std::vector<SparseVoxelBrickNodeGPU> &rasterNodes,
    const std::vector<SparseVoxelBrickOccupancyGPU> &rasterOccupancy,
    TerrainGPUV2Tree64Output &output,
    std::string *error = nullptr);
bool validateTerrainGPUV2Tree64(const TerrainGPUV2Tree64ConfigGPU &config, const TerrainGPUV2Tree64Output &output, std::string *error = nullptr);

} // namespace voxel::svraster
