#pragma once

#include "procedural/world/MapExportGpuCache.hpp"
#include "procedural/world/MapExportSampler.hpp"
#include "voxel/svraster/TerrainGPUChunkContract.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct PlanetVoxelChunkSettings;

// Keep this table in lockstep with the production world-map terrain sampler.
// V2 originally uploaded only the eight fields required by its prototype
// height function.  That made its pages deterministic, but deterministically
// different from the CPU terrain at every spatial LOD.
inline constexpr uint32_t kTerrainGPUV2ColumnFieldCount = 20u;
inline constexpr uint32_t kTerrainGPUV2ColumnInvalidField = 0xffffffffu;
inline constexpr uint32_t kTerrainGPUV2ColumnMaximumSide = 256u;
inline constexpr uint32_t kTerrainGPUV2ColumnMaximumHalo = 8u;
inline constexpr uint32_t kTerrainGPUV2ColumnGeneratorRevision = 4u;
inline constexpr uint32_t kTerrainGPUV2ColumnMaximumClipmapLOD = 30u;
inline constexpr uint32_t kTerrainGPUV2HeightSampleFlagMask = 0xffu;
inline constexpr uint32_t kTerrainGPUV2HeightSampleResidualShift = 8u;
inline constexpr uint32_t kTerrainGPUV2HeightSampleResidualBits = 24u;
inline constexpr uint32_t kTerrainGPUV2HeightSampleResidualMask =
    (1u << kTerrainGPUV2HeightSampleResidualBits) - 1u;

enum class TerrainGPUV2ColumnField : uint32_t
{
  Elevation,
  BaseElevation,
  Uplift,
  Relief,
  Landform,
  WaterType,
  WaterLevel,
  River,
  Coast,
  Erosion,
  Deposition,
  Substrate,
  SoilDepth,
  BiomeIds,
  BiomeWeights,
  Climate,
  Placement,
  SoilLayers,
  Roads,
  SettlementOrExclusion,
};

enum class TerrainGPUV2ColumnStatus : uint32_t
{
  Empty,
  Building,
  Ready,
  MissingPage,
  InvalidInput,
  QuantizationOverflow,
  StaleAtlas,
};

enum TerrainGPUV2ColumnFlags : uint32_t
{
  TerrainGPUV2ColumnValid = 1u << 0u,
  TerrainGPUV2ColumnWater = 1u << 1u,
};

struct TerrainGPUV2ColumnTileShape
{
  uint16_t columnsX = 64u;
  uint16_t columnsZ = 64u;
  uint16_t halo = 2u;
  uint16_t reserved = 0u;

  bool operator==(const TerrainGPUV2ColumnTileShape &other) const;
};

struct TerrainGPUV2ColumnTileKey
{
  uint64_t worldBundleRevision = 0u;
  int32_t tileX = 0;
  int32_t tileZ = 0;
  uint32_t clipmapLOD = 0u;
  uint32_t terrainSlot = 0u;
  uint32_t expectedGeneration = 0u;
  TerrainGPUV2ColumnTileShape shape{};

  bool operator==(const TerrainGPUV2ColumnTileKey &other) const;
  bool operator<(const TerrainGPUV2ColumnTileKey &other) const;
};

struct TerrainGPUV2ColumnBuildSettings
{
  TerrainGPUV2ColumnTileKey key{};
  uint32_t requestedMapLevel = 0u;
  std::array<int32_t, 2u> worldOriginCanonicalXZ{};
  uint32_t worldExtentCanonical = 0u;
  float heightUnitMetres = 0.0f;
  double proceduralDetailStrength = 0.0;
  double proceduralTerraceStrength = 0.0;
  double proceduralTerraceHeightMetres = 4.0;
  double proceduralTerraceTransitionFraction = 0.16;
  uint64_t expectedAtlasRevision = 0u;
  uint32_t faultCapacity = 1024u;
};

// Exactly 128 bytes and free of bools so the buffer is directly consumed by
// WGSL. X/Z positions and output heights stay on canonical integer lattices.
struct alignas(16) TerrainGPUV2ColumnConfigGPU
{
  uint32_t columnsX = 0u;
  uint32_t columnsZ = 0u;
  uint32_t halo = 0u;
  uint32_t requestedMapLevel = 0u;
  int32_t tileMinimumCanonicalX = 0;
  int32_t tileMinimumCanonicalZ = 0;
  int32_t worldOriginCanonicalX = 0;
  int32_t worldOriginCanonicalZ = 0;
  uint32_t canonicalStep = 0u;
  uint32_t worldExtentCanonical = 0u;
  float heightUnitMetres = 0.0f;
  uint32_t expectedTerrainGeneration = 0u;
  uint32_t expectedAtlasRevisionLow = 0u;
  uint32_t expectedAtlasRevisionHigh = 0u;
  uint32_t generatorRevision = kTerrainGPUV2ColumnGeneratorRevision;
  uint32_t faultCapacity = 0u;
  std::array<uint32_t, kTerrainGPUV2ColumnFieldCount> fields{};
  uint32_t seedLow = 0u;
  uint32_t seedHigh = 0u;
  uint32_t bundleSignatureLow = 0u;
  uint32_t bundleSignatureHigh = 0u;
  float proceduralDetailStrength = 0.0f;
  float proceduralTerraceStrength = 0.0f;
  float proceduralTerraceHeightMetres = 4.0f;
  float proceduralTerraceTransitionFraction = 0.16f;
  std::array<uint32_t, 4u> reserved{};
};

struct TerrainGPUV2HeightSampleGPU
{
  int32_t heightUnits = 0;
  int32_t canonicalX = 0;
  int32_t canonicalZ = 0;
  uint32_t flags = 0u;
};

inline uint32_t terrainGPUV2HeightSampleFlags(const TerrainGPUV2HeightSampleGPU &sample)
{
  return sample.flags & kTerrainGPUV2HeightSampleFlagMask;
}

inline uint32_t terrainGPUV2PackHeightSampleFlags(
    const uint32_t flags,
    const double heightMetres,
    const double heightUnitMetres,
    const int32_t heightUnits)
{
  if (!std::isfinite(heightMetres) || !std::isfinite(heightUnitMetres) ||
      !(heightUnitMetres > 0.0) || (flags & TerrainGPUV2ColumnValid) == 0u)
    return flags & kTerrainGPUV2HeightSampleFlagMask;
  const double continuousUnits = heightMetres / heightUnitMetres;
  const double residual = std::clamp(
      static_cast<double>(heightUnits) - continuousUnits, 0.0, 1.0);
  const auto quantized = static_cast<uint32_t>(std::llround(
      residual * static_cast<double>(kTerrainGPUV2HeightSampleResidualMask)));
  return (flags & kTerrainGPUV2HeightSampleFlagMask) |
      (std::min(quantized, kTerrainGPUV2HeightSampleResidualMask)
          << kTerrainGPUV2HeightSampleResidualShift);
}

inline double terrainGPUV2HeightSampleMetres(
    const TerrainGPUV2HeightSampleGPU &sample,
    const double heightUnitMetres)
{
  if (!std::isfinite(heightUnitMetres) || !(heightUnitMetres > 0.0) ||
      (terrainGPUV2HeightSampleFlags(sample) & TerrainGPUV2ColumnValid) == 0u)
    return static_cast<double>(sample.heightUnits) * heightUnitMetres;
  const uint32_t residual =
      (sample.flags >> kTerrainGPUV2HeightSampleResidualShift) &
      kTerrainGPUV2HeightSampleResidualMask;
  const double residualUnits =
      static_cast<double>(residual) /
      static_cast<double>(kTerrainGPUV2HeightSampleResidualMask);
  return (static_cast<double>(sample.heightUnits) - residualUnits) *
      heightUnitMetres;
}

struct TerrainGPUV2ColumnGPU
{
  int32_t minimumHeightUnits = 0;
  int32_t maximumHeightUnits = 0;
  uint32_t material = 0u;
  uint32_t flags = 0u;
};

struct TerrainGPUV2MapFaultGPU
{
  uint32_t field = 0u;
  uint32_t level = 0u;
  uint32_t pageX = 0u;
  uint32_t pageZ = 0u;

  bool operator==(const TerrainGPUV2MapFaultGPU &other) const;
  bool operator<(const TerrainGPUV2MapFaultGPU &other) const;
};

enum class TerrainGPUV2SemanticFallbackKind : uint32_t
{
  None,
  AncestorPage,
  UnavailableField,
  MissingPage,
  InvalidInput,
};

struct TerrainGPUV2SemanticFieldSample
{
  std::string fieldName;
  uint32_t fieldSlot = kTerrainGPUV2ColumnInvalidField;
  uint32_t atlasFieldIndex = kTerrainGPUV2ColumnInvalidField;
  bool required = false;
  TerrainGPUV2SemanticFallbackKind fallback = TerrainGPUV2SemanticFallbackKind::None;
  procedural::world::MapExportPageAddress requestedPage{};
  procedural::world::MapExportPageAddress resolvedPage{};
  std::array<double, 4u> values{};
  uint32_t componentCount = 0u;
};

struct TerrainGPUV2SemanticBiomeSample
{
  TerrainGPUV2SemanticFallbackKind fallback = TerrainGPUV2SemanticFallbackKind::None;
  procedural::world::MapExportPageAddress requestedPage{};
  procedural::world::MapExportPageAddress resolvedPage{};
  std::array<uint8_t, 4u> ids{};
  std::array<double, 4u> weights{};
  uint32_t count = 0u;
  uint8_t dominantBiome = 0u;
};

struct TerrainGPUV2SemanticSampleReport
{
  std::vector<TerrainGPUV2SemanticFieldSample> fields;
  TerrainGPUV2SemanticBiomeSample biome{};
  uint32_t sampledFields = 0u;
  uint32_t ancestorFallbacks = 0u;
  uint32_t unavailableFieldFallbacks = 0u;
  uint32_t missingPageFaults = 0u;
  uint32_t invalidInputFaults = 0u;
  uint32_t material = 0u;
  uint32_t materialFlags = 0u;
};

struct TerrainGPUV2ColumnResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPUV2ColumnStatus::Empty);
  uint32_t missingSamples = 0u;
  uint32_t faultCount = 0u;
  uint32_t quantizationOverflows = 0u;
  uint32_t heightSampleCount = 0u;
  uint32_t columnCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t writtenColumns = 0u;
  uint32_t atlasRevisionLow = 0u;
  uint32_t atlasRevisionHigh = 0u;
  uint32_t generatorRevision = 0u;
  uint32_t selectedWorkgroup = 0u;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
  uint32_t reserved2 = 0u;
  uint32_t reserved3 = 0u;
};

static_assert(sizeof(TerrainGPUV2ColumnTileShape) == 8u);
static_assert(sizeof(TerrainGPUV2ColumnConfigGPU) == 192u);
static_assert(sizeof(TerrainGPUV2HeightSampleGPU) == 16u);
static_assert(sizeof(TerrainGPUV2ColumnGPU) == 16u);
static_assert(sizeof(TerrainGPUV2MapFaultGPU) == 16u);
static_assert(sizeof(TerrainGPUV2ColumnResultGPU) == 64u);

struct TerrainGPUV2ColumnWorkgroupCandidate
{
  uint32_t x = 8u;
  uint32_t z = 8u;
  uint32_t elapsedMicroseconds = 0u;
};

bool terrainGPUV2SelectColumnWorkgroup(const std::vector<TerrainGPUV2ColumnWorkgroupCandidate> &measurements, uint32_t maximumInvocations, TerrainGPUV2ColumnWorkgroupCandidate &selected, std::string *error = nullptr);

bool terrainGPUV2ColumnTileKey(const TerrainGPUChunkKey &chunk, const TerrainGPUChunkShape &chunkShape, uint32_t halo, TerrainGPUV2ColumnTileKey &tile, std::string *error = nullptr);

bool planTerrainGPUV2Columns(
    const procedural::world::MapExportBundle &bundle,
    const procedural::world::MapExportGpuPageLayout &layout,
    const TerrainGPUV2ColumnBuildSettings &settings,
    TerrainGPUV2ColumnConfigGPU &config,
    std::string *error = nullptr);

// CPU probe of the exact height expression used by the V2 column shader.
// This is intentionally limited to camera placement and diagnostics; generated
// column arrays remain GPU-owned. Keeping the expression shared with the V2
// oracle prevents a close-ground camera from being anchored against the older
// procedural-column formula and ending up inside the generated solid volume.
bool sampleTerrainGPUV2SurfaceHeight(const procedural::world::MapExportBundle &bundle, uint8_t requestedMapLevel, double normalizedU, double normalizedV, double &heightMetres, std::string *error = nullptr);

// Production-settings overload used by runtime camera anchoring and parity
// diagnostics.  The short overload above intentionally retains zero detail
// defaults for synthetic/unit-test bundles.
bool sampleTerrainGPUV2SurfaceHeight(
    const procedural::world::MapExportBundle &bundle,
    const PlanetVoxelChunkSettings &settings,
    uint8_t requestedMapLevel,
    double normalizedU,
    double normalizedV,
    double &heightMetres,
    std::string *error = nullptr);

// CPU-visible oracle for the semantic map inputs consumed by V2 terrain
// generation.  It records the same page resolution decisions MapExportSampler
// makes (including gutters, unit conversion, nearest categorical filtering,
// paired biome normalization and ancestor fallback) so GPU-generated terrain
// chunks can be validated without reading generated terrain payloads back.
bool sampleTerrainGPUV2SemanticInputs(
    const procedural::world::MapExportBundle &bundle,
    const TerrainGPUV2ColumnConfigGPU &config,
    double normalizedU,
    double normalizedV,
    TerrainGPUV2SemanticSampleReport &report,
    std::vector<TerrainGPUV2MapFaultGPU> *faults = nullptr,
    std::string *error = nullptr);

bool terrainGPUV2ColumnFootprint(
    const procedural::world::MapExportBundle &bundle,
    const procedural::world::MapExportGpuPageLayout &layout,
    const TerrainGPUV2ColumnConfigGPU &config,
    procedural::world::MapExportStreamRequest &request,
    std::string *error = nullptr);

bool buildTerrainGPUV2ColumnOracle(
    const procedural::world::MapExportBundle &bundle,
    const TerrainGPUV2ColumnConfigGPU &config,
    std::vector<TerrainGPUV2HeightSampleGPU> &heights,
    std::vector<TerrainGPUV2ColumnGPU> &columns,
    std::string *error = nullptr);

enum class TerrainGPUV2ColumnTileState : uint32_t
{
  WaitingForAtlas,
  ReadyToBuild,
  Building,
  Ready,
  Faulted,
};

struct TerrainGPUV2ColumnTileRecord
{
  uint64_t id = 0u;
  TerrainGPUV2ColumnTileKey key{};
  TerrainGPUV2ColumnTileState state = TerrainGPUV2ColumnTileState::WaitingForAtlas;
  uint64_t atlasRevision = 0u;
  uint32_t consumerCount = 0u;
  uint32_t retryCount = 0u;
  bool releaseWhenIdle = false;
  std::vector<TerrainGPUV2MapFaultGPU> faults{};
};

// CPU control-plane registry. Vertical chunks with the same X/Z identity
// acquire one tile and therefore sample maps only once.
class TerrainGPUV2ColumnTileRegistry
{
public:
  bool acquire(uint64_t jobId, const TerrainGPUV2ColumnTileKey &key, uint64_t &tileId, bool &created, std::string *error = nullptr);
  bool release(uint64_t jobId, std::string *error = nullptr);
  bool markAtlasReady(uint64_t tileId, uint64_t atlasRevision, std::string *error = nullptr);
  bool beginBuild(uint64_t tileId, std::string *error = nullptr);
  bool completeBuild(uint64_t tileId, const TerrainGPUV2ColumnResultGPU &result, std::vector<TerrainGPUV2MapFaultGPU> faults, std::string *error = nullptr);
  std::optional<TerrainGPUV2ColumnTileRecord> tile(uint64_t tileId) const;
  std::optional<uint64_t> tileForJob(uint64_t jobId) const;
  uint32_t tileCount() const;
  uint32_t jobCount() const;

private:
  struct Entry
  {
    TerrainGPUV2ColumnTileRecord record{};
    std::vector<uint64_t> consumers{};
  };
  uint64_t nextTileId_ = 1u;
  std::map<TerrainGPUV2ColumnTileKey, uint64_t> idsByKey_{};
  std::map<uint64_t, Entry> entries_{};
  std::map<uint64_t, uint64_t> tileByJob_{};
};

} // namespace voxel::svraster
