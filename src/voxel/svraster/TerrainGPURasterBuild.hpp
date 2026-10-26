#pragma once

#include "voxel/svraster/PlanetVoxelChunk.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

// A 4096-cell global vertical datum contains 512 fine brick rows and needs ten
// binary reductions before it reaches one row. Keep two spare levels for the
// supported wider page descriptions and future alignment changes.
inline constexpr uint32_t kTerrainGPURasterMaximumLevels = 12u;
inline constexpr uint32_t kTerrainGPURasterMaterialTableSize = 256u;
inline constexpr uint32_t kTerrainGPURasterSemanticRevision = 1u;

enum class TerrainGPURasterStatus : uint32_t
{
  Empty = 0u,
  Building = 1u,
  Ready = 2u,
  Overflow = 3u,
  InvalidInput = 4u,
  ValidationFailed = 5u,
  StaleGeneration = 6u,
};

enum class TerrainGPURasterOverflowStage : uint32_t
{
  None = 0u,
  DenseCandidates = 1u,
  Nodes = 2u,
  Children = 3u,
  Materials = 4u,
  Roots = 5u,
};

struct TerrainGPURasterCapacity
{
  uint32_t denseCandidates = 0u;
  uint32_t nodes = 0u;
  uint32_t children = 0u;
  uint32_t materials = 0u;
  uint32_t roots = 0u;
};

// Geometry-independent build description used by the full-GPU path. The CPU
// scheduler owns these page dimensions, but never materializes columns,
// voxels, bricks, or hierarchy payloads.
struct TerrainGPURasterPageDescription
{
  uint32_t side = 0u;
  uint32_t maximumVerticalVoxels = 0u;
  double voxelSizeMetres = 0.0;
  double edgeMetres = 0.0;
};

// Dense candidates are deliberately ordered X-major, then Y, then Z. This is
// the same ordering as std::map<BrickKey, ...> in the CPU oracle and makes
// compaction independent of workgroup scheduling and frame slicing.
struct TerrainGPURasterLevelGPU
{
  int32_t minimumX = 0;
  int32_t minimumY = 0;
  int32_t minimumZ = 0;
  uint32_t candidateBase = 0u;
  uint32_t sizeX = 0u;
  uint32_t sizeY = 0u;
  uint32_t sizeZ = 0u;
  uint32_t reserved = 0u;
};

struct TerrainGPURasterConfigGPU
{
  uint32_t side = 0u;
  uint32_t maximumVerticalVoxels = 0u;
  uint32_t maximumLevelCount = 0u;
  uint32_t columnCount = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t childCapacity = 0u;
  uint32_t materialCapacity = 0u;
  uint32_t rootCapacity = 0u;
  uint32_t denseCandidateCapacity = 0u;
  uint32_t materialWordCapacity = 0u;
  uint32_t paletteCount = 0u;
  uint32_t expectedTerrainGeneration = 0u;
  uint32_t buildRevisionLow = 0u;
  uint32_t buildRevisionHigh = 0u;
  uint32_t voxelSizeBits = 0u;
  uint32_t edgeBits = 0u;
  std::array<TerrainGPURasterLevelGPU,
      kTerrainGPURasterMaximumLevels> levels{};
};

struct TerrainGPURasterMaterialGPU
{
  uint32_t paletteEntry = 0xffffffffu;
  uint32_t packed0 = 0u;
  uint32_t packed1 = 0u;
  uint32_t reserved = 0u;
};

// GPU staging record. It preserves the two public Phase 1 records verbatim but
// binds them as one storage array to stay below conservative storage-binding
// limits on Phase 3 hardware.
struct TerrainGPURasterColumnGPU
{
  WorldMapTerrainColumnGPU column{};
  WorldMapTerrainColumnWallGPU wall{};
};

struct TerrainGPURasterResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPURasterStatus::Empty);
  uint32_t overflowStage =
      static_cast<uint32_t>(TerrainGPURasterOverflowStage::None);
  uint32_t validationErrors = 0u;
  uint32_t levelCount = 0u;
  uint32_t nodeCount = 0u;
  uint32_t childCount = 0u;
  uint32_t materialCount = 0u;
  uint32_t rootCount = 0u;
  uint32_t occupiedVoxelCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t semanticHashLow = 0u;
  uint32_t semanticHashHigh = 0u;
  uint32_t buildRevisionLow = 0u;
  uint32_t buildRevisionHigh = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};

static_assert(sizeof(TerrainGPURasterLevelGPU) == 32u);
static_assert(sizeof(TerrainGPURasterConfigGPU) == 448u);
static_assert(sizeof(TerrainGPURasterMaterialGPU) == 16u);
static_assert(sizeof(TerrainGPURasterColumnGPU) == 32u);
static_assert(sizeof(TerrainGPURasterResultGPU) == 64u);

// Produces the fixed dispatch layout and validates the column subset supported
// by Phase 3. `denseCandidates` is both an allocation contract and a hard
// safety ceiling; no shader stage derives an address outside this plan.
bool planTerrainGPURasterBuild(
    const WorldMapTerrainColumnPage &page,
    const TerrainGPURasterCapacity &capacity,
    uint32_t expectedTerrainGeneration,
    uint64_t buildRevision,
    uint32_t paletteCount,
    TerrainGPURasterConfigGPU &config,
    std::string *error = nullptr);

bool planTerrainGPURasterBuild(
    const TerrainGPURasterPageDescription &page,
    const TerrainGPURasterCapacity &capacity,
    uint32_t expectedTerrainGeneration,
    uint64_t buildRevision,
    uint32_t paletteCount,
    TerrainGPURasterConfigGPU &config,
    std::string *error = nullptr);

// The terrain palette is still an immutable upload in Phase 3. This creates a
// material-id lookup into the exact CPU-oracle palette; GPU kernels generate
// only the per-occupied-cell byte and context streams.
bool buildTerrainGPURasterMaterialTable(
    const WorldMapTerrainColumnPage &page,
    const SparseVoxelBrickHierarchy &reference,
    std::array<TerrainGPURasterMaterialGPU,
        kTerrainGPURasterMaterialTableSize> &table,
    std::string *error = nullptr);

// Builds the complete material-id lookup without inspecting generated
// columns. This is the production full-GPU contract; the page-specific
// overload above remains the differential-test oracle.
bool buildTerrainGPURasterMaterialTable(
    const SparseVoxelBrickHierarchy &reference,
    std::array<TerrainGPURasterMaterialGPU,
        kTerrainGPURasterMaterialTableSize> &table,
    std::string *error = nullptr);

// CPU mirror of the terminal GPU validator. It is intentionally independent
// of the generic builder and is used for readback qualification and for the
// future Phase 5 publication guard.
bool validateTerrainGPURasterHierarchy(
    const SparseVoxelBrickHierarchy &hierarchy,
    std::string *error = nullptr);

// Canonical payload hash used by CPU/GPU differential tests. It excludes
// vector capacities and statistics and includes every renderer-visible raster
// word in canonical emission order.
uint64_t terrainGPURasterSemanticHash(
    const SparseVoxelBrickHierarchy &hierarchy);

} // namespace voxel::svraster
