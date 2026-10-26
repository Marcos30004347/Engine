#include "voxel/svraster/TerrainGPUColumnBuild.hpp"

#include "procedural/world/MapExportSampler.hpp"

#include <algorithm>
#include <cmath>
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

uint32_t low(const uint64_t value)
{
  return static_cast<uint32_t>(value);
}

uint32_t high(const uint64_t value)
{
  return static_cast<uint32_t>(value >> 32u);
}

void mix(uint64_t &hash, const uint32_t value)
{
  for (uint32_t shift = 0u; shift < 32u; shift += 8u)
  {
    hash ^= static_cast<uint8_t>(value >> shift);
    hash *= 1099511628211ull;
  }
}

uint32_t gpuHash32(uint32_t value)
{
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

float gpuLatticeNoise(
    const int32_t x,
    const int32_t z,
    const uint32_t salt,
    const procedural::world::MapExportBundle &bundle)
{
  const uint32_t key = static_cast<uint32_t>(x) * 0x9e3779b9u ^
      static_cast<uint32_t>(z) * 0x85ebca6bu ^
      static_cast<uint32_t>(bundle.seed) ^
      static_cast<uint32_t>(bundle.bundleSignature >> 32u) ^ salt;
  return static_cast<float>(gpuHash32(key) & 0x00ffffffu) /
          8388607.5f -
      1.0f;
}

float gpuSmoothNoise(
    const float worldX,
    const float worldZ,
    const float wavelength,
    const uint32_t salt,
    const procedural::world::MapExportBundle &bundle)
{
  const float px = worldX / wavelength;
  const float pz = worldZ / wavelength;
  const int32_t x = static_cast<int32_t>(std::floor(px));
  const int32_t z = static_cast<int32_t>(std::floor(pz));
  float tx = px - std::floor(px);
  float tz = pz - std::floor(pz);
  tx = tx * tx * (3.0f - 2.0f * tx);
  tz = tz * tz * (3.0f - 2.0f * tz);
  const float x00 = gpuLatticeNoise(x, z, salt, bundle);
  const float x10 = gpuLatticeNoise(x + 1, z, salt, bundle);
  const float x01 = gpuLatticeNoise(x, z + 1, salt, bundle);
  const float x11 = gpuLatticeNoise(x + 1, z + 1, salt, bundle);
  const float a = x00 + (x10 - x00) * tx;
  const float b = x01 + (x11 - x01) * tx;
  return a + (b - a) * tz;
}
} // namespace

bool sampleTerrainGPUWorldMapSurfaceHeight(
    const procedural::world::MapExportBundle &bundle,
    const uint8_t requestedLevel,
    const PlanetVoxelChunkSettings &settings,
    const double normalizedU,
    const double normalizedV,
    float &heightMetres,
    std::string *error)
{
  using procedural::world::MapExportFieldSample;
  if (error != nullptr)
    error->clear();
  if (!std::isfinite(normalizedU) || !std::isfinite(normalizedV) ||
      requestedLevel > bundle.maximumPageLevel ||
      !(bundle.extentMetres > 0.0) ||
      !(settings.proceduralTerraceHeightMetres > 0.0))
    return fail(error, "GPU terrain surface probe settings are invalid");

  const float u = static_cast<float>(std::clamp(normalizedU, 0.0, 1.0));
  const float v = static_cast<float>(std::clamp(normalizedV, 0.0, 1.0));
  const auto available = [&](const char *name)
  {
    const auto *field = bundle.field(name);
    return field != nullptr && field->available;
  };
  const auto sample = [&](const char *name,
                          const std::array<float, 4u> fallback,
                          const bool required,
                          std::array<float, 4u> &value)
  {
    if (!available(name))
    {
      if (required)
        return fail(error, std::string("GPU terrain surface probe lacks ") + name);
      value = fallback;
      return true;
    }
    MapExportFieldSample source{};
    if (!procedural::world::sampleMapExportField(
            bundle, name, u, v, requestedLevel, source, error))
      return false;
    for (uint32_t component = 0u; component < 4u; ++component)
      value[component] = static_cast<float>(source.values[component]);
    return true;
  };

  const char *elevationName = available("refined_elevation")
      ? "refined_elevation" : "base_elevation";
  const char *settlementName = available("settlement_activity")
      ? "settlement_activity" : "object_exclusion";
  std::array<float, 4u> elevation{}, water{}, waterLevel{}, river{};
  std::array<float, 4u> erosion{}, deposition{}, uplift{}, relief{};
  std::array<float, 4u> coast{}, landform{}, roads{}, settlement{}, base{};
  if (!sample(elevationName, {}, true, elevation) ||
      !sample("water_type", {}, true, water) ||
      !sample("water_level", {}, true, waterLevel) ||
      !sample("river", {}, true, river))
    return false;
  if (static_cast<uint32_t>(std::lround(water[0])) != 0u)
  {
    heightMetres = waterLevel[0];
    return true;
  }
  if (!sample("erosion_wear_flow", {}, false, erosion) ||
      !sample("erosion_deposition", {}, false, deposition) ||
      !sample("uplift", {}, false, uplift) ||
      !sample("relief_roughness", {}, false, relief) ||
      !sample("coast_distance", {-1.0f, 0.0f, 0.0f, 0.0f}, false, coast) ||
      !sample("landform_id", {4.0f, 0.0f, 0.0f, 0.0f}, false, landform) ||
      !sample("roads", {}, false, roads) ||
      !sample(settlementName, {}, false, settlement))
    return false;

  const float discharge = std::max(river[1], 0.0f);
  const float halfWidth = std::clamp(
      5.0f + std::sqrt(discharge) * 8.0f, 5.0f, 120.0f);
  const float channelT = std::clamp(1.0f - river[0] / halfWidth, 0.0f, 1.0f);
  const float incision = channelT * channelT * (3.0f - 2.0f * channelT) *
      std::clamp(
          std::log(1.0f + discharge) * 1.8f +
              std::clamp(erosion[1], 0.0f, 1.0f) * 4.0f,
          0.0f, 12.0f);
  const float authored = elevation[0] - incision;
  if (!(settings.proceduralDetailStrength > 0.0))
  {
    heightMetres = authored;
    return true;
  }

  const uint32_t landformId = static_cast<uint32_t>(std::lround(landform[0]));
  float character = 1.0f;
  if (landformId == 3u)
    character = 0.25f;
  if (landformId == 5u)
    character = 1.4f;
  if (landformId == 6u || landformId == 12u)
    character = 0.8f;
  if (landformId == 13u)
    character = 1.1f;
  const float coastMask = coast[0] < 0.0f
      ? 0.0f
      : std::clamp(1.0f - std::abs(coast[0]) / 320.0f, 0.0f, 1.0f);
  const float signal = static_cast<float>(settings.proceduralDetailStrength) *
      (1.0f + std::clamp(std::abs(uplift[0]) / 0.001f, 0.0f, 1.0f) * 0.35f) *
      (1.0f - std::clamp(erosion[0], 0.0f, 1.0f) * 0.15f) *
      (1.0f - std::clamp(erosion[1], 0.0f, 1.0f) * 0.45f) *
      (1.0f - std::clamp(deposition[0], 0.0f, 1.0f) * 0.35f) *
      (1.0f - coastMask * 0.65f);
  const float worldX = u * static_cast<float>(bundle.extentMetres);
  const float worldZ = v * static_cast<float>(bundle.extentMetres);
  float detail =
      (gpuSmoothNoise(worldX, worldZ, 720.0f, 3u, bundle) * 11.0f +
       gpuSmoothNoise(worldX, worldZ, 220.0f, 5u, bundle) * 4.0f +
       gpuSmoothNoise(worldX, worldZ, 48.0f, 6u, bundle) * 1.4f +
       gpuSmoothNoise(worldX, worldZ, 14.0f, 7u, bundle) * 0.32f) *
      signal * character *
      std::clamp(relief[0] / 100.0f + relief[1], 0.1f, 4.0f);
  const float terraceHeight =
      static_cast<float>(settings.proceduralTerraceHeightMetres);
  const float riverShoulder = std::max(180.0f, terraceHeight * 16.0f);
  const float riverSmooth = std::clamp(
      1.0f - river[0] / riverShoulder, 0.0f, 1.0f);
  const float settlementMask = settlement[0] > 1.0f
      ? settlement[0] / 255.0f : settlement[0];
  const float infrastructure = std::max(
      roads[0] > 0.0f ? 1.0f : 0.0f,
      std::clamp(settlementMask, 0.0f, 1.0f));
  detail *= 1.0f - std::max(
      infrastructure,
      riverSmooth * riverSmooth * (3.0f - 2.0f * riverSmooth) * 0.9f);
  const float detailed = authored + detail;
  if (!(settings.proceduralTerraceStrength > 0.0))
  {
    heightMetres = detailed;
    return true;
  }
  if (!sample("base_elevation", {elevation[0], 0.0f, 0.0f, 0.0f},
          false, base))
    return false;
  const float scaled = base[0] / terraceHeight;
  const float lower = std::floor(scaled);
  const float phase = scaled - lower;
  const float transition =
      static_cast<float>(settings.proceduralTerraceTransitionFraction);
  const float t = transition > 0.0f
      ? std::clamp(
            (phase - (1.0f - transition * 2.0f)) /
                (transition * 2.0f),
            0.0f, 1.0f)
      : 0.0f;
  const float terraced =
      (lower + t * t * (3.0f - 2.0f * t)) * terraceHeight +
      (authored - base[0]) + detail * 0.35f;
  const float corridor = std::max(infrastructure, riverSmooth);
  const float landformWeight =
      landformId == 6u || landformId == 12u || landformId == 13u
      ? 1.0f : 0.0f;
  const float blend = std::clamp(
      static_cast<float>(settings.proceduralTerraceStrength) *
          landformWeight * (1.0f - corridor),
      0.0f, 1.0f);
  heightMetres = detailed + (terraced - detailed) * blend;
  return std::isfinite(heightMetres) ||
      fail(error, "GPU terrain surface probe produced a non-finite height");
}

bool planTerrainGPUColumnBuild(
    const procedural::world::MapExportBundle &bundle,
    const procedural::world::MapExportGpuPageLayout &layout,
    const TerrainGPUColumnBuildSettings &settings,
    TerrainGPUColumnConfigGPU &config,
    std::string *error)
{
  config = {};
  config.fields.fill(kTerrainGPUColumnInvalidField);
  if (error != nullptr)
    error->clear();
  const double minimumU = std::clamp(std::min(settings.normalizedBounds[0], settings.normalizedBounds[2]), 0.0, 1.0);
  const double minimumV = std::clamp(std::min(settings.normalizedBounds[1], settings.normalizedBounds[3]), 0.0, 1.0);
  const double maximumU = std::clamp(std::max(settings.normalizedBounds[0], settings.normalizedBounds[2]), 0.0, 1.0);
  const double maximumV = std::clamp(std::max(settings.normalizedBounds[1], settings.normalizedBounds[3]), 0.0, 1.0);
  const double edgeX = (maximumU - minimumU) * bundle.extentMetres;
  const double edgeZ = (maximumV - minimumV) * bundle.extentMetres;
  if (settings.side == 0u || settings.side > 1024u || settings.halo < 1u || settings.halo > 8u || settings.maximumVerticalVoxels == 0u || settings.maximumVerticalVoxels > 4096u ||
      settings.requestedLevel > bundle.maximumPageLevel || settings.boundaryStitchSubdivisions == 0u || settings.boundaryStitchSubdivisions > 8u || !(bundle.extentMetres > 0.0) || !(edgeX > 0.0) ||
      std::abs(edgeX - edgeZ) > std::max(edgeX, edgeZ) * 1.0e-6 || !(settings.finestVoxelSizeMetres > 0.0) || settings.foundationDepthVoxels > settings.maximumVerticalVoxels || !std::isfinite(settings.elevationOriginMetres) || !std::isfinite(settings.proceduralDetailStrength) ||
      !std::isfinite(settings.proceduralTerraceStrength) || !(settings.proceduralTerraceHeightMetres > 0.0) || settings.proceduralTerraceTransitionFraction < 0.0 || settings.proceduralTerraceTransitionFraction >= 0.5)
    return fail(error, "GPU terrain column settings are invalid");

  const std::array<const char *, kTerrainGPUColumnFieldCount> names{
    "refined_elevation",  "base_elevation", "uplift",     "relief_roughness", "landform_id",       "water_type", "water_level",         "river",       "coast_distance", "erosion_wear_flow",
    "erosion_deposition", "substrate_id",   "soil_depth", "biome_mix_ids",    "biome_mix_weights", "climate",    "placement_potential", "soil_layers", "roads",          "settlement_activity",
  };
  using procedural::world::MapExportFilter;
  using procedural::world::MapExportFormat;
  const std::array<MapExportFormat, kTerrainGPUColumnFieldCount> formats{
    MapExportFormat::R16Unorm,   MapExportFormat::R16Unorm,  MapExportFormat::R16Float,   MapExportFormat::RG16Float,  MapExportFormat::R8Uint, MapExportFormat::R8Uint,   MapExportFormat::R16Float,
    MapExportFormat::RG16Float,  MapExportFormat::R16Float,  MapExportFormat::RG16Float,  MapExportFormat::R16Float,   MapExportFormat::R8Uint, MapExportFormat::R16Float, MapExportFormat::RGBA8Uint,
    MapExportFormat::RGBA8Unorm, MapExportFormat::RG16Float, MapExportFormat::RGBA8Unorm, MapExportFormat::RGBA8Unorm, MapExportFormat::R8Uint, MapExportFormat::R8Uint,
  };
  const std::array<MapExportFilter, kTerrainGPUColumnFieldCount> filters{
    MapExportFilter::Linear,        MapExportFilter::Linear, MapExportFilter::Linear, MapExportFilter::Linear, MapExportFilter::Nearest, MapExportFilter::Nearest, MapExportFilter::Linear,
    MapExportFilter::Linear,        MapExportFilter::Linear, MapExportFilter::Linear, MapExportFilter::Linear, MapExportFilter::Nearest, MapExportFilter::Linear,  MapExportFilter::SparseMixture,
    MapExportFilter::SparseMixture, MapExportFilter::Linear, MapExportFilter::Linear, MapExportFilter::Linear, MapExportFilter::Nearest, MapExportFilter::Nearest,
  };
  for (uint32_t index = 0u; index < names.size(); ++index)
  {
    auto field = layout.fieldIndex(names[index]);
    if (index == static_cast<uint32_t>(TerrainGPUColumnField::Elevation) && !field.has_value())
      field = layout.fieldIndex("base_elevation");
    if (index == static_cast<uint32_t>(TerrainGPUColumnField::SettlementOrExclusion) && !field.has_value())
      field = layout.fieldIndex("object_exclusion");
    if (field.has_value())
    {
      if (layout.fields()[*field].format != formats[index] || layout.fields()[*field].filter != filters[index])
        return fail(error, "GPU terrain column layout has an incompatible field format");
      config.fields[index] = *field;
    }
  }
  const std::array<TerrainGPUColumnField, 8u> required{
    TerrainGPUColumnField::Elevation,
    TerrainGPUColumnField::WaterType,
    TerrainGPUColumnField::WaterLevel,
    TerrainGPUColumnField::River,
    TerrainGPUColumnField::Substrate,
    TerrainGPUColumnField::SoilDepth,
    TerrainGPUColumnField::BiomeIds,
    TerrainGPUColumnField::BiomeWeights,
  };
  for (const auto field : required)
    if (config.fields[static_cast<uint32_t>(field)] == kTerrainGPUColumnInvalidField)
      return fail(error, "GPU terrain column layout lacks a required field");

  const double voxelSize = edgeX / settings.side;
  const double heightUnit = settings.finestVoxelSizeMetres / kWorldMapTerrainHeightUnitsPerVoxel;
  if (!std::isfinite(voxelSize) || !(voxelSize > 0.0) || !std::isfinite(heightUnit) || !(heightUnit > 0.0))
    return fail(error, "GPU terrain column scale is invalid");
  config.side = settings.side;
  config.halo = settings.halo;
  config.maximumVerticalVoxels = settings.maximumVerticalVoxels;
  config.requestedLevel = settings.requestedLevel;
  config.internalLOD = settings.internalLOD;
  config.boundaryStitchSubdivisions = settings.boundaryStitchSubdivisions;
  config.surfaceShellDepthVoxels = settings.surfaceShellDepthVoxels;
  config.flags = settings.exteriorShellOnly ? TerrainGPUColumnExteriorShell : 0u;
  if (settings.resolveElevationOriginOnGPU)
    config.flags |= TerrainGPUColumnResolvePageDatum;
  config.expectedTerrainGeneration = settings.expectedTerrainGeneration;
  config.generatorRevisionLow = low(kTerrainGPUColumnGeneratorRevision);
  config.generatorRevisionHigh = high(kTerrainGPUColumnGeneratorRevision);
  config.expectedAtlasRevisionLow = low(settings.expectedAtlasRevision);
  config.expectedAtlasRevisionHigh = high(settings.expectedAtlasRevision);
  config.seedLow = low(bundle.seed);
  config.seedHigh = high(bundle.seed);
  config.bundleSignatureLow = low(bundle.bundleSignature);
  config.bundleSignatureHigh = high(bundle.bundleSignature);
  config.minimumU = static_cast<float>(minimumU);
  config.minimumV = static_cast<float>(minimumV);
  config.maximumU = static_cast<float>(maximumU);
  config.maximumV = static_cast<float>(maximumV);
  config.extentMetres = static_cast<float>(bundle.extentMetres);
  config.edgeMetres = static_cast<float>(edgeX);
  config.voxelSizeMetres = static_cast<float>(voxelSize);
  config.finestVoxelSizeMetres = static_cast<float>(settings.finestVoxelSizeMetres);
  config.heightUnitMetres = static_cast<float>(heightUnit);
  config.elevationOriginMetres = static_cast<float>(settings.elevationOriginMetres);
  config.proceduralDetailStrength = static_cast<float>(settings.proceduralDetailStrength);
  config.proceduralTerraceStrength = static_cast<float>(settings.proceduralTerraceStrength);
  config.proceduralTerraceHeightMetres = static_cast<float>(settings.proceduralTerraceHeightMetres);
  config.proceduralTerraceTransitionFraction = static_cast<float>(settings.proceduralTerraceTransitionFraction);
  config.foundationDepthVoxels = settings.foundationDepthVoxels;
  return true;
}

bool buildTerrainGPUColumnOracle(const TerrainGPUColumnConfigGPU &config, const TerrainGPUColumnSurfaceSamples &samples, WorldMapTerrainColumnPage &page, std::string *error)
{
  page = {};
  if (error != nullptr)
    error->clear();
  const uint32_t cornerSide = config.side + 2u * config.halo + 1u;
  if (config.side == 0u || config.maximumVerticalVoxels == 0u || samples.cornerHeights.size() != static_cast<size_t>(cornerSide) * cornerSide ||
      samples.materials.size() != static_cast<size_t>(config.side) * config.side || (!samples.boundaryMinimumHeights.empty() && samples.boundaryMinimumHeights.size() != 4u * config.side) ||
      (!samples.boundaryMaximumHeights.empty() && samples.boundaryMaximumHeights.size() != 4u * config.side) || !(config.voxelSizeMetres > 0.0f) || !(config.heightUnitMetres > 0.0f))
    return fail(error, "GPU terrain column oracle input is invalid");
  const auto corner = [&](const int32_t x, const int32_t z)
  {
    return samples.cornerHeights[static_cast<size_t>(z + static_cast<int32_t>(config.halo)) * cornerSide + static_cast<uint32_t>(x + static_cast<int32_t>(config.halo))];
  };
  const auto relativeUnits = [&](const double height)
  {
    return static_cast<int64_t>(std::llround((height - config.elevationOriginMetres) / config.voxelSizeMetres * kWorldMapTerrainHeightUnitsPerVoxel));
  };
  const auto ownedTop = [&](const double height)
  {
    const int64_t value = std::max<int64_t>(relativeUnits(height), 0);
    return value == 0 ? 0u : static_cast<uint32_t>((static_cast<uint64_t>(value) + kWorldMapTerrainHeightUnitsPerVoxel - 1u) / kWorldMapTerrainHeightUnitsPerVoxel - 1u);
  };
  const auto floorCell = [&](const double height)
  {
    return static_cast<uint32_t>(std::max<int64_t>(relativeUnits(height), 0) / kWorldMapTerrainHeightUnitsPerVoxel);
  };
  const uint32_t topSide = config.side + 2u * config.halo;
  std::vector<uint32_t> tops(static_cast<size_t>(topSide) * topSide);
  const auto topIndex = [&](const int32_t x, const int32_t z)
  {
    return static_cast<size_t>(z + static_cast<int32_t>(config.halo)) * topSide + static_cast<uint32_t>(x + static_cast<int32_t>(config.halo));
  };
  for (int32_t z = -static_cast<int32_t>(config.halo); z < static_cast<int32_t>(config.side + config.halo); ++z)
    for (int32_t x = -static_cast<int32_t>(config.halo); x < static_cast<int32_t>(config.side + config.halo); ++x)
    {
      const double maximum = std::max({corner(x, z), corner(x + 1, z), corner(x, z + 1), corner(x + 1, z + 1)});
      const uint32_t top = ownedTop(maximum);
      if (top >= config.maximumVerticalVoxels)
        return fail(error, "GPU terrain column exceeds vertical capacity");
      tops[topIndex(x, z)] = top;
    }
  const auto top = [&](const int32_t x, const int32_t z)
  {
    return tops[topIndex(x, z)];
  };

  page.side = config.side;
  page.halo = config.halo;
  page.maximumVerticalVoxels = config.maximumVerticalVoxels;
  page.internalLOD = config.internalLOD;
  page.flags = (config.flags & TerrainGPUColumnExteriorShell) != 0u ? WorldMapTerrainColumnExteriorShell : 0u;
  page.heightUnitMetres = config.heightUnitMetres;
  page.voxelSizeMetres = config.voxelSizeMetres;
  page.edgeMetres = config.edgeMetres;
  page.elevationOriginMetres = config.elevationOriginMetres;
  page.columns.resize(static_cast<size_t>(config.side) * config.side);
  page.walls.resize(page.columns.size());
  page.canonicalCornerHeights.resize(static_cast<size_t>(config.side + 1u) * (config.side + 1u));
  page.canonicalBoundaryMinimumHeights.resize(4u * config.side);
  page.canonicalBoundaryMaximumHeights.resize(4u * config.side);
  std::vector<float> boundaryMinimums(4u * config.side);
  std::vector<float> boundaryMaximums(4u * config.side);
  for (uint32_t z = 0u; z <= config.side; ++z)
    for (uint32_t x = 0u; x <= config.side; ++x)
      page.canonicalCornerHeights[static_cast<size_t>(z) * (config.side + 1u) + x] = quantizeWorldMapTerrainHeight(corner(x, z), config.heightUnitMetres);
  for (uint32_t segment = 0u; segment < config.side; ++segment)
  {
    const std::array<std::array<float, 2u>, 4u> endpoints{{
      {{corner(0, segment), corner(0, segment + 1u)}},
      {{corner(config.side, segment), corner(config.side, segment + 1u)}},
      {{corner(segment, 0), corner(segment + 1u, 0)}},
      {{corner(segment, config.side), corner(segment + 1u, config.side)}},
    }};
    for (uint32_t face = 0u; face < 4u; ++face)
    {
      const size_t index = static_cast<size_t>(face) * config.side + segment;
      boundaryMinimums[index] = samples.boundaryMinimumHeights.empty() ? std::min(endpoints[face][0], endpoints[face][1]) : samples.boundaryMinimumHeights[index];
      boundaryMaximums[index] = samples.boundaryMaximumHeights.empty() ? std::max(endpoints[face][0], endpoints[face][1]) : samples.boundaryMaximumHeights[index];
      page.canonicalBoundaryMinimumHeights[index] = quantizeWorldMapTerrainHeight(boundaryMinimums[index], config.heightUnitMetres);
      page.canonicalBoundaryMaximumHeights[index] = quantizeWorldMapTerrainHeight(boundaryMaximums[index], config.heightUnitMetres);
    }
  }
  for (uint32_t z = 0u; z < config.side; ++z)
    for (uint32_t x = 0u; x < config.side; ++x)
    {
      float maximum = std::numeric_limits<float>::lowest();
      if (x == 0u)
        maximum = std::max(maximum, boundaryMaximums[z]);
      if (x + 1u == config.side)
        maximum = std::max(maximum, boundaryMaximums[config.side + z]);
      if (z == 0u)
        maximum = std::max(maximum, boundaryMaximums[2u * config.side + x]);
      if (z + 1u == config.side)
        maximum = std::max(maximum, boundaryMaximums[3u * config.side + x]);
      if (maximum != std::numeric_limits<float>::lowest())
      {
        const uint32_t boundaryTop = ownedTop(maximum);
        if (boundaryTop >= config.maximumVerticalVoxels)
          return fail(error, "GPU terrain boundary exceeds vertical capacity");
        tops[topIndex(x, z)] = std::max(tops[topIndex(x, z)], boundaryTop);
      }
    }

  const bool shell = (config.flags & TerrainGPUColumnExteriorShell) != 0u;
  for (uint32_t z = 0u; z < config.side; ++z)
    for (uint32_t x = 0u; x < config.side; ++x)
    {
      const size_t index = static_cast<size_t>(z) * config.side + x;
      const uint32_t columnTop = top(x, z);
      const uint32_t lowest = std::min({columnTop, top(x - 1, z), top(x + 1, z), top(x, z - 1), top(x, z + 1)});
      uint32_t first = 0u;
      if (shell)
      {
        const uint32_t exposed = lowest < columnTop ? lowest + 1u : columnTop;
        first = lowest < columnTop && exposed > config.surfaceShellDepthVoxels ? exposed - config.surfaceShellDepthVoxels : (lowest < columnTop ? 0u : columnTop);
        const float minimumCorner = std::min({corner(x, z), corner(x + 1, z), corner(x, z + 1), corner(x + 1, z + 1)});
        first = std::min(first, std::min(floorCell(minimumCorner), config.maximumVerticalVoxels - 1u));
      }
      else if (config.surfaceShellDepthVoxels != 0u && columnTop > config.surfaceShellDepthVoxels)
        first = std::min(columnTop - config.surfaceShellDepthVoxels, lowest);
      const uint32_t unstitched = first;
      if (shell && config.boundaryStitchSubdivisions > 1u)
      {
        float boundaryHeight = std::numeric_limits<float>::max();
        if (x == 0u)
          boundaryHeight = std::min(boundaryHeight, boundaryMinimums[z]);
        if (x + 1u == config.side)
          boundaryHeight = std::min(boundaryHeight, boundaryMinimums[config.side + z]);
        if (z == 0u)
          boundaryHeight = std::min(boundaryHeight, boundaryMinimums[2u * config.side + x]);
        if (z + 1u == config.side)
          boundaryHeight = std::min(boundaryHeight, boundaryMinimums[3u * config.side + x]);
        if (std::isfinite(boundaryHeight))
        {
          const uint32_t boundaryTop = std::min(floorCell(boundaryHeight), config.maximumVerticalVoxels - 1u);
          const uint32_t stitchFloor = boundaryTop > config.surfaceShellDepthVoxels ? boundaryTop - config.surfaceShellDepthVoxels : 0u;
          first = std::min(first, stitchFloor);
        }
      }
      uint32_t threshold = columnTop;
      SparseVoxelMicroContext wall = SparseVoxelMicroContext::Interior;
      const auto consider = [&](const uint32_t candidate, const SparseVoxelMicroContext context)
      {
        if (candidate < threshold)
        {
          threshold = candidate;
          wall = context;
        }
      };
      consider(top(x - 1, z), SparseVoxelMicroContext::WallNegativeX);
      consider(top(x + 1, z), SparseVoxelMicroContext::WallPositiveX);
      consider(top(x, z - 1), SparseVoxelMicroContext::WallNegativeZ);
      consider(top(x, z + 1), SparseVoxelMicroContext::WallPositiveZ);
      const SparseVoxelMicroContext boundary = x == 0u                 ? SparseVoxelMicroContext::WallNegativeX
                                               : x + 1u == config.side ? SparseVoxelMicroContext::WallPositiveX
                                               : z == 0u               ? SparseVoxelMicroContext::WallNegativeZ
                                               : z + 1u == config.side ? SparseVoxelMicroContext::WallPositiveZ
                                                                       : SparseVoxelMicroContext::Interior;
      uint32_t packedFlags = shell ? WorldMapTerrainColumnExteriorShell : 0u;
      if (first < unstitched)
        packedFlags |= WorldMapTerrainColumnBoundaryStitched;
      page.columns[index] = {columnTop, first, (samples.materials[index] & 0xffu) | (static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface) << 8u), packedFlags};
      page.walls[index] = {unstitched, threshold, static_cast<uint32_t>(wall) | (static_cast<uint32_t>(boundary) << 8u), 0u};
    }
  return validateWorldMapTerrainColumns(page, error);
}

uint64_t terrainGPUColumnSemanticHash(const WorldMapTerrainColumnPage &page)
{
  uint64_t hash = 1469598103934665603ull;
  mix(hash, page.side);
  for (const auto &column : page.columns)
  {
    mix(hash, column.topY);
    mix(hash, column.firstSolidY);
    mix(hash, column.packedMaterialContext);
    mix(hash, column.packedFlags);
  }
  for (const auto &wall : page.walls)
  {
    mix(hash, wall.firstSolidYWithoutBoundaryStitch);
    mix(hash, wall.lowestNeighbourY);
    mix(hash, wall.packedContexts);
  }
  return hash;
}

} // namespace voxel::svraster
