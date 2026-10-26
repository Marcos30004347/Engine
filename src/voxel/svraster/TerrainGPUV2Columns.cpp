#include "voxel/svraster/TerrainGPUV2Columns.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

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

uint32_t low(const uint64_t value)
{
  return static_cast<uint32_t>(value);
}

uint32_t high(const uint64_t value)
{
  return static_cast<uint32_t>(value >> 32u);
}

bool available(const procedural::world::MapExportBundle &bundle, const char *name)
{
  const auto *field = bundle.field(name);
  return field != nullptr && field->available;
}

uint32_t biomeMaterial(const uint32_t biome)
{
  if (biome == 0u)
    return 0u;
  if (biome == 1u)
    return 1u;
  if (biome == 2u)
    return 2u;
  if (biome == 3u)
    return 8u;
  if (biome == 4u || biome == 5u || biome == 24u)
    return 9u;
  if (biome == 7u || biome == 10u)
    return 12u;
  if (biome == 8u)
    return 16u;
  if (biome == 9u)
    return 15u;
  if (biome == 16u || biome == 17u || biome == 18u)
    return 18u;
  if (biome == 15u)
    return 19u;
  if (biome == 19u || biome == 20u || biome == 6u)
    return 20u;
  if (biome == 11u || biome == 13u || biome == 14u || biome == 12u)
    return 13u;
  if (biome == 21u || biome == 22u || biome == 23u)
    return 7u;
  if (biome == 25u)
    return 4u;
  return 3u;
}

bool sampleField(const procedural::world::MapExportBundle &bundle, const char *name, const double u, const double v, const uint8_t level, procedural::world::MapExportFieldSample &sample, std::string *error)
{
  return procedural::world::sampleMapExportField(bundle, name, u, v, level, sample, error);
}

bool sampleOptionalField(const procedural::world::MapExportBundle &bundle, const char *name, const double u, const double v, const uint8_t level, procedural::world::MapExportFieldSample &sample, std::string *error)
{
  return !available(bundle, name) || sampleField(bundle, name, u, v, level, sample, error);
}

uint32_t columnFieldSlot(const TerrainGPUV2ColumnField field)
{
  return static_cast<uint32_t>(field);
}

uint32_t atlasFieldIndex(const TerrainGPUV2ColumnConfigGPU &config, const uint32_t slot)
{
  return slot < config.fields.size() ? config.fields[slot] : kTerrainGPUV2ColumnInvalidField;
}

bool samplingErrorIsMissingPage(const std::string &message)
{
  return message.find("no resident page") != std::string::npos || message.find("no common resident ancestor") != std::string::npos;
}

bool surfaceHeight(const procedural::world::MapExportBundle &bundle, const TerrainGPUV2ColumnConfigGPU &config, const double u, const double v, double &height, uint32_t &flags, std::string *error)
{
  procedural::world::MapExportFieldSample water{};
  procedural::world::MapExportFieldSample waterLevel{};
  if (!sampleField(bundle, "water_type", u, v, static_cast<uint8_t>(config.requestedMapLevel), water, error))
    return false;
  if (static_cast<uint32_t>(std::llround(water.values[0])) != 0u)
  {
    if (!sampleField(bundle, "water_level", u, v, static_cast<uint8_t>(config.requestedMapLevel), waterLevel, error))
      return false;
    height = waterLevel.values[0];
    flags |= TerrainGPUV2ColumnWater;
    return true;
  }
  procedural::world::MapExportFieldSample elevation{};
  procedural::world::MapExportFieldSample baseElevation{};
  procedural::world::MapExportFieldSample uplift{};
  procedural::world::MapExportFieldSample relief{};
  procedural::world::MapExportFieldSample landformSample{};
  procedural::world::MapExportFieldSample river{};
  procedural::world::MapExportFieldSample coast{};
  procedural::world::MapExportFieldSample erosion{};
  procedural::world::MapExportFieldSample deposition{};
  procedural::world::MapExportFieldSample roads{};
  procedural::world::MapExportFieldSample settlement{};
  procedural::world::MapExportFieldSample exclusion{};
  const char *elevationName = available(bundle, "refined_elevation") ? "refined_elevation" : "base_elevation";
  const uint8_t level = static_cast<uint8_t>(config.requestedMapLevel);
  if (!sampleField(bundle, elevationName, u, v, level, elevation, error) || !sampleField(bundle, "river", u, v, level, river, error) || !sampleOptionalField(bundle, "base_elevation", u, v, level, baseElevation, error) ||
      !sampleOptionalField(bundle, "uplift", u, v, level, uplift, error) || !sampleOptionalField(bundle, "relief_roughness", u, v, level, relief, error) ||
      !sampleOptionalField(bundle, "landform_id", u, v, level, landformSample, error) || !sampleOptionalField(bundle, "coast_distance", u, v, level, coast, error) ||
      !sampleOptionalField(bundle, "erosion_wear_flow", u, v, level, erosion, error) || !sampleOptionalField(bundle, "erosion_deposition", u, v, level, deposition, error) ||
      !sampleOptionalField(bundle, "roads", u, v, level, roads, error) || !sampleOptionalField(bundle, "settlement_activity", u, v, level, settlement, error) ||
      !sampleOptionalField(bundle, "object_exclusion", u, v, level, exclusion, error))
    return false;

  double infrastructure = roads.values[0] > 0.0 ? 1.0 : 0.0;
  if (available(bundle, "settlement_activity"))
  {
    const double activity = settlement.values[0] > 1.0 ? settlement.values[0] / 255.0 : settlement.values[0];
    infrastructure = std::max(infrastructure, std::clamp(activity, 0.0, 1.0));
  }
  if (available(bundle, "object_exclusion") && exclusion.values[0] > 0.0)
    infrastructure = 1.0;

  procedural::world::CellType landform = procedural::world::CellType::Hill;
  if (available(bundle, "landform_id"))
  {
    const auto value = static_cast<uint8_t>(std::clamp(std::llround(landformSample.values[0]), 0ll, static_cast<long long>(procedural::world::CellType::Count) - 1ll));
    landform = static_cast<procedural::world::CellType>(value);
  }
  WorldMapTerrainSignals signals{};
  signals.hasBaseElevation = available(bundle, "base_elevation");
  signals.baseElevationMetres = baseElevation.values[0];
  signals.upliftMetresPerYear = uplift.values[0];
  signals.erosionWear = erosion.values[0];
  signals.erosionFlow = erosion.values[1];
  signals.erosionDeposition = deposition.values[0];
  signals.riverDischargeCubicMetresPerSecond = river.values[1];
  signals.coastDistanceMetres = available(bundle, "coast_distance") ? coast.values[0] : -1.0;
  const procedural::Point2 position{u * bundle.extentMetres, v * bundle.extentMetres};
  height = proceduralWorldMapTerrainHeight(
      position,
      bundle,
      elevation.values[0],
      relief.values[0],
      relief.values[1],
      landform,
      config.proceduralDetailStrength,
      config.proceduralTerraceStrength,
      config.proceduralTerraceHeightMetres,
      config.proceduralTerraceTransitionFraction,
      river.values[0],
      infrastructure,
      signals);
  return true;
}

bool surfaceMaterial(const procedural::world::MapExportBundle &bundle, const TerrainGPUV2ColumnConfigGPU &config, const double u, const double v, uint32_t &material, uint32_t &flags, std::string *error)
{
  procedural::world::MapExportFieldSample water{};
  procedural::world::MapExportFieldSample substrate{};
  procedural::world::MapExportFieldSample soil{};
  procedural::world::MapExportFieldSample river{};
  procedural::world::MapExportFieldSample climate{};
  procedural::world::MapExportFieldSample placement{};
  procedural::world::MapExportFieldSample soilLayers{};
  const uint8_t level = static_cast<uint8_t>(config.requestedMapLevel);
  if (!sampleField(bundle, "water_type", u, v, level, water, error) || !sampleField(bundle, "substrate_id", u, v, level, substrate, error) || !sampleField(bundle, "soil_depth", u, v, level, soil, error) ||
      !sampleField(bundle, "river", u, v, level, river, error) || !sampleOptionalField(bundle, "climate", u, v, level, climate, error) ||
      !sampleOptionalField(bundle, "placement_potential", u, v, level, placement, error) || !sampleOptionalField(bundle, "soil_layers", u, v, level, soilLayers, error))
    return false;
  const uint32_t waterType = static_cast<uint32_t>(std::llround(water.values[0]));
  if (waterType != 0u)
  {
    material = waterType == 1u ? 0u : 1u;
    flags |= TerrainGPUV2ColumnWater;
    return true;
  }
  procedural::world::MapExportBiomeSample biome{};
  if (!procedural::world::sampleMapExportBiome(bundle, u, v, level, biome, error))
    return false;
  const procedural::Point2 position{u * bundle.extentMetres, v * bundle.extentMetres};
  material = biomeMaterial(worldMapBiomeForColumn(biome, position, bundle, config.heightUnitMetres));
  const uint32_t substrateId = static_cast<uint32_t>(std::llround(substrate.values[0]));
  const bool exposedRock = (available(bundle, "placement_potential") && placement.values[3] >= 0.58 && soil.values[0] < 0.5) || (available(bundle, "soil_layers") && soilLayers.values[0] >= 0.62);
  if (soil.values[0] < 0.15 || substrateId == 1u || substrateId == 2u || exposedRock)
    material = 4u;
  if (available(bundle, "climate") && climate.values[0] <= -2.0 && climate.values[1] >= 0.12)
    material = 5u;
  const double discharge = std::max(river.values[1], 0.0);
  const double voxelSize = config.heightUnitMetres * config.canonicalStep;
  const double halfWidth = discharge > 0.0 ? std::clamp(5.0 + std::sqrt(discharge) * 8.0, std::max(2.0, voxelSize * 0.75), 120.0) : 0.0;
  if (available(bundle, "soil_layers") && soilLayers.values[2] + soilLayers.values[3] >= 0.48 && climate.values[1] >= 0.55 && river.values[0] <= std::max(halfWidth * 3.0, 30.0))
    material = 7u;
  if (discharge > 0.0 && river.values[0] <= halfWidth)
    material = 6u;
  return true;
}

} // namespace

bool sampleTerrainGPUV2SurfaceHeight(const procedural::world::MapExportBundle &bundle, const uint8_t requestedMapLevel, const double normalizedU, const double normalizedV, double &heightMetres, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!std::isfinite(normalizedU) || !std::isfinite(normalizedV) || requestedMapLevel > bundle.maximumPageLevel)
    return fail(error, "terrain GPU V2 surface probe settings are invalid");
  TerrainGPUV2ColumnConfigGPU config{};
  config.requestedMapLevel = requestedMapLevel;
  uint32_t flags = TerrainGPUV2ColumnValid;
  return surfaceHeight(bundle, config, std::clamp(normalizedU, 0.0, 1.0), std::clamp(normalizedV, 0.0, 1.0), heightMetres, flags, error);
}

bool sampleTerrainGPUV2SurfaceHeight(
    const procedural::world::MapExportBundle &bundle,
    const PlanetVoxelChunkSettings &settings,
    const uint8_t requestedMapLevel,
    const double normalizedU,
    const double normalizedV,
    double &heightMetres,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!std::isfinite(normalizedU) || !std::isfinite(normalizedV) || requestedMapLevel > bundle.maximumPageLevel)
    return fail(error, "terrain GPU V2 surface probe settings are invalid");
  TerrainGPUV2ColumnConfigGPU config{};
  config.requestedMapLevel = requestedMapLevel;
  config.proceduralDetailStrength = static_cast<float>(settings.proceduralDetailStrength);
  config.proceduralTerraceStrength = static_cast<float>(settings.proceduralTerraceStrength);
  config.proceduralTerraceHeightMetres = static_cast<float>(settings.proceduralTerraceHeightMetres);
  config.proceduralTerraceTransitionFraction = static_cast<float>(settings.proceduralTerraceTransitionFraction);
  uint32_t flags = TerrainGPUV2ColumnValid;
  return surfaceHeight(bundle, config, std::clamp(normalizedU, 0.0, 1.0), std::clamp(normalizedV, 0.0, 1.0), heightMetres, flags, error);
}

bool sampleTerrainGPUV2SemanticInputs(
    const procedural::world::MapExportBundle &bundle,
    const TerrainGPUV2ColumnConfigGPU &config,
    const double normalizedU,
    const double normalizedV,
    TerrainGPUV2SemanticSampleReport &report,
    std::vector<TerrainGPUV2MapFaultGPU> *faults,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (faults != nullptr)
    faults->clear();
  report = {};
  if (!std::isfinite(normalizedU) || !std::isfinite(normalizedV) || config.requestedMapLevel > bundle.maximumPageLevel || config.requestedMapLevel > std::numeric_limits<uint8_t>::max())
  {
    ++report.invalidInputFaults;
    return fail(error, "terrain GPU V2 semantic sample settings are invalid");
  }

  const double u = std::clamp(normalizedU, 0.0, 1.0);
  const double v = std::clamp(normalizedV, 0.0, 1.0);
  const uint8_t level = static_cast<uint8_t>(config.requestedMapLevel);
  const auto addFault = [&](const uint32_t slot, const procedural::world::MapExportPageAddress &page)
  {
    if (faults == nullptr)
      return;
    faults->push_back({slot, page.level, page.x, page.y});
  };
  const auto classifyFailure = [&](TerrainGPUV2SemanticFieldSample &record, const std::string &sampleError, const uint32_t slot)
  {
    if (samplingErrorIsMissingPage(sampleError))
    {
      record.fallback = TerrainGPUV2SemanticFallbackKind::MissingPage;
      ++report.missingPageFaults;
      addFault(slot, record.requestedPage);
    }
    else
    {
      record.fallback = TerrainGPUV2SemanticFallbackKind::InvalidInput;
      ++report.invalidInputFaults;
    }
  };
  const auto sampleOne = [&](const char *name, const uint32_t slot, const bool required)
  {
    TerrainGPUV2SemanticFieldSample record{};
    record.fieldName = name;
    record.fieldSlot = slot;
    record.atlasFieldIndex = atlasFieldIndex(config, slot);
    record.required = required;
    procedural::world::mapExportPageAt(bundle, u, v, level, record.requestedPage);

    const auto *field = bundle.field(name);
    if (field == nullptr || !field->available)
    {
      record.fallback = TerrainGPUV2SemanticFallbackKind::UnavailableField;
      ++report.unavailableFieldFallbacks;
      report.fields.push_back(std::move(record));
      if (required)
        return fail(error, "terrain GPU V2 required semantic field is unavailable: " + std::string{name});
      return true;
    }

    procedural::world::MapExportFieldSample sample{};
    std::string sampleError;
    if (!procedural::world::sampleMapExportField(bundle, name, u, v, level, sample, &sampleError))
    {
      record.requestedPage = sample.requestedPage.level != 0u || sample.requestedPage.x != 0u || sample.requestedPage.y != 0u ? sample.requestedPage : record.requestedPage;
      classifyFailure(record, sampleError, slot);
      report.fields.push_back(std::move(record));
      return fail(error, "terrain GPU V2 semantic field sample failed for " + std::string{name} + ": " + sampleError);
    }

    record.requestedPage = sample.requestedPage;
    record.resolvedPage = sample.resolvedPage;
    record.values = sample.values;
    record.componentCount = sample.componentCount;
    if (sample.usedAncestor())
    {
      record.fallback = TerrainGPUV2SemanticFallbackKind::AncestorPage;
      ++report.ancestorFallbacks;
    }
    ++report.sampledFields;
    report.fields.push_back(std::move(record));
    return true;
  };

  const char *elevationName = available(bundle, "refined_elevation") ? "refined_elevation" : "base_elevation";
  struct Probe
  {
    const char *name = "";
    uint32_t slot = kTerrainGPUV2ColumnInvalidField;
    bool required = false;
  };
  const std::array<Probe, 27u> probes{{
    {elevationName, columnFieldSlot(TerrainGPUV2ColumnField::Elevation), true},
    {"base_elevation", columnFieldSlot(TerrainGPUV2ColumnField::BaseElevation), false},
    {"uplift", columnFieldSlot(TerrainGPUV2ColumnField::Uplift), false},
    {"relief_roughness", columnFieldSlot(TerrainGPUV2ColumnField::Relief), false},
    {"landform_id", columnFieldSlot(TerrainGPUV2ColumnField::Landform), false},
    {"water_type", columnFieldSlot(TerrainGPUV2ColumnField::WaterType), true},
    {"water_level", columnFieldSlot(TerrainGPUV2ColumnField::WaterLevel), true},
    {"river", columnFieldSlot(TerrainGPUV2ColumnField::River), true},
    {"coast_distance", columnFieldSlot(TerrainGPUV2ColumnField::Coast), false},
    {"erosion_wear_flow", columnFieldSlot(TerrainGPUV2ColumnField::Erosion), false},
    {"erosion_deposition", columnFieldSlot(TerrainGPUV2ColumnField::Deposition), false},
    {"substrate_id", columnFieldSlot(TerrainGPUV2ColumnField::Substrate), true},
    {"soil_depth", columnFieldSlot(TerrainGPUV2ColumnField::SoilDepth), true},
    {"climate", columnFieldSlot(TerrainGPUV2ColumnField::Climate), false},
    {"placement_potential", columnFieldSlot(TerrainGPUV2ColumnField::Placement), false},
    {"soil_layers", columnFieldSlot(TerrainGPUV2ColumnField::SoilLayers), false},
    {"roads", columnFieldSlot(TerrainGPUV2ColumnField::Roads), false},
    {available(bundle, "settlement_activity") ? "settlement_activity" : "object_exclusion", columnFieldSlot(TerrainGPUV2ColumnField::SettlementOrExclusion), false},
    {"region_id", kTerrainGPUV2ColumnInvalidField, false},
    {"rock_potential", kTerrainGPUV2ColumnInvalidField, false},
    {"rock_type", kTerrainGPUV2ColumnInvalidField, false},
    {"exposed_bedrock", kTerrainGPUV2ColumnInvalidField, false},
    {"vegetation_potential", kTerrainGPUV2ColumnInvalidField, false},
    {"vegetation_type", kTerrainGPUV2ColumnInvalidField, false},
    {"canopy_density", kTerrainGPUV2ColumnInvalidField, false},
    {"ground_density", kTerrainGPUV2ColumnInvalidField, false},
    {"riparian_mask", kTerrainGPUV2ColumnInvalidField, false},
  }};
  for (const auto &probe : probes)
    if (!sampleOne(probe.name, probe.slot, probe.required))
      return false;

  procedural::world::MapExportBiomeSample biome{};
  std::string biomeError;
  if (!procedural::world::sampleMapExportBiome(bundle, u, v, level, biome, &biomeError))
  {
    report.biome.fallback = samplingErrorIsMissingPage(biomeError) ? TerrainGPUV2SemanticFallbackKind::MissingPage : TerrainGPUV2SemanticFallbackKind::InvalidInput;
    procedural::world::mapExportPageAt(bundle, u, v, level, report.biome.requestedPage);
    if (report.biome.fallback == TerrainGPUV2SemanticFallbackKind::MissingPage)
    {
      ++report.missingPageFaults;
      addFault(columnFieldSlot(TerrainGPUV2ColumnField::BiomeIds), report.biome.requestedPage);
    }
    else
      ++report.invalidInputFaults;
    return fail(error, "terrain GPU V2 semantic biome sample failed: " + biomeError);
  }
  report.biome.requestedPage = biome.requestedPage;
  report.biome.resolvedPage = biome.resolvedPage;
  report.biome.ids = biome.ids;
  report.biome.weights = biome.weights;
  report.biome.count = biome.count;
  report.biome.dominantBiome = biome.dominantBiome();
  if (biome.usedAncestor())
  {
    report.biome.fallback = TerrainGPUV2SemanticFallbackKind::AncestorPage;
    ++report.ancestorFallbacks;
  }

  uint32_t material = 0u;
  uint32_t materialFlags = TerrainGPUV2ColumnValid;
  if (!surfaceMaterial(bundle, config, u, v, material, materialFlags, error))
    return false;
  report.material = material;
  report.materialFlags = materialFlags;
  return true;
}

bool TerrainGPUV2ColumnTileShape::operator==(const TerrainGPUV2ColumnTileShape &other) const
{
  return columnsX == other.columnsX && columnsZ == other.columnsZ && halo == other.halo;
}

bool TerrainGPUV2ColumnTileKey::operator==(const TerrainGPUV2ColumnTileKey &other) const
{
  return worldBundleRevision == other.worldBundleRevision && tileX == other.tileX && tileZ == other.tileZ && clipmapLOD == other.clipmapLOD && terrainSlot == other.terrainSlot &&
         expectedGeneration == other.expectedGeneration && shape == other.shape;
}

bool TerrainGPUV2ColumnTileKey::operator<(const TerrainGPUV2ColumnTileKey &other) const
{
  return std::tie(worldBundleRevision, tileX, tileZ, clipmapLOD, terrainSlot, expectedGeneration, shape.columnsX, shape.columnsZ, shape.halo) <
         std::tie(other.worldBundleRevision, other.tileX, other.tileZ, other.clipmapLOD, other.terrainSlot, other.expectedGeneration, other.shape.columnsX, other.shape.columnsZ, other.shape.halo);
}

bool TerrainGPUV2MapFaultGPU::operator==(const TerrainGPUV2MapFaultGPU &other) const
{
  return field == other.field && level == other.level && pageX == other.pageX && pageZ == other.pageZ;
}

bool TerrainGPUV2MapFaultGPU::operator<(const TerrainGPUV2MapFaultGPU &other) const
{
  return std::tie(field, level, pageX, pageZ) < std::tie(other.field, other.level, other.pageX, other.pageZ);
}

bool terrainGPUV2SelectColumnWorkgroup(const std::vector<TerrainGPUV2ColumnWorkgroupCandidate> &measurements, const uint32_t maximumInvocations, TerrainGPUV2ColumnWorkgroupCandidate &selected, std::string *error)
{
  selected = {};
  bool found = false;
  for (const auto &candidate : measurements)
  {
    const bool compiled = (candidate.x == 8u && candidate.z == 8u) || (candidate.x == 16u && candidate.z == 8u) || (candidate.x == 8u && candidate.z == 16u);
    const uint64_t invocations = uint64_t{candidate.x} * candidate.z;
    if (!compiled || candidate.elapsedMicroseconds == 0u || invocations > maximumInvocations)
      continue;
    if (!found || candidate.elapsedMicroseconds < selected.elapsedMicroseconds || (candidate.elapsedMicroseconds == selected.elapsedMicroseconds && std::tie(candidate.x, candidate.z) < std::tie(selected.x, selected.z)))
    {
      selected = candidate;
      found = true;
    }
  }
  return found || fail(error, "terrain GPU V2 has no qualified column workgroup measurement");
}

bool terrainGPUV2ColumnTileKey(const TerrainGPUChunkKey &chunk, const TerrainGPUChunkShape &chunkShape, const uint32_t halo, TerrainGPUV2ColumnTileKey &tile, std::string *error)
{
  tile = {};
  TerrainGPUChunkShapeCounts counts{};
  if (!terrainGPUChunkShapeCounts(chunkShape, counts, error) || counts.voxelDimensions[0] > kTerrainGPUV2ColumnMaximumSide || counts.voxelDimensions[2] > kTerrainGPUV2ColumnMaximumSide || halo == 0u ||
      halo > kTerrainGPUV2ColumnMaximumHalo)
    return fail(error, "terrain GPU V2 column tile shape is invalid");
  tile.worldBundleRevision = chunk.worldBundleRevision;
  tile.tileX = chunk.chunkX;
  tile.tileZ = chunk.chunkZ;
  tile.clipmapLOD = chunk.clipmapLOD;
  tile.terrainSlot = chunk.terrainSlot;
  tile.expectedGeneration = chunk.expectedGeneration;
  tile.shape.columnsX = static_cast<uint16_t>(counts.voxelDimensions[0]);
  tile.shape.columnsZ = static_cast<uint16_t>(counts.voxelDimensions[2]);
  tile.shape.halo = static_cast<uint16_t>(halo);
  return true;
}

bool planTerrainGPUV2Columns(
    const procedural::world::MapExportBundle &bundle,
    const procedural::world::MapExportGpuPageLayout &layout,
    const TerrainGPUV2ColumnBuildSettings &settings,
    TerrainGPUV2ColumnConfigGPU &config,
    std::string *error)
{
  using procedural::world::MapExportFilter;
  using procedural::world::MapExportFormat;
  config = {};
  config.fields.fill(kTerrainGPUV2ColumnInvalidField);
  const auto &shape = settings.key.shape;
  if (shape.columnsX == 0u || shape.columnsZ == 0u || shape.columnsX > kTerrainGPUV2ColumnMaximumSide || shape.columnsZ > kTerrainGPUV2ColumnMaximumSide || shape.halo == 0u ||
      shape.halo > kTerrainGPUV2ColumnMaximumHalo || settings.key.clipmapLOD > kTerrainGPUV2ColumnMaximumClipmapLOD || settings.requestedMapLevel > bundle.maximumPageLevel || settings.worldExtentCanonical == 0u ||
      !(settings.heightUnitMetres > 0.0f) || !std::isfinite(settings.heightUnitMetres) || settings.faultCapacity == 0u || !std::isfinite(settings.proceduralDetailStrength) || settings.proceduralDetailStrength < 0.0 ||
      !std::isfinite(settings.proceduralTerraceStrength) || settings.proceduralTerraceStrength < 0.0 || !std::isfinite(settings.proceduralTerraceHeightMetres) || !(settings.proceduralTerraceHeightMetres > 0.0) ||
      !std::isfinite(settings.proceduralTerraceTransitionFraction) || settings.proceduralTerraceTransitionFraction < 0.0 || settings.proceduralTerraceTransitionFraction >= 0.5)
    return fail(error, "terrain GPU V2 column settings are invalid");

  const uint64_t step = uint64_t{1u} << settings.key.clipmapLOD;
  const int64_t minimumX = int64_t{settings.worldOriginCanonicalXZ[0]} + int64_t{settings.key.tileX} * shape.columnsX * step;
  const int64_t minimumZ = int64_t{settings.worldOriginCanonicalXZ[1]} + int64_t{settings.key.tileZ} * shape.columnsZ * step;
  const int64_t outerMinimumX = minimumX - int64_t{shape.halo} * step;
  const int64_t outerMinimumZ = minimumZ - int64_t{shape.halo} * step;
  const int64_t outerMaximumX = minimumX + (uint64_t{shape.columnsX} + shape.halo) * step;
  const int64_t outerMaximumZ = minimumZ + (uint64_t{shape.columnsZ} + shape.halo) * step;
  if (step > std::numeric_limits<uint32_t>::max() || minimumX < std::numeric_limits<int32_t>::min() || minimumX > std::numeric_limits<int32_t>::max() || minimumZ < std::numeric_limits<int32_t>::min() ||
      minimumZ > std::numeric_limits<int32_t>::max() || outerMinimumX < std::numeric_limits<int32_t>::min() || outerMinimumZ < std::numeric_limits<int32_t>::min() || outerMaximumX > std::numeric_limits<int32_t>::max() ||
      outerMaximumZ > std::numeric_limits<int32_t>::max())
    return fail(error, "terrain GPU V2 column coordinates exceed the shader domain");

  const std::array<const char *, kTerrainGPUV2ColumnFieldCount> names{
    available(bundle, "refined_elevation") ? "refined_elevation" : "base_elevation",
    "base_elevation",
    "uplift",
    "relief_roughness",
    "landform_id",
    "water_type",
    "water_level",
    "river",
    "coast_distance",
    "erosion_wear_flow",
    "erosion_deposition",
    "substrate_id",
    "soil_depth",
    "biome_mix_ids",
    "biome_mix_weights",
    "climate",
    "placement_potential",
    "soil_layers",
    "roads",
    available(bundle, "settlement_activity") ? "settlement_activity" : "object_exclusion"};
  const std::array<MapExportFormat, kTerrainGPUV2ColumnFieldCount> formats{MapExportFormat::R16Unorm,  MapExportFormat::R16Unorm,   MapExportFormat::R16Float,   MapExportFormat::RG16Float, MapExportFormat::R8Uint,
                                                                           MapExportFormat::R8Uint,    MapExportFormat::R16Float,   MapExportFormat::RG16Float,  MapExportFormat::R16Float,  MapExportFormat::RG16Float,
                                                                           MapExportFormat::R16Float,  MapExportFormat::R8Uint,     MapExportFormat::R16Float,   MapExportFormat::RGBA8Uint, MapExportFormat::RGBA8Unorm,
                                                                           MapExportFormat::RG16Float, MapExportFormat::RGBA8Unorm, MapExportFormat::RGBA8Unorm, MapExportFormat::R8Uint,    MapExportFormat::R8Uint};
  const std::array<MapExportFilter, kTerrainGPUV2ColumnFieldCount> filters{MapExportFilter::Linear,  MapExportFilter::Linear,  MapExportFilter::Linear, MapExportFilter::Linear,        MapExportFilter::Nearest,
                                                                           MapExportFilter::Nearest, MapExportFilter::Linear,  MapExportFilter::Linear, MapExportFilter::Linear,        MapExportFilter::Linear,
                                                                           MapExportFilter::Linear,  MapExportFilter::Nearest, MapExportFilter::Linear, MapExportFilter::SparseMixture, MapExportFilter::SparseMixture,
                                                                           MapExportFilter::Linear,  MapExportFilter::Linear,  MapExportFilter::Linear, MapExportFilter::Nearest,       MapExportFilter::Nearest};
  for (uint32_t index = 0u; index < names.size(); ++index)
  {
    const auto field = layout.fieldIndex(names[index]);
    const bool required = index == static_cast<uint32_t>(TerrainGPUV2ColumnField::Elevation) ||
                          (index >= static_cast<uint32_t>(TerrainGPUV2ColumnField::WaterType) && index <= static_cast<uint32_t>(TerrainGPUV2ColumnField::River)) ||
                          index == static_cast<uint32_t>(TerrainGPUV2ColumnField::Substrate) || index == static_cast<uint32_t>(TerrainGPUV2ColumnField::SoilDepth) ||
                          index == static_cast<uint32_t>(TerrainGPUV2ColumnField::BiomeIds) || index == static_cast<uint32_t>(TerrainGPUV2ColumnField::BiomeWeights);
    if ((!field && required) || (field && (layout.fields()[*field].format != formats[index] || layout.fields()[*field].filter != filters[index])))
      return fail(error, "terrain GPU V2 column atlas field contract is invalid");
    if (field)
      config.fields[index] = *field;
  }
  const auto *biomeIds = bundle.field("biome_mix_ids");
  const auto *biomeWeights = bundle.field("biome_mix_weights");
  if (biomeIds == nullptr || biomeWeights == nullptr || biomeIds->samplingGroup.empty() || biomeIds->samplingGroup != biomeWeights->samplingGroup)
    return fail(error, "terrain GPU V2 biome fields are not an atomic sampling group");

  config.columnsX = shape.columnsX;
  config.columnsZ = shape.columnsZ;
  config.halo = shape.halo;
  config.requestedMapLevel = settings.requestedMapLevel;
  config.tileMinimumCanonicalX = static_cast<int32_t>(minimumX);
  config.tileMinimumCanonicalZ = static_cast<int32_t>(minimumZ);
  config.worldOriginCanonicalX = settings.worldOriginCanonicalXZ[0];
  config.worldOriginCanonicalZ = settings.worldOriginCanonicalXZ[1];
  config.canonicalStep = static_cast<uint32_t>(step);
  config.worldExtentCanonical = settings.worldExtentCanonical;
  config.heightUnitMetres = settings.heightUnitMetres;
  config.expectedTerrainGeneration = settings.key.expectedGeneration;
  config.expectedAtlasRevisionLow = low(settings.expectedAtlasRevision);
  config.expectedAtlasRevisionHigh = high(settings.expectedAtlasRevision);
  config.generatorRevision = kTerrainGPUV2ColumnGeneratorRevision;
  config.faultCapacity = settings.faultCapacity;
  config.seedLow = low(bundle.seed);
  config.seedHigh = high(bundle.seed);
  config.bundleSignatureLow = low(bundle.bundleSignature);
  config.bundleSignatureHigh = high(bundle.bundleSignature);
  config.proceduralDetailStrength = static_cast<float>(settings.proceduralDetailStrength);
  config.proceduralTerraceStrength = static_cast<float>(settings.proceduralTerraceStrength);
  config.proceduralTerraceHeightMetres = static_cast<float>(settings.proceduralTerraceHeightMetres);
  config.proceduralTerraceTransitionFraction = static_cast<float>(settings.proceduralTerraceTransitionFraction);
  return true;
}

bool terrainGPUV2ColumnFootprint(
    const procedural::world::MapExportBundle &bundle,
    const procedural::world::MapExportGpuPageLayout &layout,
    const TerrainGPUV2ColumnConfigGPU &config,
    procedural::world::MapExportStreamRequest &request,
    std::string *error)
{
  request = {};
  if (config.columnsX == 0u || config.columnsZ == 0u || config.halo == 0u || config.worldExtentCanonical == 0u || config.requestedMapLevel > bundle.maximumPageLevel)
    return fail(error, "terrain GPU V2 column footprint is invalid");
  const double minimumX = static_cast<double>(int64_t{config.tileMinimumCanonicalX} - int64_t{config.halo} * config.canonicalStep);
  const double minimumZ = static_cast<double>(int64_t{config.tileMinimumCanonicalZ} - int64_t{config.halo} * config.canonicalStep);
  const double maximumX = static_cast<double>(int64_t{config.tileMinimumCanonicalX} + int64_t{config.columnsX + config.halo} * config.canonicalStep);
  const double maximumZ = static_cast<double>(int64_t{config.tileMinimumCanonicalZ} + int64_t{config.columnsZ + config.halo} * config.canonicalStep);
  request.normalizedBounds = {
    (minimumX - config.worldOriginCanonicalX) / config.worldExtentCanonical,
    (minimumZ - config.worldOriginCanonicalZ) / config.worldExtentCanonical,
    (maximumX - config.worldOriginCanonicalX) / config.worldExtentCanonical,
    (maximumZ - config.worldOriginCanonicalZ) / config.worldExtentCanonical};
  request.level = static_cast<uint8_t>(config.requestedMapLevel);
  request.guardPages = 0u;
  request.byteBudget = std::numeric_limits<size_t>::max();
  for (const uint32_t field : config.fields)
  {
    if (field == kTerrainGPUV2ColumnInvalidField)
      continue;
    if (field >= layout.fields().size())
      return fail(error, "terrain GPU V2 footprint field index is invalid");
    request.fields.push_back(layout.fields()[field].name);
  }
  std::sort(request.fields.begin(), request.fields.end());
  request.fields.erase(std::unique(request.fields.begin(), request.fields.end()), request.fields.end());
  return true;
}

bool buildTerrainGPUV2ColumnOracle(
    const procedural::world::MapExportBundle &bundle,
    const TerrainGPUV2ColumnConfigGPU &config,
    std::vector<TerrainGPUV2HeightSampleGPU> &heights,
    std::vector<TerrainGPUV2ColumnGPU> &columns,
    std::string *error)
{
  heights.clear();
  columns.clear();
  if (config.columnsX == 0u || config.columnsZ == 0u || config.halo == 0u || config.worldExtentCanonical == 0u || !(config.heightUnitMetres > 0.0f))
    return fail(error, "terrain GPU V2 column oracle config is invalid");
  const uint32_t sampleX = config.columnsX + 2u * config.halo + 1u;
  const uint32_t sampleZ = config.columnsZ + 2u * config.halo + 1u;
  const uint32_t columnX = sampleX - 1u;
  const uint32_t columnZ = sampleZ - 1u;
  heights.resize(uint64_t{sampleX} * sampleZ);
  columns.resize(uint64_t{columnX} * columnZ);
  const auto uv = [&](const double canonicalX, const double canonicalZ)
  {
    return std::array<double, 2u>{(canonicalX - config.worldOriginCanonicalX) / config.worldExtentCanonical, (canonicalZ - config.worldOriginCanonicalZ) / config.worldExtentCanonical};
  };
  const auto quantizedSurface = [&](const double canonicalX, const double canonicalZ, int32_t &heightUnits, uint32_t *sampleFlags = nullptr)
  {
    const auto coordinate = uv(canonicalX, canonicalZ);
    double height = 0.0;
    uint32_t flags = TerrainGPUV2ColumnValid;
    if (!surfaceHeight(bundle, config, coordinate[0], coordinate[1], height, flags, error))
      return false;
    const double units = height / config.heightUnitMetres;
    if (!std::isfinite(units) || units < std::numeric_limits<int32_t>::min() || units > std::numeric_limits<int32_t>::max())
      return fail(error, "terrain GPU V2 column height exceeds canonical quantization");
    // Match the occupancy-only GPU contract: this integer is the upper
    // half-open surface boundary. Keeping sub-voxel height reconstruction out
    // of the raster means ceil(height / voxel) - 1 selects the solid cell.
    heightUnits = static_cast<int32_t>(std::ceil(units));
    if (sampleFlags != nullptr)
      *sampleFlags = terrainGPUV2PackHeightSampleFlags(
          flags, height, config.heightUnitMetres, heightUnits);
    return true;
  };
  for (uint32_t z = 0u; z < sampleZ; ++z)
    for (uint32_t x = 0u; x < sampleX; ++x)
    {
      const int64_t canonicalX = int64_t{config.tileMinimumCanonicalX} + (int64_t{x} - config.halo) * config.canonicalStep;
      const int64_t canonicalZ = int64_t{config.tileMinimumCanonicalZ} + (int64_t{z} - config.halo) * config.canonicalStep;
      int32_t heightUnits = 0;
      uint32_t flags = TerrainGPUV2ColumnValid;
      if (!quantizedSurface(canonicalX, canonicalZ, heightUnits, &flags))
        return false;
      heights[uint64_t{z} * sampleX + x] = {heightUnits, static_cast<int32_t>(canonicalX), static_cast<int32_t>(canonicalZ), flags};
    }
  for (uint32_t z = 0u; z < columnZ; ++z)
    for (uint32_t x = 0u; x < columnX; ++x)
    {
      const auto &a = heights[uint64_t{z} * sampleX + x];
      const auto &b = heights[uint64_t{z} * sampleX + x + 1u];
      const auto &c = heights[uint64_t{z + 1u} * sampleX + x];
      const auto &d = heights[uint64_t{z + 1u} * sampleX + x + 1u];
      // The shader forms this address in f32 after the integer lattice step.
      // Mirror that arithmetic in the differential oracle; otherwise very
      // coarse pages can select the adjacent sparse-biome mixture texel even
      // though the GPU is behaving deterministically.
      const float centreX = static_cast<float>(int64_t{config.tileMinimumCanonicalX} + (int64_t{x} - config.halo) * config.canonicalStep) + 0.5f * static_cast<float>(config.canonicalStep);
      const float centreZ = static_cast<float>(int64_t{config.tileMinimumCanonicalZ} + (int64_t{z} - config.halo) * config.canonicalStep) + 0.5f * static_cast<float>(config.canonicalStep);
      const std::array<double, 2u> coordinate{
        static_cast<double>((centreX - static_cast<float>(config.worldOriginCanonicalX)) / static_cast<float>(config.worldExtentCanonical)),
        static_cast<double>((centreZ - static_cast<float>(config.worldOriginCanonicalZ)) / static_cast<float>(config.worldExtentCanonical))};
      uint32_t material = 0u;
      uint32_t flags = TerrainGPUV2ColumnValid;
      if (!surfaceMaterial(bundle, config, coordinate[0], coordinate[1], material, flags, error))
        return false;
      int32_t minimumHeight = std::min({a.heightUnits, b.heightUnits, c.heightUnits, d.heightUnits});
      int32_t maximumHeight = std::max({a.heightUnits, b.heightUnits, c.heightUnits, d.heightUnits});
      const int32_t localX = static_cast<int32_t>(x) - static_cast<int32_t>(config.halo);
      const int32_t localZ = static_cast<int32_t>(z) - static_cast<int32_t>(config.halo);
      if (config.canonicalStep > 1u)
      {
        const int64_t step = config.canonicalStep;
        const int64_t halfStep = step / 2;
        const auto includeBoundary = [&](const double canonicalX, const double canonicalZ)
        {
          int32_t boundary = 0;
          if (!quantizedSurface(canonicalX, canonicalZ, boundary))
            return false;
          minimumHeight = std::min(minimumHeight, boundary);
          maximumHeight = std::max(maximumHeight, boundary);
          return true;
        };
        if (localX == 0 && localZ >= 0 && localZ < static_cast<int32_t>(config.columnsZ) && !includeBoundary(config.tileMinimumCanonicalX, int64_t{config.tileMinimumCanonicalZ} + localZ * step + halfStep))
          return false;
        if (localX == static_cast<int32_t>(config.columnsX) - 1 && localZ >= 0 && localZ < static_cast<int32_t>(config.columnsZ) &&
            !includeBoundary(int64_t{config.tileMinimumCanonicalX} + int64_t{config.columnsX} * step, int64_t{config.tileMinimumCanonicalZ} + localZ * step + halfStep))
          return false;
        if (localZ == 0 && localX >= 0 && localX < static_cast<int32_t>(config.columnsX) && !includeBoundary(int64_t{config.tileMinimumCanonicalX} + localX * step + halfStep, config.tileMinimumCanonicalZ))
          return false;
        if (localZ == static_cast<int32_t>(config.columnsZ) - 1 && localX >= 0 && localX < static_cast<int32_t>(config.columnsX) &&
            !includeBoundary(int64_t{config.tileMinimumCanonicalX} + localX * step + halfStep, int64_t{config.tileMinimumCanonicalZ} + int64_t{config.columnsZ} * step))
          return false;
      }
      columns[uint64_t{z} * columnX + x] = {minimumHeight, maximumHeight, material, flags};
    }
  return true;
}

bool TerrainGPUV2ColumnTileRegistry::acquire(const uint64_t jobId, const TerrainGPUV2ColumnTileKey &key, uint64_t &tileId, bool &created, std::string *error)
{
  tileId = 0u;
  created = false;
  if (jobId == 0u || tileByJob_.count(jobId) != 0u)
    return fail(error, "terrain GPU V2 column consumer is invalid or duplicate");
  const auto existing = idsByKey_.find(key);
  if (existing != idsByKey_.end())
  {
    tileId = existing->second;
    auto &entry = entries_.at(tileId);
    entry.consumers.push_back(jobId);
    entry.record.consumerCount = static_cast<uint32_t>(entry.consumers.size());
    entry.record.releaseWhenIdle = false;
  }
  else
  {
    tileId = nextTileId_++;
    Entry entry{};
    entry.record.id = tileId;
    entry.record.key = key;
    entry.record.consumerCount = 1u;
    entry.consumers.push_back(jobId);
    idsByKey_.emplace(key, tileId);
    entries_.emplace(tileId, std::move(entry));
    created = true;
  }
  tileByJob_.emplace(jobId, tileId);
  return true;
}

bool TerrainGPUV2ColumnTileRegistry::release(const uint64_t jobId, std::string *error)
{
  const auto job = tileByJob_.find(jobId);
  if (job == tileByJob_.end())
    return fail(error, "terrain GPU V2 column consumer is not registered");
  const uint64_t tileId = job->second;
  auto entry = entries_.find(tileId);
  auto &consumers = entry->second.consumers;
  consumers.erase(std::remove(consumers.begin(), consumers.end(), jobId), consumers.end());
  entry->second.record.consumerCount = static_cast<uint32_t>(consumers.size());
  tileByJob_.erase(job);
  if (consumers.empty())
  {
    if (entry->second.record.state == TerrainGPUV2ColumnTileState::Building)
      entry->second.record.releaseWhenIdle = true;
    else
    {
      idsByKey_.erase(entry->second.record.key);
      entries_.erase(entry);
    }
  }
  return true;
}

bool TerrainGPUV2ColumnTileRegistry::markAtlasReady(const uint64_t tileId, const uint64_t atlasRevision, std::string *error)
{
  const auto found = entries_.find(tileId);
  if (found == entries_.end() || atlasRevision == 0u || found->second.record.state == TerrainGPUV2ColumnTileState::Building)
    return fail(error, "terrain GPU V2 column atlas transition is invalid");
  found->second.record.atlasRevision = atlasRevision;
  found->second.record.state = TerrainGPUV2ColumnTileState::ReadyToBuild;
  found->second.record.faults.clear();
  return true;
}

bool TerrainGPUV2ColumnTileRegistry::beginBuild(const uint64_t tileId, std::string *error)
{
  const auto found = entries_.find(tileId);
  if (found == entries_.end() || found->second.record.state != TerrainGPUV2ColumnTileState::ReadyToBuild)
    return fail(error, "terrain GPU V2 column build transition is invalid");
  found->second.record.state = TerrainGPUV2ColumnTileState::Building;
  return true;
}

bool TerrainGPUV2ColumnTileRegistry::completeBuild(const uint64_t tileId, const TerrainGPUV2ColumnResultGPU &result, std::vector<TerrainGPUV2MapFaultGPU> faults, std::string *error)
{
  const auto found = entries_.find(tileId);
  if (found == entries_.end() || found->second.record.state != TerrainGPUV2ColumnTileState::Building)
    return fail(error, "terrain GPU V2 column completion is invalid");
  auto &record = found->second.record;
  if (result.status == static_cast<uint32_t>(TerrainGPUV2ColumnStatus::Ready))
  {
    if (result.writtenColumns != result.columnCount || result.terrainGeneration != record.key.expectedGeneration || (uint64_t{result.atlasRevisionHigh} << 32u | result.atlasRevisionLow) != record.atlasRevision)
      return fail(error, "terrain GPU V2 ready column result is stale");
    record.state = TerrainGPUV2ColumnTileState::Ready;
    record.faults.clear();
  }
  else if (result.status == static_cast<uint32_t>(TerrainGPUV2ColumnStatus::MissingPage))
  {
    std::sort(faults.begin(), faults.end());
    faults.erase(std::unique(faults.begin(), faults.end()), faults.end());
    record.faults = std::move(faults);
    record.state = TerrainGPUV2ColumnTileState::Faulted;
    ++record.retryCount;
  }
  else
    return fail(error, "terrain GPU V2 column build failed");

  if (record.releaseWhenIdle && record.consumerCount == 0u)
  {
    idsByKey_.erase(record.key);
    entries_.erase(found);
  }
  return true;
}

std::optional<TerrainGPUV2ColumnTileRecord> TerrainGPUV2ColumnTileRegistry::tile(const uint64_t tileId) const
{
  const auto found = entries_.find(tileId);
  return found == entries_.end() ? std::nullopt : std::optional<TerrainGPUV2ColumnTileRecord>{found->second.record};
}

std::optional<uint64_t> TerrainGPUV2ColumnTileRegistry::tileForJob(const uint64_t jobId) const
{
  const auto found = tileByJob_.find(jobId);
  return found == tileByJob_.end() ? std::nullopt : std::optional<uint64_t>{found->second};
}

uint32_t TerrainGPUV2ColumnTileRegistry::tileCount() const
{
  return static_cast<uint32_t>(entries_.size());
}

uint32_t TerrainGPUV2ColumnTileRegistry::jobCount() const
{
  return static_cast<uint32_t>(tileByJob_.size());
}

} // namespace voxel::svraster
