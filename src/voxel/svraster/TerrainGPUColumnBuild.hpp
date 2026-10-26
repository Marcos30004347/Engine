#pragma once

#include "procedural/world/MapExportGpuCache.hpp"
#include "voxel/svraster/TerrainGPURasterBuild.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

inline constexpr uint64_t kTerrainGPUColumnGeneratorRevision = 14u;
inline constexpr uint32_t kTerrainGPUColumnFieldCount = 20u;
inline constexpr uint32_t kTerrainGPUColumnInvalidField = 0xffffffffu;

enum class TerrainGPUColumnStatus : uint32_t
{
  Empty = 0u,
  Building = 1u,
  Ready = 2u,
  InvalidInput = 3u,
  MissingPage = 4u,
  VerticalOverflow = 5u,
  ValidationFailed = 6u,
  StaleAtlas = 7u,
};

enum class TerrainGPUColumnField : uint32_t
{
  Elevation = 0u,
  BaseElevation,
  Uplift,
  ReliefRoughness,
  Landform,
  WaterType,
  WaterLevel,
  River,
  CoastDistance,
  ErosionWearFlow,
  ErosionDeposition,
  Substrate,
  SoilDepth,
  BiomeIds,
  BiomeWeights,
  Climate,
  PlacementPotential,
  SoilLayers,
  Roads,
  SettlementOrExclusion,
};

enum TerrainGPUColumnFlags : uint32_t
{
  TerrainGPUColumnExteriorShell = 1u << 0u,
  // Derive a page-local vertical datum from the sampled halo entirely on the
  // GPU. The resolved value is returned as compact metadata and is consumed
  // by raster/Tree64 relocation without reading columns back to the CPU.
  TerrainGPUColumnResolvePageDatum = 1u << 1u,
};

// Revision 14 resolves a compact per-page datum on the GPU. The resolver snaps
// it to that page's voxel lattice, so every datum is also on the canonical
// finest lattice and adjacent/parent pages share identical world-space planes.
struct TerrainGPUColumnBuildSettings
{
  std::array<double, 4> normalizedBounds{0.0, 0.0, 1.0, 1.0};
  uint32_t side = 0u;
  uint32_t halo = 2u;
  uint32_t maximumVerticalVoxels = 0u;
  uint32_t requestedLevel = 0u;
  uint32_t internalLOD = 0u;
  uint32_t boundaryStitchSubdivisions = 1u;
  uint32_t surfaceShellDepthVoxels = 2u;
  bool exteriorShellOnly = true;
  bool resolveElevationOriginOnGPU = false;
  uint32_t foundationDepthVoxels = 4u;
  double finestVoxelSizeMetres = 0.0;
  double elevationOriginMetres = 0.0;
  double proceduralDetailStrength = 1.0;
  double proceduralTerraceStrength = 1.0;
  double proceduralTerraceHeightMetres = 12.0;
  double proceduralTerraceTransitionFraction = 0.12;
  uint32_t expectedTerrainGeneration = 0u;
  uint64_t expectedAtlasRevision = 0u;
};

// Kept word-addressable and free of bools so the layout is identical in WGSL.
struct TerrainGPUColumnConfigGPU
{
  uint32_t side = 0u;
  uint32_t halo = 0u;
  uint32_t maximumVerticalVoxels = 0u;
  uint32_t requestedLevel = 0u;
  uint32_t internalLOD = 0u;
  uint32_t boundaryStitchSubdivisions = 0u;
  uint32_t surfaceShellDepthVoxels = 0u;
  uint32_t flags = 0u;
  uint32_t expectedTerrainGeneration = 0u;
  uint32_t generatorRevisionLow = 0u;
  uint32_t generatorRevisionHigh = 0u;
  uint32_t expectedAtlasRevisionLow = 0u;
  uint32_t expectedAtlasRevisionHigh = 0u;
  uint32_t seedLow = 0u;
  uint32_t seedHigh = 0u;
  uint32_t bundleSignatureLow = 0u;
  uint32_t bundleSignatureHigh = 0u;
  float minimumU = 0.0f;
  float minimumV = 0.0f;
  float maximumU = 0.0f;
  float maximumV = 0.0f;
  float extentMetres = 0.0f;
  float edgeMetres = 0.0f;
  float voxelSizeMetres = 0.0f;
  float finestVoxelSizeMetres = 0.0f;
  float heightUnitMetres = 0.0f;
  float elevationOriginMetres = 0.0f;
  float proceduralDetailStrength = 0.0f;
  float proceduralTerraceStrength = 0.0f;
  float proceduralTerraceHeightMetres = 0.0f;
  float proceduralTerraceTransitionFraction = 0.0f;
  std::array<uint32_t, kTerrainGPUColumnFieldCount> fields{};
  uint32_t foundationDepthVoxels = 0u;
  std::array<uint32_t, 4u> reserved{};
};

struct TerrainGPUHeight64GPU
{
  uint32_t low = 0u;
  uint32_t high = 0u;
};

struct TerrainGPUColumnResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPUColumnStatus::Empty);
  uint32_t validationErrors = 0u;
  uint32_t missingSamples = 0u;
  uint32_t verticalOverflows = 0u;
  uint32_t columnCount = 0u;
  uint32_t cornerCount = 0u;
  uint32_t boundaryCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t atlasRevisionLow = 0u;
  uint32_t atlasRevisionHigh = 0u;
  uint32_t generatorRevisionLow = 0u;
  uint32_t generatorRevisionHigh = 0u;
  uint32_t semanticHashLow = 0u;
  uint32_t semanticHashHigh = 0u;
  uint32_t elevationOriginBits = 0u;
  uint32_t occupiedVoxelCount = 0u;
};

static_assert(sizeof(TerrainGPUColumnConfigGPU) == 224u);
static_assert(sizeof(TerrainGPUHeight64GPU) == 8u);
static_assert(sizeof(TerrainGPUColumnResultGPU) == 64u);
inline constexpr size_t kTerrainGPUColumnOccupiedVoxelCountOffset = offsetof(TerrainGPUColumnResultGPU, occupiedVoxelCount);

bool planTerrainGPUColumnBuild(
    const procedural::world::MapExportBundle &bundle,
    const procedural::world::MapExportGpuPageLayout &layout,
    const TerrainGPUColumnBuildSettings &settings,
    TerrainGPUColumnConfigGPU &config,
    std::string *error = nullptr);

// CPU-side metadata probe of the GPU column shader's surface function. This
// samples only immutable source-map fields; it does not construct, cache, or
// read back terrain columns/voxels. Runtime camera placement and diagnostics
// use it when generated geometry is exclusively GPU-owned.
bool sampleTerrainGPUWorldMapSurfaceHeight(
    const procedural::world::MapExportBundle &bundle,
    uint8_t requestedLevel,
    const PlanetVoxelChunkSettings &settings,
    double normalizedU,
    double normalizedV,
    float &heightMetres,
    std::string *error = nullptr);

// CPU mirror of stages 2/3. Tests feed it decoded atlas samples, which keeps
// shell ownership differential tests independent of either sampling backend.
struct TerrainGPUColumnSurfaceSamples
{
  // (side + 2 * halo + 1)^2, including the corner halo.
  std::vector<float> cornerHeights;
  // side^2 material IDs selected at column centres.
  std::vector<uint32_t> materials;
  // Optional four faces * side values. Supplying these mirrors boundary
  // subdivision samples; empty arrays use the two canonical endpoints.
  std::vector<float> boundaryMinimumHeights;
  std::vector<float> boundaryMaximumHeights;
};

bool buildTerrainGPUColumnOracle(const TerrainGPUColumnConfigGPU &config, const TerrainGPUColumnSurfaceSamples &samples, WorldMapTerrainColumnPage &page, std::string *error = nullptr);

uint64_t terrainGPUColumnSemanticHash(const WorldMapTerrainColumnPage &page);

} // namespace voxel::svraster
