#pragma once

#include "voxel/svraster/PlanetVoxelChunk.hpp"
#include "voxel/svraster/SparseVoxelTerrainTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace voxel::svraster
{

inline constexpr uint32_t kTerrainGPUTree64MaximumLevels = 8u;
inline constexpr uint32_t kTerrainGPUTree64SemanticRevision = 1u;

enum class TerrainGPUTree64Status : uint32_t
{
  Empty = 0u,
  Building = 1u,
  Ready = 2u,
  Overflow = 3u,
  InvalidInput = 4u,
  ValidationFailed = 5u,
};

enum class TerrainGPUTree64OverflowStage : uint32_t
{
  None = 0u,
  DenseMasks = 1u,
  Nodes = 2u,
};

struct TerrainGPUTree64Capacity
{
  uint32_t masks = 0u;
  uint32_t nodes = 0u;
};

struct TerrainGPUTree64PageDescription
{
  uint32_t columnSide = 0u;
  uint32_t maximumVerticalVoxels = 0u;
  uint32_t internalLOD = 0u;
  double voxelSizeMetres = 0.0;
  double edgeMetres = 0.0;
};

// Fixed layout for one inactive Tree64 page. Level zero contains terminal
// 4-cubed masks; each following level OR-reduces 4x4x4 masks until the root.
struct TerrainGPUTree64ConfigGPU
{
  uint32_t side = 0u;
  uint32_t levelCount = 0u;
  uint32_t columnSide = 0u;
  uint32_t columnCount = 0u;
  uint32_t maskCapacity = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t expectedTerrainGeneration = 0u;
  uint32_t internalLOD = 0u;
  uint32_t terrainSlot = 0u;
  uint32_t nodePageSlot = 0u;
  uint32_t nodePageGeneration = 0u;
  uint32_t occupiedVoxelCount = 0u;
  uint32_t geometryRevisionLow = 0u;
  uint32_t geometryRevisionHigh = 0u;
  uint32_t voxelSizeBits = 0u;
  uint32_t edgeBits = 0u;
  std::array<uint32_t, kTerrainGPUTree64MaximumLevels> levelOffsets{};
  std::array<uint32_t, kTerrainGPUTree64MaximumLevels> levelCounts{};
  // Dense construction is rectangular even though the published model ABI is
  // cubic. Terrain pages are shallow in X/Z relative to a map-wide vertical
  // datum; allocating side^3 temporary masks is otherwise catastrophic.
  std::array<uint32_t, kTerrainGPUTree64MaximumLevels> levelHorizontalAxes{};
  std::array<uint32_t, kTerrainGPUTree64MaximumLevels> levelVerticalAxes{};
};

struct TerrainGPUTree64ResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPUTree64Status::Empty);
  uint32_t overflowStage =
      static_cast<uint32_t>(TerrainGPUTree64OverflowStage::None);
  uint32_t validationErrors = 0u;
  uint32_t side = 0u;
  uint32_t nodeCount = 0u;
  uint32_t levelCount = 0u;
  uint32_t occupiedVoxelCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t semanticHashLow = 0u;
  uint32_t semanticHashHigh = 0u;
  uint32_t geometryRevisionLow = 0u;
  uint32_t geometryRevisionHigh = 0u;
  uint32_t visitedNodeCount = 0u;
  uint32_t maximumStack = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  SparseVoxelTerrainTree64DescriptorGPU descriptor{};
};

static_assert(sizeof(TerrainGPUTree64ConfigGPU) == 192u);
static_assert(sizeof(TerrainGPUTree64ResultGPU) == 128u);
inline constexpr size_t kTerrainGPUTree64OccupiedVoxelCountConfigOffset =
    offsetof(TerrainGPUTree64ConfigGPU, occupiedVoxelCount);

bool planTerrainGPUTree64Build(
    const WorldMapTerrainColumnPage &columns,
    const TerrainGPUTree64Capacity &capacity,
    uint32_t expectedTerrainGeneration,
    uint64_t geometryRevision,
    uint32_t terrainSlot,
    uint32_t nodePageSlot,
    uint32_t nodePageGeneration,
    TerrainGPUTree64ConfigGPU &config,
    std::string *error = nullptr);

bool planTerrainGPUTree64Build(
    const TerrainGPUTree64PageDescription &page,
    const TerrainGPUTree64Capacity &capacity,
    uint32_t expectedTerrainGeneration,
    uint64_t geometryRevision,
    uint32_t terrainSlot,
    uint32_t nodePageSlot,
    uint32_t nodePageGeneration,
    TerrainGPUTree64ConfigGPU &config,
    std::string *error = nullptr);

bool validateTerrainGPUTree64Chunk(
    const SparseVoxelTree64Chunk &tree,
    std::string *error = nullptr);

uint64_t terrainGPUTree64SemanticHash(
    const SparseVoxelTree64Chunk &tree);

} // namespace voxel::svraster
