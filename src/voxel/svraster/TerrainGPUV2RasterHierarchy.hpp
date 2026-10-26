#pragma once

#include "voxel/svraster/TerrainGPUV2FineRaster.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

inline constexpr uint32_t kTerrainGPUV2RasterHierarchyGeneratorRevision = 2u;
inline constexpr uint32_t kTerrainGPUV2RasterHierarchyMaximumLevels = 8u;

enum class TerrainGPUV2RasterMode : uint32_t
{
  LeafOnly,
  Hierarchy,
};

enum class TerrainGPUV2RasterHierarchyStatus : uint32_t
{
  Empty,
  Building,
  Ready,
  Overflow,
  InvalidInput,
  StaleRaster,
  ValidationFailed,
};

enum class TerrainGPUV2RasterHierarchyOverflowStage : uint32_t
{
  None,
  Nodes,
  Materials,
  Children,
  Roots,
};

enum TerrainGPUV2RasterHierarchyFlags : uint32_t
{
  TerrainGPUV2RasterHierarchyOccupancyOnly = 1u << 0u,
  TerrainGPUV2RasterHierarchyNoMicroGeometry = 1u << 1u,
  TerrainGPUV2RasterHierarchyMonotonicError = 1u << 2u,
  TerrainGPUV2RasterHierarchyExperimental = 1u << 3u,
};

struct TerrainGPUV2RasterHierarchyCapacity
{
  uint32_t nodes = 0u;
  uint32_t materials = 0u;
  uint32_t children = 0u;
  uint32_t roots = 0u;
};

struct TerrainGPUV2RasterHierarchyScratchLayout
{
  uint32_t summaryBase = 0u;
  uint32_t countBase = 0u;
  uint32_t offsetBase = 0u;
  uint32_t level0SumsBase = 0u;
  uint32_t level0OffsetsBase = 0u;
  uint32_t level1SumsBase = 0u;
  uint32_t level1OffsetsBase = 0u;
  uint32_t mappingBase = 0u;
  uint32_t levelStatsBase = 0u;
  uint32_t totalWords = 0u;
};

// Frozen 320-byte ABI. Level zero is the Phase 4 leaf grid. Higher levels use
// ceil(axis / 2) independently, so non-cubic and odd-sized 3D chunks remain
// half-open and do not acquire padding voxels at their boundaries.
struct alignas(16) TerrainGPUV2RasterHierarchyConfigGPU
{
  uint32_t bricksX = 0u;
  uint32_t bricksY = 0u;
  uint32_t bricksZ = 0u;
  uint32_t brickSide = 0u;
  uint32_t levelCount = 0u;
  uint32_t totalDenseCandidates = 0u;
  uint32_t maximumParentCandidates = 0u;
  uint32_t expectedTerrainGeneration = 0u;
  uint32_t leafNodeCapacity = 0u;
  uint32_t leafMaterialCapacity = 0u;
  uint32_t leafMaterialWordCapacity = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t materialCapacity = 0u;
  uint32_t materialWordCapacity = 0u;
  uint32_t childCapacity = 0u;
  uint32_t rootCapacity = 0u;
  uint32_t canonicalStep = 0u;
  int32_t chunkMinimumCanonicalX = 0;
  int32_t chunkMinimumCanonicalY = 0;
  int32_t chunkMinimumCanonicalZ = 0;
  float chunkMinimumWorldX = 0.0f;
  float chunkMinimumWorldY = 0.0f;
  float chunkMinimumWorldZ = 0.0f;
  float voxelSizeMetres = 0.0f;
  uint32_t regionMappingId = 0u;
  uint32_t paletteBase = 0u;
  uint32_t generatorRevision = kTerrainGPUV2RasterHierarchyGeneratorRevision;
  uint32_t selectedMode = static_cast<uint32_t>(TerrainGPUV2RasterMode::Hierarchy);
  uint32_t summaryBase = 0u;
  uint32_t countBase = 0u;
  uint32_t offsetBase = 0u;
  uint32_t level0SumsBase = 0u;
  uint32_t level0OffsetsBase = 0u;
  uint32_t level1SumsBase = 0u;
  uint32_t level1OffsetsBase = 0u;
  uint32_t mappingBase = 0u;
  uint32_t levelStatsBase = 0u;
  uint32_t scratchWords = 0u;
  uint32_t flags = 0u;
  uint32_t reserved = 0u;
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> levelDenseOffsets{};
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> levelDenseCounts{};
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> levelAxesX{};
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> levelAxesY{};
  std::array<uint32_t, kTerrainGPUV2RasterHierarchyMaximumLevels> levelAxesZ{};
};

struct TerrainGPUV2RasterHierarchyResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPUV2RasterHierarchyStatus::Empty);
  uint32_t overflowStage = static_cast<uint32_t>(TerrainGPUV2RasterHierarchyOverflowStage::None);
  uint32_t validationErrors = 0u;
  uint32_t flags = 0u;
  uint32_t leafNodeCount = 0u;
  uint32_t nodeCount = 0u;
  uint32_t materialCount = 0u;
  uint32_t childCount = 0u;
  uint32_t rootCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t generatorRevision = 0u;
  uint32_t selectedWorkgroup = 0u;
  uint32_t levelCount = 0u;
  uint32_t scratchWords = 0u;
  uint32_t microSurfaceBytes = 0u;
  uint32_t selectedMode = static_cast<uint32_t>(TerrainGPUV2RasterMode::LeafOnly);
};

static_assert(sizeof(TerrainGPUV2RasterHierarchyConfigGPU) == 320u);
static_assert(sizeof(TerrainGPUV2RasterHierarchyResultGPU) == 64u);

struct TerrainGPUV2RasterHierarchySettings
{
  TerrainGPUV2RasterHierarchyCapacity capacity{};
  uint32_t maximumLevels = kTerrainGPUV2RasterHierarchyMaximumLevels;
};

struct TerrainGPUV2RasterHierarchyOutput
{
  TerrainGPUV2RasterHierarchyResultGPU result{};
  std::vector<SparseVoxelBrickNodeGPU> nodes{};
  std::vector<SparseVoxelBrickOccupancyGPU> occupancy{};
  std::vector<uint32_t> materialWords{};
  std::vector<uint32_t> childIndices{};
  std::vector<uint32_t> rootIndices{};
};

struct TerrainGPUV2RasterHierarchyMemory
{
  uint64_t scratchBytes = 0u;
  uint64_t outputBytes = 0u;
  uint64_t microGeometryBytes = 0u;
};

struct TerrainGPUV2RasterHierarchyQualification
{
  bool experimentEnabled = false;
  bool deviceQualified = false;
  uint32_t minimumImprovementPermille = 50u;
  uint32_t leafOnlyEndToEndP99Microseconds = 0u;
  uint32_t hierarchyEndToEndP99Microseconds = 0u;
};

struct TerrainGPUV2RasterHierarchyCapture
{
  uint32_t buildP99Microseconds = 0u;
  uint32_t endToEndP99Microseconds = 0u;
  uint32_t renderedNodesP99 = 0u;
  uint64_t residentBytes = 0u;
};

struct TerrainGPUV2RasterHierarchyCaptureComparison
{
  uint32_t renderNodeSavingsPermille = 0u;
  int32_t buildP99DeltaMicroseconds = 0;
  int32_t endToEndImprovementPermille = 0;
  int64_t residentByteDelta = 0;
};

bool terrainGPUV2RasterHierarchyScratchLayout(
    uint32_t maximumParentCandidates,
    uint32_t totalDenseCandidates,
    uint32_t levelCount,
    TerrainGPUV2RasterHierarchyScratchLayout &layout,
    std::string *error = nullptr);

bool planTerrainGPUV2RasterHierarchy(
    const TerrainGPUV2FineRasterConfigGPU &raster,
    const TerrainGPUV2RasterHierarchySettings &settings,
    TerrainGPUV2RasterHierarchyConfigGPU &config,
    std::string *error = nullptr);

bool terrainGPUV2RasterHierarchyMemory(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    TerrainGPUV2RasterHierarchyMemory &memory,
    std::string *error = nullptr);

TerrainGPUV2RasterMode selectTerrainGPUV2RasterMode(const TerrainGPUV2RasterHierarchyQualification &qualification);

TerrainGPUV2RasterHierarchyCaptureComparison compareTerrainGPUV2RasterHierarchyCaptures(
    const TerrainGPUV2RasterHierarchyCapture &leafOnly,
    const TerrainGPUV2RasterHierarchyCapture &hierarchy);

bool buildTerrainGPUV2RasterHierarchyOracle(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    const TerrainGPUV2FineRasterResultGPU &rasterResult,
    const std::vector<SparseVoxelBrickNodeGPU> &rasterNodes,
    const std::vector<SparseVoxelBrickOccupancyGPU> &rasterOccupancy,
    const std::vector<uint32_t> &rasterMaterialWords,
    const std::vector<TerrainGPUV2HeightSampleGPU> &heightSamples,
    TerrainGPUV2RasterHierarchyOutput &output,
    std::string *error = nullptr);

bool buildTerrainGPUV2RasterHierarchyOracle(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    const TerrainGPUV2FineRasterResultGPU &rasterResult,
    const std::vector<SparseVoxelBrickNodeGPU> &rasterNodes,
    const std::vector<SparseVoxelBrickOccupancyGPU> &rasterOccupancy,
    const std::vector<uint32_t> &rasterMaterialWords,
    TerrainGPUV2RasterHierarchyOutput &output,
    std::string *error = nullptr);

bool validateTerrainGPUV2RasterHierarchy(
    const TerrainGPUV2RasterHierarchyConfigGPU &config,
    const TerrainGPUV2RasterHierarchyOutput &output,
    std::string *error = nullptr);

std::vector<uint32_t> selectTerrainGPUV2RasterHierarchyCut(
    const TerrainGPUV2RasterHierarchyOutput &hierarchy,
    float maximumWorldError);

} // namespace voxel::svraster
