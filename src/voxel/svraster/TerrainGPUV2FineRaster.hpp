#pragma once

#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/TerrainGPUV2Columns.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

inline constexpr uint32_t kTerrainGPUV2FineRasterGeneratorRevision = 1u;
inline constexpr uint32_t kTerrainGPUV2FineRasterMaximumDenseBricks = 1u << 20u;
inline constexpr uint32_t kTerrainGPUV2FineRasterWordsPerBrick = 16u;
inline constexpr uint32_t kTerrainGPUV2FineRasterCellsPerInvocation = 32u;
inline constexpr uint32_t kTerrainGPUV2FineRasterNoPerVoxelScratch = 0u;

enum class TerrainGPUV2FineRasterStatus : uint32_t
{
  Empty,
  Building,
  Ready,
  Overflow,
  InvalidInput,
  StaleColumns,
  ValidationFailed,
};

enum class TerrainGPUV2FineRasterOverflowStage : uint32_t
{
  None,
  Nodes,
  Materials,
  Vertical,
};

enum TerrainGPUV2FineRasterFlags : uint32_t
{
  TerrainGPUV2FineRasterLeafOnly = 1u << 0u,
  TerrainGPUV2FineRasterOccupancyOnly = 1u << 1u,
  TerrainGPUV2FineRasterNoMicroGeometry = 1u << 2u,
};

struct TerrainGPUV2FineRasterCapacity
{
  uint32_t nodes = 0u;
  uint32_t materials = 0u;
};

struct TerrainGPUV2FineRasterScratchLayout
{
  uint32_t wordBase = 0u;
  uint32_t countBase = 0u;
  uint32_t offsetBase = 0u;
  uint32_t level0SumsBase = 0u;
  uint32_t level0OffsetsBase = 0u;
  uint32_t level1SumsBase = 0u;
  uint32_t level1OffsetsBase = 0u;
  uint32_t compactBase = 0u;
  uint32_t totalWords = 0u;
};

// Frozen 144-byte GPU ABI. The scratch offsets are in u32 words and describe
// only per-word summaries and paired scan data; no 512-cell node lane exists.
struct alignas(16) TerrainGPUV2FineRasterConfigGPU
{
  uint32_t bricksX = 0u;
  uint32_t bricksY = 0u;
  uint32_t bricksZ = 0u;
  uint32_t brickSide = 0u;
  uint32_t columnsX = 0u;
  uint32_t columnsZ = 0u;
  uint32_t columnStride = 0u;
  uint32_t halo = 0u;
  uint32_t denseBrickCount = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t materialCapacity = 0u;
  uint32_t materialWordCapacity = 0u;
  uint32_t canonicalStep = 0u;
  int32_t chunkMinimumCanonicalX = 0;
  int32_t chunkMinimumCanonicalY = 0;
  int32_t chunkMinimumCanonicalZ = 0;
  float chunkMinimumWorldX = 0.0f;
  float chunkMinimumWorldY = 0.0f;
  float chunkMinimumWorldZ = 0.0f;
  float voxelSizeMetres = 0.0f;
  uint32_t expectedTerrainGeneration = 0u;
  uint32_t regionMappingId = 0u;
  uint32_t paletteBase = 0u;
  uint32_t generatorRevision = kTerrainGPUV2FineRasterGeneratorRevision;
  uint32_t wordBase = 0u;
  uint32_t countBase = 0u;
  uint32_t offsetBase = 0u;
  uint32_t level0SumsBase = 0u;
  uint32_t level0OffsetsBase = 0u;
  uint32_t level1SumsBase = 0u;
  uint32_t level1OffsetsBase = 0u;
  uint32_t compactBase = 0u;
  uint32_t surfaceShellDepthVoxels = 1u;
  uint32_t boundaryStitchSubdivisions = 1u;
  std::array<uint32_t, 2u> reserved{};
};

struct TerrainGPUV2FineRasterWordGPU
{
  uint32_t bits = 0u;
  uint32_t count = 0u;
  uint32_t minimumPacked = 0xffffffffu;
  uint32_t maximumPacked = 0u;
  uint32_t coarseLow = 0u;
  uint32_t coarseHigh = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

struct TerrainGPUV2FineRasterResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::Empty);
  uint32_t overflowStage = static_cast<uint32_t>(TerrainGPUV2FineRasterOverflowStage::None);
  uint32_t validationErrors = 0u;
  uint32_t flags = 0u;
  uint32_t denseBrickCount = 0u;
  uint32_t nodeCount = 0u;
  uint32_t materialCount = 0u;
  uint32_t rootCount = 0u;
  uint32_t occupiedVoxelCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t generatorRevision = 0u;
  uint32_t selectedWorkgroup = 0u;
  uint32_t scratchWords = 0u;
  uint32_t perNodeVoxelScratchWords = 0u;
  uint32_t microSurfaceBytes = 0u;
  uint32_t reserved = 0u;
};

static_assert(sizeof(TerrainGPUV2FineRasterConfigGPU) == 144u);
static_assert(sizeof(TerrainGPUV2FineRasterWordGPU) == 32u);
static_assert(sizeof(TerrainGPUV2FineRasterResultGPU) == 64u);

struct TerrainGPUV2FineRasterSettings
{
  TerrainGPUChunkKey key{};
  TerrainGPUChunkShape shape{};
  TerrainGPUV2ColumnConfigGPU columns{};
  TerrainGPUV2FineRasterCapacity capacity{};
  std::array<double, 3u> worldOriginMetres{};
  double finestVoxelSizeMetres = 0.0;
  uint32_t regionMappingId = 1u;
  uint32_t paletteBase = 0u;
  uint32_t surfaceShellDepthVoxels = 1u;
  uint32_t boundaryStitchSubdivisions = 1u;
};

struct TerrainGPUV2FineRasterWorkgroupCandidate
{
  uint32_t invocations = 64u;
  uint32_t elapsedMicroseconds = 0u;
};

struct TerrainGPUV2FineRasterOutput
{
  TerrainGPUV2FineRasterResultGPU result{};
  std::vector<SparseVoxelBrickNodeGPU> nodes{};
  std::vector<SparseVoxelBrickOccupancyGPU> occupancy{};
  std::vector<uint32_t> materialWords{};
  std::vector<uint32_t> rootIndices{};
};

struct TerrainGPUV2FineRasterMemory
{
  uint64_t scratchBytes = 0u;
  uint64_t outputBytes = 0u;
  uint64_t microGeometryBytes = 0u;
};

bool terrainGPUV2FineRasterScratchLayout(uint32_t denseBrickCount, TerrainGPUV2FineRasterScratchLayout &layout, std::string *error = nullptr);

bool terrainGPUV2SelectFineRasterWorkgroup(
    const std::vector<TerrainGPUV2FineRasterWorkgroupCandidate> &measurements,
    uint32_t maximumInvocations,
    TerrainGPUV2FineRasterWorkgroupCandidate &selected,
    std::string *error = nullptr);

bool planTerrainGPUV2FineRaster(const TerrainGPUV2FineRasterSettings &settings, TerrainGPUV2FineRasterConfigGPU &config, std::string *error = nullptr);

bool terrainGPUV2FineRasterMemory(const TerrainGPUV2FineRasterConfigGPU &config, TerrainGPUV2FineRasterMemory &memory, std::string *error = nullptr);

bool buildTerrainGPUV2FineRasterOracle(
    const TerrainGPUV2FineRasterConfigGPU &config,
    const TerrainGPUV2ColumnResultGPU &columnResult,
    const std::vector<TerrainGPUV2ColumnGPU> &columns,
    TerrainGPUV2FineRasterOutput &output,
    std::string *error = nullptr);

// Generic brick packer used to exhaustively validate all sixteen occupancy
// word positions and arbitrary bit patterns independently of heightfields.
bool terrainGPUV2PackFineBrick(const std::array<int32_t, kSparseVoxelBrickVoxelCount> &materials, SparseVoxelBrickOccupancyGPU &occupancy, std::vector<uint32_t> &materialWords, std::string *error = nullptr);

uint8_t terrainGPUV2FineRasterMaterial(const std::vector<uint32_t> &materialWords, uint32_t address);

bool validateTerrainGPUV2FineRaster(const TerrainGPUV2FineRasterConfigGPU &config, const TerrainGPUV2FineRasterOutput &output, std::string *error = nullptr);

} // namespace voxel::svraster
