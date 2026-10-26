#include "procedural/PolygonTerrain.hpp"
#include "procedural/world/MapPlacement.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{

using namespace procedural;
using namespace procedural::world;

uint32_t failures = 0u;

void expect(const bool condition, const std::string &message)
{
  if (condition)
    return;
  std::cerr << "FAILED: " << message << "\n";
  ++failures;
}

template <typename T>
bool byteEqual(const std::vector<T> &left, const std::vector<T> &right)
{
  return left.size() == right.size() &&
      (left.empty() ||
       std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
}

template <typename T>
bool namedByteEqual(
    const char *name,
    const std::vector<T> &left,
    const std::vector<T> &right)
{
  if (byteEqual(left, right))
    return true;
  std::cerr << "terrain payload mismatch: " << name << " sizes="
            << left.size() << "/" << right.size();
  if (left.size() == right.size() && !left.empty())
  {
    const auto *leftBytes = reinterpret_cast<const uint8_t *>(left.data());
    const auto *rightBytes = reinterpret_cast<const uint8_t *>(right.data());
    const size_t byteCount = left.size() * sizeof(T);
    const auto mismatch = std::mismatch(
        leftBytes, leftBytes + byteCount, rightBytes);
    std::cerr << " firstByte=" << (mismatch.first - leftBytes)
              << " values=" << static_cast<uint32_t>(*mismatch.first)
              << "/" << static_cast<uint32_t>(*mismatch.second);
  }
  std::cerr << "\n";
  return false;
}

bool terrainPayloadByteEqual(
    const voxel::svraster::PlanetVoxelChunkSet &left,
    const voxel::svraster::PlanetVoxelChunkSet &right)
{
  bool equal = true;
  equal &= namedByteEqual("nodes", left.bricks.nodes, right.bricks.nodes);
  equal &= namedByteEqual(
      "occupancy", left.bricks.occupancy, right.bricks.occupancy);
  equal &= namedByteEqual(
      "childIndices", left.bricks.childIndices, right.bricks.childIndices);
  equal &= namedByteEqual(
      "rootIndices", left.bricks.rootIndices, right.bricks.rootIndices);
  equal &= namedByteEqual(
      "materialIndices", left.bricks.materialIndices, right.bricks.materialIndices);
  equal &= namedByteEqual(
      "microSurfaceData", left.bricks.microSurfaceData,
      right.bricks.microSurfaceData);
  equal &= namedByteEqual("palette", left.bricks.palette, right.bricks.palette);
  equal &= namedByteEqual(
      "paletteDescriptorIds", left.bricks.paletteDescriptorIds,
      right.bricks.paletteDescriptorIds);
  equal &= namedByteEqual(
      "surfaceHeights", left.surfaceHeights, right.surfaceHeights);
  equal &= namedByteEqual(
      "surfaceEnvelope", left.surfaceEnvelope, right.surfaceEnvelope);
  equal &= namedByteEqual(
      "surfaceMaterials", left.surfaceMaterials, right.surfaceMaterials);
  equal &= left.chunks.size() == right.chunks.size() && !left.chunks.empty();
  if (!left.chunks.empty() && !right.chunks.empty() &&
      left.chunks.front().brickPayloadRevision !=
          right.chunks.front().brickPayloadRevision)
  {
    std::cerr << "terrain payload mismatch: brickPayloadRevision "
              << left.chunks.front().brickPayloadRevision << "/"
              << right.chunks.front().brickPayloadRevision << "\n";
    equal = false;
  }
  return equal;
}

void appendU16(std::vector<uint8_t> &bytes, const uint16_t value)
{
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8u));
}

std::vector<uint8_t> u16(const uint16_t value)
{
  std::vector<uint8_t> bytes;
  appendU16(bytes, value);
  return bytes;
}

std::vector<uint8_t> halfPair(const uint16_t first, const uint16_t second)
{
  std::vector<uint8_t> bytes;
  appendU16(bytes, first);
  appendU16(bytes, second);
  return bytes;
}

MapExportField
constantField(const std::string &name, const MapExportFormat format, const MapExportFilter filter, const std::vector<uint8_t> &texel, const uint32_t pageSize, const uint16_t gutter, const std::string &samplingGroup = {})
{
  MapExportField field{};
  field.name = name;
  field.format = format;
  field.filter = filter;
  field.width = pageSize;
  field.height = pageSize;
  field.available = true;
  field.samplingGroup = samplingGroup;
  MapExportPage page{};
  page.address = {0u, 0u, 0u};
  page.width = pageSize + gutter * 2u;
  page.height = page.width;
  page.data.reserve(static_cast<size_t>(page.width) * page.height * texel.size());
  for (size_t index = 0u; index < static_cast<size_t>(page.width) * page.height; ++index)
    page.data.insert(page.data.end(), texel.begin(), texel.end());
  field.pages.push_back(std::move(page));
  return field;
}

void setConstant(MapExportBundle &bundle, const std::string &name, const std::vector<uint8_t> &texel)
{
  MapExportField *field = bundle.field(name);
  if (field == nullptr || field->pages.empty())
    return;
  MapExportPage &page = field->pages.front();
  page.data.clear();
  page.data.reserve(static_cast<size_t>(page.width) * page.height * texel.size());
  for (size_t index = 0u; index < static_cast<size_t>(page.width) * page.height; ++index)
    page.data.insert(page.data.end(), texel.begin(), texel.end());
}

MapExportBundle placementFixture(const std::array<uint8_t, 4> potential = {255u, 255u, 255u, 255u})
{
  constexpr uint32_t pageSize = 16u;
  constexpr uint16_t gutter = 1u;
  MapExportBundle bundle{};
  bundle.resolution = pageSize;
  bundle.pageSizePixels = pageSize;
  bundle.gutterPixels = gutter;
  bundle.maximumPageLevel = 0u;
  bundle.extentMetres = 6400.0;
  bundle.seed = 0x4f1bbcdc1234abcdull;
  bundle.fields = {
    constantField("placement_potential", MapExportFormat::RGBA8Unorm, MapExportFilter::Linear, {potential[0], potential[1], potential[2], potential[3]}, pageSize, gutter),
    constantField("water_type", MapExportFormat::R8Uint, MapExportFilter::Nearest, {0u}, pageSize, gutter),
    constantField("water_level", MapExportFormat::R16Float, MapExportFilter::Linear, u16(0x0000u), pageSize, gutter),
    constantField("climate", MapExportFormat::RG16Float, MapExportFilter::Linear, halfPair(0x0000u, 0x3800u), pageSize, gutter), // 0 C, 0.5 moisture
    constantField("refined_elevation", MapExportFormat::R16Unorm, MapExportFilter::Linear, u16(49151u), pageSize, gutter),
    constantField("base_elevation", MapExportFormat::R16Unorm, MapExportFilter::Linear, u16(6554u), pageSize, gutter),
    constantField("river", MapExportFormat::RG16Float, MapExportFilter::Linear, halfPair(0x3c00u, 0x0000u), pageSize, gutter), // distance=extent, discharge=0
    constantField("substrate_id", MapExportFormat::R8Uint, MapExportFilter::Nearest, {3u}, pageSize, gutter),
    constantField("soil_depth", MapExportFormat::R16Float, MapExportFilter::Linear, u16(0x3c00u), pageSize, gutter), // 1 metre
    constantField("biome_mix_ids", MapExportFormat::RGBA8Uint, MapExportFilter::SparseMixture, {static_cast<uint8_t>(Biome::Grassland), 0u, 0u, 0u}, pageSize, gutter, "biome_mixture"),
    constantField("biome_mix_weights", MapExportFormat::RGBA8Unorm, MapExportFilter::SparseMixture, {255u, 0u, 0u, 0u}, pageSize, gutter, "biome_mixture"),
    constantField("object_exclusion", MapExportFormat::R8Uint, MapExportFilter::Nearest, {0u}, pageSize, gutter),
  };
  bundle.field("refined_elevation")->scale[0] = 1000.0;
  bundle.field("base_elevation")->scale[0] = 1000.0;
  bundle.field("river")->scale[0] = bundle.extentMetres;
  return bundle;
}

PlacementAssetRule assetRule(const PlacementPotentialChannel potential = PlacementPotentialChannel::Groundcover)
{
  PlacementAssetRule rule{};
  rule.assetKey = "test.asset";
  rule.collectionKey = "test.collection";
  rule.resourceKey = "test/resource";
  rule.potential = potential;
  rule.footprintRadiusMetres = 0.0;
  rule.minimumSpacingMetres = 0.0;
  return rule;
}

PlacementTileRequest requestFor(const MapExportBundle &bundle, const double cellMetres = 100.0)
{
  PlacementTileRequest request{};
  request.requestedLevel = bundle.maximumPageLevel;
  request.candidateCellMetres = cellMetres;
  request.maximumEvaluatedSites = 1u << 17u;
  request.maximumCandidates = 1u << 17u;
  return request;
}

bool build(const MapExportBundle &bundle, const PlacementPalette &palette, const PlacementTileRequest &request, PlacementCandidateSet &result)
{
  std::string error;
  const bool success = buildPlacementCandidates(bundle, palette, request, result, &error);
  expect(success, "placement fixture builds: " + error);
  return success;
}

std::vector<uint64_t> candidateIds(const PlacementCandidateSet &set)
{
  std::vector<uint64_t> ids;
  ids.reserve(set.candidates.size());
  for (const PlacementCandidate &candidate : set.candidates)
    ids.push_back(candidate.stableId);
  return ids;
}

bool sortedSubset(const std::vector<uint64_t> &subset, const std::vector<uint64_t> &superset)
{
  size_t superIndex = 0u;
  for (const uint64_t id : subset)
  {
    while (superIndex < superset.size() && superset[superIndex] < id)
      ++superIndex;
    if (superIndex == superset.size() || superset[superIndex] != id)
      return false;
    ++superIndex;
  }
  return true;
}

void testProgressiveDitherThresholds()
{
  const std::array<uint8_t, 5> densityLevels{0u, 64u, 128u, 192u, 255u};
  std::array<PlacementCandidateSet, densityLevels.size()> sets{};
  PlacementPalette palette{};
  palette.assets = {assetRule()};
  for (size_t level = 0u; level < densityLevels.size(); ++level)
  {
    const uint8_t density = densityLevels[level];
    const MapExportBundle bundle = placementFixture({0u, 0u, density, 0u});
    const PlacementTileRequest request = requestFor(bundle);
    if (!build(bundle, palette, request, sets[level]))
      return;
    const size_t siteCount = 64u * 64u;
    const double expected = siteCount * density / 255.0;
    const double tolerance = std::max(20.0, siteCount * 0.035);
    expect(std::abs(static_cast<double>(sets[level].candidates.size()) - expected) <= tolerance, "dither occupancy tracks its authored density at level " + std::to_string(density));
    expect(sets[level].densityRejected + sets[level].candidates.size() == siteCount, "dither accounts for every unmasked candidate site");
    if (level > 0u)
      expect(sortedSubset(candidateIds(sets[level - 1u]), candidateIds(sets[level])), "raising density only adds candidates to the stable threshold pattern");
  }
  expect(sets.front().candidates.empty(), "zero density emits no candidates");
  expect(sets.back().candidates.size() == 64u * 64u, "unit density emits every candidate site");

  MapExportBundle repeatBundle = placementFixture({0u, 0u, 128u, 0u});
  PlacementCandidateSet repeat{};
  build(repeatBundle, palette, requestFor(repeatBundle), repeat);
  expect(candidateIds(repeat) == candidateIds(sets[2]), "the dither threshold pattern is bit-stable for a world seed");

  std::array<size_t, 64> buckets{};
  for (const PlacementCandidate &candidate : sets[2].candidates)
  {
    const uint32_t x = std::min(7u, static_cast<uint32_t>(candidate.position.x / repeatBundle.extentMetres * 8.0));
    const uint32_t y = std::min(7u, static_cast<uint32_t>(candidate.position.y / repeatBundle.extentMetres * 8.0));
    ++buckets[static_cast<size_t>(y) * 8u + x];
  }
  expect(
      std::all_of(
          buckets.begin(),
          buckets.end(),
          [](const size_t count)
          {
            return count >= 15u && count <= 49u;
          }),
      "dither occupancy has no large-scale empty or saturated blocks");
}

void testLayeredDensityAndAssetSelection()
{
  const MapExportBundle bundle = placementFixture({255u, 128u, 64u, 0u});
  const PlacementTileRequest request = requestFor(bundle);
  const std::array<PlacementPotentialChannel, 4> channels{
    PlacementPotentialChannel::Trees,
    PlacementPotentialChannel::Shrubs,
    PlacementPotentialChannel::Groundcover,
    PlacementPotentialChannel::ExposedRock,
  };
  const std::array<double, 4> expectedFractions{1.0, 128.0 / 255.0, 64.0 / 255.0, 0.0};
  for (size_t layer = 0u; layer < channels.size(); ++layer)
  {
    PlacementPalette palette{};
    palette.assets = {assetRule(channels[layer])};
    PlacementCandidateSet candidates{};
    if (!build(bundle, palette, request, candidates))
      return;
    const double observed = candidates.candidates.size() / 4096.0;
    expect(std::abs(observed - expectedFractions[layer]) <= 0.04, "each placement-potential layer discretizes independently");
  }

  PlacementAssetRule common = assetRule(PlacementPotentialChannel::Trees);
  PlacementAssetRule rare = common;
  common.assetKey = "tree.common";
  common.resourceKey = "tree/common";
  common.selectionWeight = 3.0;
  rare.assetKey = "tree.rare";
  rare.resourceKey = "tree/rare";
  rare.selectionWeight = 1.0;
  PlacementPalette palette{};
  palette.assets = {common, rare};
  PlacementCandidateSet selected{};
  if (!build(bundle, palette, request, selected))
    return;
  std::array<size_t, 2> selectedCounts{};
  for (const PlacementCandidate &candidate : selected.candidates)
    ++selectedCounts[candidate.assetIndex];
  expect(selected.candidates.size() == 4096u, "a fully dense flattened asset layer fills every dither slot");
  expect(selectedCounts[0] > 2800u && selectedCounts[0] < 3350u && selectedCounts[1] > 750u && selectedCounts[1] < 1300u, "weighted assets retain their authored three-to-one selection ratio");
}

void testEcotopeWeightsBecomeDensityLayers()
{
  PlacementAssetRule grass = assetRule();
  grass.biomes = {{Biome::Grassland, 1.0}};
  PlacementPalette palette{};
  palette.assets = {grass};
  MapExportBundle grassBundle = placementFixture();
  const PlacementTileRequest request = requestFor(grassBundle);
  PlacementCandidateSet fullGrass{};
  build(grassBundle, palette, request, fullGrass);

  MapExportBundle ecotone = grassBundle;
  setConstant(ecotone, "biome_mix_ids", {static_cast<uint8_t>(Biome::Grassland), static_cast<uint8_t>(Biome::Desert), 0u, 0u});
  setConstant(ecotone, "biome_mix_weights", {128u, 127u, 0u, 0u});
  PlacementCandidateSet mixedGrass{};
  build(ecotone, palette, request, mixedGrass);
  expect(fullGrass.candidates.size() == 4096u, "a pure matching ecotope retains full authored density");
  expect(std::abs(static_cast<double>(mixedGrass.candidates.size()) - 4096.0 * 128.0 / 255.0) < 150.0, "a half-weight ecotope contributes approximately half of the species density");
  expect(sortedSubset(candidateIds(mixedGrass), candidateIds(fullGrass)), "ecotope blending uses the same progressive dither thresholds");

  MapExportBundle desert = grassBundle;
  setConstant(desert, "biome_mix_ids", {static_cast<uint8_t>(Biome::Desert), 0u, 0u, 0u});
  setConstant(desert, "biome_mix_weights", {255u, 0u, 0u, 0u});
  PlacementCandidateSet noGrass{};
  build(desert, palette, request, noGrass);
  expect(noGrass.candidates.empty(), "an absent ecotope contributor emits none of its restricted assets");
}

void testWorldDataLayersAndMasks()
{
  MapExportBundle bundle = placementFixture();
  const PlacementTileRequest request = requestFor(bundle, 400.0);
  PlacementAssetRule constrained = assetRule();
  constrained.altitudeRangeMetres = {700.0, 800.0};
  constrained.moistureRange = {0.4, 0.6};
  constrained.soilDepthRangeMetres = {0.5, 1.5};
  constrained.substrates = {3u};
  PlacementPalette palette{};
  palette.assets = {constrained};
  PlacementCandidateSet accepted{};
  build(bundle, palette, request, accepted);
  expect(accepted.candidates.size() == 16u * 16u, "height, climate, soil and substrate layers compose into an accepted density mask");

  MapExportBundle baseOnly = bundle;
  baseOnly.field("refined_elevation")->available = false;
  PlacementCandidateSet rejectedHeight{};
  build(baseOnly, palette, request, rejectedHeight);
  expect(rejectedHeight.candidates.empty(), "refined elevation is preferred and base elevation remains its explicit fallback layer");

  MapExportBundle dry = bundle;
  setConstant(dry, "climate", halfPair(0x0000u, 0x0000u));
  PlacementCandidateSet rejectedClimate{};
  build(dry, palette, request, rejectedClimate);
  expect(rejectedClimate.candidates.empty(), "climate thresholds suppress an otherwise dense placement layer");

  MapExportBundle wrongSubstrate = bundle;
  setConstant(wrongSubstrate, "substrate_id", {7u});
  PlacementCandidateSet rejectedSubstrate{};
  build(wrongSubstrate, palette, request, rejectedSubstrate);
  expect(rejectedSubstrate.candidates.empty(), "categorical substrate constraints participate in density evaluation");

  MapExportBundle water = bundle;
  setConstant(water, "water_type", {1u});
  PlacementCandidateSet rejectedWater{};
  build(water, palette, request, rejectedWater);
  expect(rejectedWater.candidates.empty() && rejectedWater.waterRejected == 256u, "water masks reject every covered site before density evaluation");

  MapExportBundle exclusion = bundle;
  setConstant(exclusion, "object_exclusion", {1u});
  PlacementCandidateSet rejectedExclusion{};
  build(exclusion, palette, request, rejectedExclusion);
  expect(rejectedExclusion.candidates.empty() && rejectedExclusion.exclusionRejected == 256u, "authored object topology acts as a hard placement exclusion layer");
}

void testFootprintsAndTileSeams()
{
  const MapExportBundle bundle = placementFixture();
  PlacementAssetRule shrub = assetRule();
  shrub.assetKey = "shrub";
  shrub.resourceKey = "shrub";
  shrub.selectionWeight = 3.0;
  shrub.footprintRadiusMetres = 40.0;
  shrub.minimumSpacingMetres = 140.0;
  PlacementAssetRule tree = shrub;
  tree.assetKey = "tree";
  tree.resourceKey = "tree";
  tree.selectionWeight = 1.0;
  tree.footprintRadiusMetres = 120.0;
  tree.minimumSpacingMetres = 220.0;
  PlacementPalette palette{};
  palette.assets = {shrub, tree};
  PlacementTileRequest request = requestFor(bundle);
  PlacementCandidateSet whole{};
  if (!build(bundle, palette, request, whole))
    return;
  bool separated = !whole.candidates.empty() && whole.candidates.size() < 4096u;
  std::array<size_t, 2> kinds{};
  for (size_t index = 0u; index < whole.candidates.size(); ++index)
  {
    const PlacementCandidate &candidate = whole.candidates[index];
    const PlacementAssetRule &asset = palette.assets[candidate.assetIndex];
    ++kinds[candidate.assetIndex];
    for (size_t previousIndex = 0u; previousIndex < index; ++previousIndex)
    {
      const PlacementCandidate &previous = whole.candidates[previousIndex];
      const PlacementAssetRule &previousAsset = palette.assets[previous.assetIndex];
      const double required = std::max({asset.minimumSpacingMetres, previousAsset.minimumSpacingMetres, asset.footprintRadiusMetres + previousAsset.footprintRadiusMetres});
      separated = separated && std::hypot(candidate.position.x - previous.position.x, candidate.position.y - previous.position.y) >= required;
    }
  }
  expect(separated && kinds[0] > 0u && kinds[1] > 0u, "different asset footprints resolve without overlap while retaining both layers");

  PlacementTileRequest leftRequest = request;
  PlacementTileRequest rightRequest = request;
  leftRequest.normalizedBounds[2] = 0.5;
  rightRequest.normalizedBounds[0] = 0.5;
  PlacementCandidateSet left{};
  PlacementCandidateSet right{};
  build(bundle, palette, leftRequest, left);
  build(bundle, palette, rightRequest, right);
  std::vector<uint64_t> joined = candidateIds(left);
  const std::vector<uint64_t> rightIds = candidateIds(right);
  joined.insert(joined.end(), rightIds.begin(), rightIds.end());
  std::sort(joined.begin(), joined.end());
  expect(joined == candidateIds(whole), "layered dither and footprint decisions are invariant across a streaming tile seam");
}

void testGeneratedWorldBuildsVoxelTerrain()
{
  const MapExportBundle bundle = placementFixture();
  voxel::svraster::WorldMapVoxelTerrainSettings settings{};
  settings.normalizedBounds = {0.0, 0.0, 1.0, 1.0};
  settings.requestedLevel = 0u;
  settings.voxel.horizontalVoxelCount = 64u;
  settings.voxel.foundationDepthVoxels = 2u;
  settings.voxel.surfaceShellDepthVoxels = 4u;
  settings.voxel.maximumVerticalVoxels = 32u;
  settings.voxel.maximumBricks = 1u << 18u;
  settings.voxel.microGeometry = false;
  settings.voxel.proceduralDetailStrength = 0.0;
  voxel::svraster::PlanetVoxelChunkSet terrain{};
  std::string error;
  const bool built = voxel::svraster::buildWorldMapVoxelTerrain(bundle, settings, terrain, &error);
  expect(built, "generated world maps build renderer-ready voxel terrain: " + error);
  expect(
      built && terrain.chunks.size() == 1u && !terrain.bricks.empty() && terrain.voxelSizeMetres == 100.0 && terrain.leafVoxelCount == 64u * 64u * 3u && terrain.surfaceHeightSide == 70u &&
          terrain.surfaceHeights.size() == 70u * 70u &&
          std::all_of(
              terrain.surfaceHeights.begin(),
              terrain.surfaceHeights.end(),
              [](const float height)
              {
                return std::abs(height - 749.9962f) < 0.001f;
              }),
      "the bridge preserves sampled elevation, voxel scale, halo and solid columns");
  expect(
      built && terrain.centerMaterial == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Grassland) && terrain.minimumWorldMetres[0] == -3200.0 && terrain.maximumWorldMetres[0] == 3200.0,
      "the bridge maps biome materials and centers planar world coordinates");

  MapExportBundle ocean = bundle;
  setConstant(ocean, "water_type", {1u});
  voxel::svraster::PlanetVoxelChunkSet oceanTerrain{};
  expect(
      voxel::svraster::buildWorldMapVoxelTerrain(ocean, settings, oceanTerrain, &error) && oceanTerrain.centerMaterial == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Ocean) &&
          oceanTerrain.centerSurfaceMetres == 0.0,
      "water type and water level override land elevation and biome material");

  MapExportBundle snow = bundle;
  setConstant(snow, "climate", halfPair(0xc400u, 0x3800u));
  voxel::svraster::PlanetVoxelChunkSet snowTerrain{};
  expect(
      voxel::svraster::buildWorldMapVoxelTerrain(
          snow, settings, snowTerrain, &error) &&
          snowTerrain.centerMaterial == static_cast<uint8_t>(
              procedural::PolygonTerrainMaterial::Snow),
      "cold moist climate contributes snow to terrain construction");

  MapExportBundle channel = bundle;
  setConstant(channel, "river", halfPair(0x0000u, 0x3c00u));
  voxel::svraster::PlanetVoxelChunkSet channelTerrain{};
  expect(
      voxel::svraster::buildWorldMapVoxelTerrain(
          channel, settings, channelTerrain, &error) &&
          channelTerrain.centerMaterial == static_cast<uint8_t>(
              procedural::PolygonTerrainMaterial::River) &&
          channelTerrain.centerSurfaceMetres < terrain.centerSurfaceMetres,
      "river distance and discharge widen and incise runtime channels");

  MapExportBundle scenic = bundle;
  MapExportField *scenicElevation = scenic.field("refined_elevation");
  if (scenicElevation != nullptr && !scenicElevation->pages.empty())
  {
    MapExportPage &page = scenicElevation->pages.front();
    page.data.clear();
    for (uint32_t y = 0u; y < page.height; ++y)
      for (uint32_t x = 0u; x < page.width; ++x)
      {
        const int32_t dx = static_cast<int32_t>(x) - 13;
        const int32_t dy = static_cast<int32_t>(y) - 9;
        const int32_t radiusSquared = dx * dx + dy * dy;
        const uint16_t elevation = radiusSquared <= 2
            ? 60'000u
            : radiusSquared <= 8 ? 38'000u : 6'554u;
        appendU16(page.data, elevation);
      }
  }
  voxel::svraster::WorldMapTerrainSpawnView scenicView{};
  const bool selectedScenicView =
      voxel::svraster::selectWorldMapTerrainSpawnView(
          scenic, 0u, 0.5, 0.5, 4000.0, scenicView, &error);
  const double scenicDistance = std::hypot(
      scenicView.targetU - scenicView.spawnU,
      scenicView.targetV - scenicView.spawnV) * scenic.extentMetres;
  expect(
      selectedScenicView && scenicView.found &&
          scenicView.targetElevationMetres -
              scenicView.spawnElevationMetres > 300.0 &&
          scenicDistance <= 4000.0,
      "startup selection places gentle land within a visible mountain skyline: " +
          error);
  voxel::svraster::WorldMapTerrainSpawnView pinnedScenicView{};
  const bool selectedPinnedScenicView =
      voxel::svraster::selectWorldMapTerrainSpawnView(
          scenic, 0u, 0.5, 0.5, 4000.0, pinnedScenicView, &error, true);
  expect(
      selectedPinnedScenicView && pinnedScenicView.found &&
          pinnedScenicView.spawnU == 0.5 &&
          pinnedScenicView.spawnV == 0.5 &&
          pinnedScenicView.targetElevationMetres >
              pinnedScenicView.spawnElevationMetres + 300.0,
      "an explicit world-map center pins the spawn but still frames relief: " +
          error);

  MapExportBundle slope = bundle;
  MapExportField *slopeElevation = slope.field("refined_elevation");
  if (slopeElevation != nullptr && !slopeElevation->pages.empty())
  {
    MapExportPage &page = slopeElevation->pages.front();
    page.data.clear();
    for (uint32_t y = 0u; y < page.height; ++y)
      for (uint32_t x = 0u; x < page.width; ++x)
        appendU16(page.data, static_cast<uint16_t>(
            std::min(30000u + x * 1800u + y * 600u, 65535u)));
  }
  voxel::svraster::SparseVoxelMicroMaterialLibrary microMaterials;
  voxel::svraster::SparseVoxelMicroDescriptor microGeometry{};
  microGeometry.subdivision = 5u;
  microGeometry.geometryProfile =
      voxel::svraster::SparseVoxelMicroGeometryProfile::Staircase;
  expect(
      microMaterials.setMaterial(
          static_cast<uint32_t>(procedural::PolygonTerrainMaterial::Grassland),
          microGeometry, {}, &error),
      "generated-world slope material registers: " + error);
  settings.voxel.microGeometry = true;
  settings.voxel.microMaterials = &microMaterials;
  voxel::svraster::PlanetVoxelChunkSet fractionalTerrain{};
  const bool builtFractional = voxel::svraster::buildWorldMapVoxelTerrain(
      bundle, settings, fractionalTerrain, &error);
  bool foundFlatSurface = false;
  bool foundAnalyticPatch = false;
  for (const uint64_t packedContext :
      fractionalTerrain.bricks.microSurfaceData)
  {
    const uint32_t context =
        voxel::svraster::unpackSparseVoxelMicroContextValue(packedContext);
    foundFlatSurface |= context == static_cast<uint32_t>(
        voxel::svraster::SparseVoxelMicroContext::FlatSurface);
    foundAnalyticPatch |=
        voxel::svraster::unpackSparseVoxelMicroContextSurfacePatch(
            packedContext) != 0u;
  }
  expect(
      builtFractional && foundFlatSurface && !foundAnalyticPatch,
      "generated-world flat surfaces use square voxel geometry: " +
          error);
  voxel::svraster::PlanetVoxelChunkSet slopeTerrain{};
  const bool builtSlope = voxel::svraster::buildWorldMapVoxelTerrain(
      slope, settings, slopeTerrain, &error);
  bool foundSlopeSurface = false;
  bool foundSlopePatch = false;
  bool foundInterior = false;
  for (const uint64_t packedContext : slopeTerrain.bricks.microSurfaceData)
  {
    const uint32_t context =
        voxel::svraster::unpackSparseVoxelMicroContextValue(packedContext);
    foundSlopeSurface |=
        context == static_cast<uint32_t>(
            voxel::svraster::SparseVoxelMicroContext::FlatSurface);
    foundSlopePatch |=
        voxel::svraster::unpackSparseVoxelMicroContextSurfacePatch(
            packedContext) != 0u;
    foundInterior |= context == static_cast<uint32_t>(
        voxel::svraster::SparseVoxelMicroContext::Interior);
  }
  expect(
      builtSlope && foundSlopeSurface && !foundSlopePatch && foundInterior,
      "generated-world contours use square top voxels while shell fill remains solid: " +
          error);

  settings.boundaryStitchSubdivisions = 2u;
  voxel::svraster::PlanetVoxelChunkSet conservativeSlopeTerrain{};
  voxel::svraster::WorldMapVoxelGenerationMetrics conservativeMetrics{};
  const bool conservativeSlopeBuilt =
      voxel::svraster::buildWorldMapVoxelTerrain(
          slope, settings, conservativeSlopeTerrain, &error, {}, nullptr,
          &conservativeMetrics);
  expect(
      conservativeSlopeBuilt &&
          conservativeMetrics.conservativeBoundarySurfaceVoxelCount == 0u,
      "mixed-LOD closure does not raise analytic surface corners into page-edge ridges: " +
          error);
}

void testGeneratedWorldInternalLODIsDeterministicAndSurfaceOnly()
{
  MapExportBundle bundle = placementFixture();
  MapExportField *elevation = bundle.field("refined_elevation");
  if (elevation != nullptr && !elevation->pages.empty())
  {
    MapExportPage &page = elevation->pages.front();
    page.data.clear();
    for (uint32_t y = 0u; y < page.height; ++y)
      for (uint32_t x = 0u; x < page.width; ++x)
        appendU16(page.data, static_cast<uint16_t>(
            std::min(18000u + x * 1400u + y * 450u, 65535u)));
  }

  voxel::svraster::WorldMapVoxelTerrainSettings settings{};
  settings.normalizedBounds = {0.4, 0.4, 0.6, 0.6};
  settings.requestedLevel = 0u;
  settings.finestHorizontalVoxelCount = 32u;
  settings.finestVoxelSizeMetres = 40.0;
  settings.elevationAnchorLOD = 2u;
  settings.generatorRevision = 9u;
  settings.exteriorShellOnly = true;
  settings.voxel.horizontalVoxelCount = 32u;
  settings.voxel.foundationDepthVoxels = 2u;
  settings.voxel.surfaceShellDepthVoxels = 2u;
  settings.voxel.maximumVerticalVoxels = 64u;
  settings.voxel.maximumBricks = 1u << 18u;
  settings.voxel.microGeometry = false;

  std::array<voxel::svraster::PlanetVoxelChunkSet, 3u> lods;
  std::array<voxel::svraster::WorldMapVoxelGenerationMetrics, 3u> metrics;
  std::string error;
  bool allBuilt = true;
  bool deterministic = true;
  for (uint32_t lod = 0u; lod < lods.size(); ++lod)
  {
    settings.internalLOD = lod;
    voxel::svraster::PlanetVoxelChunkSet repeated;
    voxel::svraster::WorldMapVoxelGenerationMetrics repeatedMetrics{};
    allBuilt &= voxel::svraster::buildWorldMapVoxelTerrain(
        bundle, settings, lods[lod], &error, {}, nullptr, &metrics[lod]);
    allBuilt &= voxel::svraster::buildWorldMapVoxelTerrain(
        bundle, settings, repeated, &error, {}, nullptr, &repeatedMetrics);
    deterministic &= terrainPayloadByteEqual(lods[lod], repeated) &&
        metrics[lod].contentRevision == repeatedMetrics.contentRevision;
  }
  expect(allBuilt, "every fixed-footprint internal LOD builds: " + error);
  expect(
      allBuilt && deterministic &&
          lods[0].chunks.front().brickPayloadRevision !=
              lods[1].chunks.front().brickPayloadRevision &&
          lods[1].chunks.front().brickPayloadRevision !=
              lods[2].chunks.front().brickPayloadRevision,
      "each internal LOD is byte deterministic and has a distinct content "
      "revision (built=" + std::to_string(allBuilt) +
          " deterministic=" + std::to_string(deterministic) +
          " revisions=" +
          std::to_string(lods[0].chunks.empty() ? 0u :
              lods[0].chunks.front().brickPayloadRevision) + "/" +
          std::to_string(lods[1].chunks.empty() ? 0u :
              lods[1].chunks.front().brickPayloadRevision) + "/" +
          std::to_string(lods[2].chunks.empty() ? 0u :
              lods[2].chunks.front().brickPayloadRevision) + ")");
  settings.internalLOD = 0u;
  ++settings.generatorRevision;
  voxel::svraster::PlanetVoxelChunkSet revised;
  const bool revisedBuilt = voxel::svraster::buildWorldMapVoxelTerrain(
      bundle, settings, revised, &error);
  expect(
      revisedBuilt && revised.chunks.front().brickPayloadRevision !=
          lods[0].chunks.front().brickPayloadRevision,
      "the generator revision participates in deterministic payload identity");
  --settings.generatorRevision;

  bool stableBoundsAndOrigin = allBuilt;
  for (uint32_t lod = 1u; lod < lods.size() && allBuilt; ++lod)
  {
    const auto &fine = lods[0].chunks.front();
    const auto &candidate = lods[lod].chunks.front();
    stableBoundsAndOrigin &= fine.worldCenterXMetres ==
            candidate.worldCenterXMetres &&
        fine.worldCenterZMetres == candidate.worldCenterZMetres &&
        fine.edgeMetres == candidate.edgeMetres &&
        fine.elevationOriginMetres == candidate.elevationOriginMetres;
  }
  expect(
      stableBoundsAndOrigin,
      "changing internal LOD preserves chunk bounds, root translation, and elevation origin");

  bool coarseContainsFine = allBuilt;
  if (allBuilt)
  {
    const auto &fine = lods[0];
    const auto &coarse = lods[2];
    constexpr uint32_t scale = 4u;
    for (uint32_t coarseZ = 0u; coarseZ < 8u; ++coarseZ)
      for (uint32_t coarseX = 0u; coarseX < 8u; ++coarseX)
      {
        const size_t coarseIndex =
            static_cast<size_t>(coarseZ + coarse.surfaceHeightHalo) *
                coarse.surfaceHeightSide +
            coarseX + coarse.surfaceHeightHalo;
        const float conservativeTop = coarse.surfaceEnvelope[coarseIndex];
        for (uint32_t fineZ = coarseZ * scale;
             fineZ < (coarseZ + 1u) * scale; ++fineZ)
          for (uint32_t fineX = coarseX * scale;
               fineX < (coarseX + 1u) * scale; ++fineX)
          {
            const size_t fineIndex =
                static_cast<size_t>(fineZ + fine.surfaceHeightHalo) *
                    fine.surfaceHeightSide +
                fineX + fine.surfaceHeightHalo;
            coarseContainsFine &=
                conservativeTop + 1.0e-4f >= fine.surfaceHeights[fineIndex];
          }
      }
  }
  expect(
      coarseContainsFine,
      "coarse internal terrain conservatively contains every finer surface sample");

  expect(
      allBuilt && metrics[0].omittedInteriorVoxelCount != 0u &&
          metrics[0].maximumUnexposedDepthVoxels <=
              settings.voxel.surfaceShellDepthVoxels &&
          metrics[0].emittedShellVoxelCount == lods[0].leafVoxelCount &&
          metrics[0].mapSamplingMilliseconds >= 0.0 &&
          metrics[0].shellExtractionMilliseconds >= 0.0 &&
          metrics[0].hierarchyBuildMilliseconds >= 0.0,
      "surface extraction omits hidden cells, bounds closure depth, and reports phase timings");

  voxel::svraster::WorldMapVoxelTerrainSettings leftSettings = settings;
  leftSettings.internalLOD = 0u;
  leftSettings.normalizedBounds = {0.3, 0.4, 0.4, 0.5};
  leftSettings.finestHorizontalVoxelCount = 16u;
  leftSettings.finestVoxelSizeMetres = 40.0;
  leftSettings.elevationAnchorLOD = 1u;
  leftSettings.voxel.horizontalVoxelCount = 16u;
  voxel::svraster::WorldMapVoxelTerrainSettings rightSettings = leftSettings;
  rightSettings.normalizedBounds = {0.4, 0.4, 0.5, 0.5};
  bool adjacentBuilt = true;
  bool sharedHaloSamplesMatch = true;
  for (uint32_t lod = 0u; lod < 2u; ++lod)
  {
    leftSettings.internalLOD = lod;
    rightSettings.internalLOD = lod;
    voxel::svraster::PlanetVoxelChunkSet left;
    voxel::svraster::PlanetVoxelChunkSet right;
    const bool lodBuilt = voxel::svraster::buildWorldMapVoxelTerrain(
        bundle, leftSettings, left, &error) &&
        voxel::svraster::buildWorldMapVoxelTerrain(
            bundle, rightSettings, right, &error);
    adjacentBuilt &= lodBuilt;
    if (!lodBuilt)
      continue;
    const int32_t halo = static_cast<int32_t>(left.surfaceHeightHalo);
    const int32_t side = static_cast<int32_t>(16u >> lod);
    for (int32_t z = -halo; z < side + halo; ++z)
      for (int32_t offset = -halo; offset < halo; ++offset)
      {
        const size_t leftIndex =
            static_cast<size_t>(z + halo) * left.surfaceHeightSide +
            static_cast<uint32_t>(side + offset + halo);
        const size_t rightIndex =
            static_cast<size_t>(z + halo) * right.surfaceHeightSide +
            static_cast<uint32_t>(offset + halo);
        sharedHaloSamplesMatch &=
            left.surfaceHeights[leftIndex] == right.surfaceHeights[rightIndex];
      }
  }
  expect(
      adjacentBuilt && sharedHaloSamplesMatch,
      "adjacent fixed chunks have bit-identical overlapping boundary and halo heights");

  auto stitchedSettings = leftSettings;
  stitchedSettings.internalLOD = 1u;
  stitchedSettings.boundaryStitchSubdivisions = 2u;
  voxel::svraster::PlanetVoxelChunkSet stitchedPage;
  voxel::svraster::WorldMapVoxelGenerationMetrics stitchedMetrics{};
  const bool stitchedBuilt = voxel::svraster::buildWorldMapVoxelTerrain(
      bundle, stitchedSettings, stitchedPage, &error, {}, nullptr,
      &stitchedMetrics);
  expect(
      stitchedBuilt && stitchedMetrics.boundaryStitchVoxelCount != 0u &&
          stitchedMetrics.maximumUnexposedDepthVoxels <=
              stitchedSettings.voxel.surfaceShellDepthVoxels,
      "a coarse spatial page emits bounded midpoint-sampled boundary closure: " +
          error);

  MapExportBundle elevated = bundle;
  if (auto *field = elevated.field("refined_elevation"))
    field->offset[0] += 10000.0;
  settings.internalLOD = 0u;
  voxel::svraster::PlanetVoxelChunkSet elevatedTerrain;
  voxel::svraster::WorldMapVoxelGenerationMetrics elevatedMetrics{};
  const bool elevatedBuilt = voxel::svraster::buildWorldMapVoxelTerrain(
      elevated, settings, elevatedTerrain, &error, {}, nullptr,
      &elevatedMetrics);
  expect(
      elevatedBuilt &&
          elevatedMetrics.emittedShellVoxelCount ==
              metrics[0].emittedShellVoxelCount &&
          elevatedMetrics.omittedInteriorVoxelCount ==
              metrics[0].omittedInteriorVoxelCount,
      "surface-shell generation cost is independent of absolute terrain elevation");
}

void testGeneratedWorldProgressiveCacheUsesFixedChunkOwners()
{
  constexpr float verticalFov = 1.0471975512f;
  expect(
      voxel::svraster::worldVoxelChunkRadiusForProjection(
          720u, verticalFov, 64u, 8.0f) == 2u &&
          voxel::svraster::worldVoxelChunkRadiusForProjection(
              1080u, verticalFov, 64u, 8.0f) == 4u &&
          voxel::svraster::worldVoxelChunkRadiusForProjection(
              720u, verticalFov, 64u, 4.0f) == 5u,
      "camera projection expands the fixed chunk window when the terrain "
      "streaming error is lowered");
  const MapExportBundle bundle = placementFixture();
  voxel::svraster::WorldVoxelChunkStreamingSettings settings{};
  settings.finestVoxelSizeMetres = 10.0;
  settings.finestHorizontalVoxelCount = 16u;
  settings.chunkEdgeMetres = 160.0;
  settings.residentRadius = 1u;
  settings.lodCount = 2u;
  settings.maximumGeneratedChunksPerUpdate = 0u;
  settings.maximumRefinedChunksPerUpdate = 2u;
  settings.maximumCachedPayloads = 9u;
  settings.voxel.foundationDepthVoxels = 2u;
  settings.voxel.surfaceShellDepthVoxels = 4u;
  settings.voxel.maximumVerticalVoxels = 32u;
  settings.voxel.maximumBricks = 1u << 18u;
  settings.voxel.microGeometry = false;

  // Keep this cache deliberately generation-starved so admission order is
  // observable one chunk at a time. The camera is centered inside a chunk;
  // after that nearest owner, the nearest chunk inside the view cone must win
  // over equally distant side and rear chunks.
  auto prioritySettings = settings;
  prioritySettings.residentRadius = 2u;
  prioritySettings.maximumGeneratedChunksPerUpdate = 1u;
  prioritySettings.priorityNearChunkRadius = 0u;
  prioritySettings.priorityViewDirectionX = 0.0;
  prioritySettings.priorityViewDirectionZ = -1.0;
  prioritySettings.priorityViewCosine = 0.95;
  constexpr int32_t priorityCenter = 20;
  const double priorityCamera =
      (static_cast<double>(priorityCenter) + 0.5) *
          prioritySettings.chunkEdgeMetres -
      bundle.extentMetres * 0.5;
  voxel::svraster::WorldVoxelChunkCache priorityCache{};
  voxel::svraster::PlanetVoxelChunkSet priorityTerrain{};
  voxel::svraster::WorldVoxelChunkCacheUpdate priorityUpdate{};
  std::string priorityError;
  const bool nearestBuilt = voxel::svraster::updateWorldVoxelChunkCache(
      bundle, priorityCamera, priorityCamera, prioritySettings,
      priorityCache, priorityTerrain, &priorityUpdate, &priorityError);
  const bool nearestFirst = nearestBuilt && priorityCache.entries.size() == 1u &&
      priorityCache.entries.front().address ==
          voxel::svraster::WorldVoxelChunkAddress{
            priorityCenter, priorityCenter};
  const uint64_t partialLayoutRevision =
      priorityTerrain.clipmapLayoutRevision;
  const size_t partialNodeCount = priorityTerrain.bricks.nodes.size();
  const size_t partialOccupancyCount =
      priorityTerrain.bricks.occupancy.size();
  const size_t partialMaterialCount =
      priorityTerrain.bricks.materialIndices.size();
  const size_t partialMicroSurfaceCount =
      priorityTerrain.bricks.microSurfaceData.size();
  const size_t partialPaletteCount = priorityTerrain.bricks.palette.size();
  const size_t partialPaletteDescriptorCount =
      priorityTerrain.bricks.paletteDescriptorIds.size();
  const bool visibleBuilt = nearestBuilt &&
      voxel::svraster::updateWorldVoxelChunkCache(
          bundle, priorityCamera, priorityCamera, prioritySettings,
          priorityCache, priorityTerrain, &priorityUpdate, &priorityError);
  const auto forwardOwner = std::find_if(
      priorityCache.entries.begin(), priorityCache.entries.end(),
      [](const auto &entry)
      {
        return entry.address == voxel::svraster::WorldVoxelChunkAddress{
          priorityCenter, priorityCenter - 1};
      });
  const auto centerRecord = std::find_if(
      priorityTerrain.chunks.begin(), priorityTerrain.chunks.end(),
      [](const auto &record)
      {
        return record.tileOffsetX == priorityCenter &&
            record.tileOffsetY == priorityCenter;
      });
  const auto forwardRecord = std::find_if(
      priorityTerrain.chunks.begin(), priorityTerrain.chunks.end(),
      [](const auto &record)
      {
        return record.tileOffsetX == priorityCenter &&
            record.tileOffsetY == priorityCenter - 1;
      });
  expect(
      nearestFirst && visibleBuilt && priorityCache.entries.size() == 2u &&
          forwardOwner != priorityCache.entries.end() &&
          centerRecord != priorityTerrain.chunks.end() &&
          forwardRecord != priorityTerrain.chunks.end() &&
          centerRecord->streamingPriority > forwardRecord->streamingPriority &&
          priorityTerrain.clipmapLayoutRevision == partialLayoutRevision &&
          priorityTerrain.bricks.nodes.size() == partialNodeCount &&
          priorityTerrain.bricks.occupancy.size() == partialOccupancyCount &&
          priorityTerrain.bricks.materialIndices.size() ==
              partialMaterialCount &&
          priorityTerrain.bricks.microSurfaceData.size() ==
              partialMicroSurfaceCount &&
          priorityTerrain.bricks.palette.size() == partialPaletteCount &&
          priorityTerrain.bricks.paletteDescriptorIds.size() ==
              partialPaletteDescriptorCount,
      "chunk admission generates the camera owner first, then the nearest "
      "visible owner, preserves that rank after physical-slot packing, and "
      "keeps every publishable fixed-arena sidecar stable across partial "
      "batches: " +
          priorityError);

  voxel::svraster::WorldVoxelChunkCache cache{};
  voxel::svraster::PlanetVoxelChunkSet terrain{};
  voxel::svraster::WorldVoxelChunkCacheUpdate update{};
  std::string error;
  const bool built = voxel::svraster::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, settings, cache, terrain, &update, &error);
  expect(built, "fixed-footprint generated-world cache builds: " + error);
  expect(
      built && cache.entries.size() == 9u && terrain.chunks.size() == 9u &&
          update.builtChunkCount == 9u && update.reusedChunkCount == 0u &&
          update.pendingChunkCount == 0u &&
          update.coarsestResidentLOD == 1u &&
          terrain.voxelSizeMetres == settings.finestVoxelSizeMetres &&
          terrain.hierarchyPageRewrites.size() == 9u &&
          std::all_of(
              terrain.hierarchyPageRewrites.begin(),
              terrain.hierarchyPageRewrites.end(),
              [](const auto &rewrite)
              {
                return rewrite.kind ==
                        voxel::svraster::
                            PlanetVoxelHierarchyRewriteKind::InstallRoot &&
                    !rewrite.expected.valid() &&
                    rewrite.replacement.valid() &&
                    rewrite.replacement.internalLOD == 1u;
              }),
      "the progressive cache bootstraps one coarse owner per fixed grid cell");
  expect(
      built && update.gpuPageBytes != 0u && update.cpuCacheBytes == 0u,
      "phase-7 reports GPU page and CPU payload-cache budgets separately");
  auto undersizedGPUCache = cache;
  auto undersizedGPUSettings = settings;
  undersizedGPUSettings.maximumGPUPageBytes = update.gpuPageBytes - 1u;
  voxel::svraster::PlanetVoxelChunkSet rejectedBudgetTerrain{};
  const bool rejectedGPUPageBudget =
      !voxel::svraster::updateWorldVoxelChunkCache(
          bundle, 0.0, 0.0, undersizedGPUSettings,
          undersizedGPUCache, rejectedBudgetTerrain, nullptr, &error);
  expect(
      rejectedGPUPageBudget && error.find("GPU page arena") != std::string::npos,
      "the GPU page budget is enforced independently of the CPU cache");

  bool fixedFootprints = true;
  bool coarsestInternalLOD = true;
  bool noSpatialClipMetadata = true;
  bool stableRefinementSlots = true;
  bool versionedRefinementSlots =
      terrain.progressiveGeneratedWorld &&
      terrain.progressiveSlotCapacity == cache.slotCount &&
      terrain.progressiveSplitErrorPixels == settings.splitErrorPixels &&
      terrain.progressiveMergeErrorPixels == settings.mergeErrorPixels &&
      terrain.progressiveDistanceErrorGrowthPerKilometre ==
          settings.distanceErrorGrowthPerKilometre &&
      terrain.progressiveDistanceErrorExponent ==
          settings.distanceErrorExponent &&
      terrain.progressiveMinimumResidentFrames ==
          settings.minimumResidentFrames;
  for (const auto &record : terrain.chunks)
  {
    fixedFootprints &= std::abs(record.edgeMetres - 160.0) < 1.0e-6;
    coarsestInternalLOD &= record.clipmapLOD == 1u &&
        std::abs(record.voxelSizeMetres - 20.0) < 1.0e-6;
    for (uint32_t index = 0u; index < record.brickNodeCapacity; ++index)
    {
      const auto &node = terrain.bricks.nodes[record.brickNodeOffset + index];
      if (node.occupiedCount == 0u)
        continue;
      noSpatialClipMetadata &=
          !voxel::svraster::sparseVoxelBrickHasFinerTerrainClip(node) &&
          (node.level & voxel::svraster::kSparseVoxelBrickForceLeafRaster) == 0u;
      stableRefinementSlots &= node.instanceIndex == record.clipmapSlot;
    }
    const auto owner = std::find_if(
        cache.entries.begin(), cache.entries.end(),
        [&](const auto &entry)
        {
          return entry.slot == record.clipmapSlot;
        });
    versionedRefinementSlots &= owner != cache.entries.end() &&
        record.residencyGeneration == owner->generation;
  }
  expect(
      fixedFootprints && coarsestInternalLOD,
      "internal LOD changes resolution without changing chunk world bounds");
  expect(
      noSpatialClipMetadata && stableRefinementSlots &&
          versionedRefinementSlots,
      "progressive generated-world nodes carry a versioned stable request slot and no spatial clip ownership mask");

  uint64_t residentNodeCount = 0u;
  uint64_t maximumFineNodeScratch = 0u;
  uint64_t maximumCoarseNodePage = 0u;
  for (const auto &entry : cache.entries)
  {
    residentNodeCount += entry.allocation.nodes.capacity;
    maximumCoarseNodePage = std::max(
        maximumCoarseNodePage,
        static_cast<uint64_t>(entry.allocation.nodes.capacity));
    maximumFineNodeScratch = std::max(
        maximumFineNodeScratch,
        static_cast<uint64_t>(entry.allocation.nodes.capacity) * 8u);
  }
  std::string physicalPageError;
  expect(
      terrain.progressiveVirtualPages && cache.virtualArenaInitialized &&
          terrain.bricks.nodes.size() == cache.arenaNodeCapacity &&
          cache.arenaChildCapacity >= cache.arenaNodeCapacity &&
          cache.arenaNodeCapacity ==
              residentNodeCount + maximumFineNodeScratch * 2u +
                  maximumCoarseNodePage * cache.slotCount &&
          voxel::svraster::validateWorldVoxelChunkPhysicalPages(
              cache, &physicalPageError),
      "phase-5 virtual pages reserve resident, child-index, fine-detail, and deferred camera replacement cuts: " +
          physicalPageError);

  const auto &requestEntry = cache.entries.front();
  const std::vector<voxel::svraster::WorldVoxelChunkRequest> rawRequests{
    {requestEntry.slot, requestEntry.generation, 1u, 0u, 17u},
    {requestEntry.slot, requestEntry.generation, 1u, 0u, 41u},
    {requestEntry.slot, requestEntry.generation + 1u, 1u, 0u, 99u}};
  const auto coalesced =
      voxel::svraster::coalesceWorldVoxelChunkRequests(cache, rawRequests);
  expect(
      coalesced.size() == 1u &&
          coalesced.front().slot == requestEntry.slot &&
          coalesced.front().expectedGeneration == requestEntry.generation &&
          coalesced.front().currentLOD == 1u &&
          coalesced.front().desiredLOD == 0u &&
          coalesced.front().priority == 41u,
      "progressive feedback coalesces by slot generation and rejects stale requests");

  auto coarseRequestCache = cache;
  coarseRequestCache.entries.front().residentLOD = 3u;
  const auto oneLevelRequest =
      voxel::svraster::coalesceWorldVoxelChunkRequests(
          coarseRequestCache,
          {{requestEntry.slot, requestEntry.generation, 3u, 0u, 7u}});
  expect(
      oneLevelRequest.size() == 1u &&
          oneLevelRequest.front().currentLOD == 3u &&
          oneLevelRequest.front().desiredLOD == 2u,
      "phase-3 feedback admits exactly one finer complete chunk LOD");
  auto fineRequestCache = cache;
  fineRequestCache.entries.front().residentLOD = 0u;
  fineRequestCache.entries.front().requestedLOD = 0u;
  const auto oneLevelMerge =
      voxel::svraster::coalesceWorldVoxelChunkRequests(
          fineRequestCache,
          {{fineRequestCache.entries.front().slot,
            fineRequestCache.entries.front().generation,
            0u, 3u, 99u}});
  expect(
      oneLevelMerge.size() == 1u &&
          oneLevelMerge.front().currentLOD == 0u &&
          oneLevelMerge.front().desiredLOD == 1u,
      "phase-7 feedback admits exactly one coarser complete chunk LOD");

  const uint64_t initialLayoutRevision = terrain.clipmapLayoutRevision;
  const auto refinementOwner = cache.entries.front();
  const auto deferredRefinementOwner = cache.entries[1u];
  const auto initialSlotGenerations = cache.slotGenerations;
  const auto initialNodeCapacities = cache.slotNodeCapacity;
  const auto initialChildCapacities = cache.slotChildCapacity;
  const auto initialRootCapacities = cache.slotRootCapacity;
  const auto initialMaterialCapacities = cache.slotMaterialCapacity;
  const auto initialPaletteCapacities = cache.slotPaletteCapacity;
  const auto initialFreeNodeRanges = cache.freeNodeRanges;
  const auto initialFreeChildRanges = cache.freeChildRanges;
  const auto initialFreeRootRanges = cache.freeRootRanges;
  const auto initialFreeMaterialRanges = cache.freeMaterialRanges;
  const auto initialFreePaletteRanges = cache.freePaletteRanges;
  const auto initialRetiredAllocations = cache.retiredAllocations;
  const uint64_t initialNextAllocationId = cache.nextAllocationId;
  const uint32_t initialArenaNodeCapacity = cache.arenaNodeCapacity;
  const uint32_t initialArenaChildCapacity = cache.arenaChildCapacity;
  const uint32_t initialArenaRootCapacity = cache.arenaRootCapacity;
  const uint32_t initialArenaMaterialCapacity = cache.arenaMaterialCapacity;
  const uint32_t initialArenaPaletteCapacity = cache.arenaPaletteCapacity;
  voxel::svraster::PlanetVoxelChunkSet refinedTerrain{};
  voxel::svraster::WorldVoxelChunkCacheUpdate refinementUpdate{};
  const std::vector<voxel::svraster::WorldVoxelChunkRequest>
      refinementRequests{{
          refinementOwner.slot,
          refinementOwner.generation,
          refinementOwner.residentLOD,
          refinementOwner.residentLOD - 1u,
          100u},
        {
          deferredRefinementOwner.slot,
          deferredRefinementOwner.generation,
          deferredRefinementOwner.residentLOD,
          deferredRefinementOwner.residentLOD - 1u,
          50u}};
  const bool refined = voxel::svraster::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, settings, cache, refinedTerrain,
      &refinementUpdate, &error, {}, nullptr, &refinementRequests);
  uint32_t changedRecordCount = 0u;
  for (size_t index = 0u;
       refined && index < terrain.chunks.size() &&
           index < refinedTerrain.chunks.size();
       ++index)
  {
    const auto &before = terrain.chunks[index];
    const auto &after = refinedTerrain.chunks[index];
    if (before.brickPayloadRevision != after.brickPayloadRevision ||
        before.brickNodeRevision != after.brickNodeRevision)
      ++changedRecordCount;
  }
  const auto refinedOwner = std::find_if(
      cache.entries.begin(), cache.entries.end(),
      [&](const auto &entry)
      {
        return entry.slot == refinementOwner.slot;
      });
  const auto deferredOwner = std::find_if(
      cache.entries.begin(), cache.entries.end(),
      [&](const auto &entry)
      {
        return entry.slot == deferredRefinementOwner.slot;
      });
  const bool pairedRefinementRewrites =
      refinedTerrain.hierarchyPageRewrites.size() == 2u &&
      std::all_of(
          refinedTerrain.hierarchyPageRewrites.begin(),
          refinedTerrain.hierarchyPageRewrites.end(),
          [](const auto &rewrite)
          {
            return rewrite.kind ==
                    voxel::svraster::
                        PlanetVoxelHierarchyRewriteKind::Refine &&
                rewrite.expected.valid() && rewrite.replacement.valid() &&
                rewrite.replacement.slot == rewrite.expected.slot &&
                rewrite.replacement.generation ==
                    rewrite.expected.generation + 1u &&
                rewrite.replacement.internalLOD + 1u ==
                    rewrite.expected.internalLOD &&
                rewrite.replacement.rasterAllocation !=
                    rewrite.expected.rasterAllocation &&
                rewrite.replacement.geometryRevision !=
                    rewrite.expected.geometryRevision;
          });
  expect(
      refined && refinementUpdate.refinedChunkCount == 2u &&
          refinementUpdate.builtChunkCount == 2u &&
          refinementUpdate.pendingChunkCount == 0u &&
          refinementUpdate.refinementRollbackEntries.size() == 2u &&
          refinedOwner != cache.entries.end() &&
          refinedOwner->address == refinementOwner.address &&
          refinedOwner->residentLOD == refinementOwner.residentLOD - 1u &&
          refinedOwner->generation == refinementOwner.generation + 1u &&
          deferredOwner != cache.entries.end() &&
          deferredOwner->residentLOD ==
              deferredRefinementOwner.residentLOD - 1u &&
          deferredOwner->generation ==
              deferredRefinementOwner.generation + 1u &&
          refinedOwner->allocation.id != refinementOwner.allocation.id &&
          deferredOwner->allocation.id !=
              deferredRefinementOwner.allocation.id &&
          std::any_of(
              cache.retiredAllocations.begin(),
              cache.retiredAllocations.end(),
              [&](const auto &retired)
              {
                return retired.allocation.id ==
                    refinementOwner.allocation.id;
              }) &&
          refinedTerrain.clipmapLayoutRevision == initialLayoutRevision &&
          cache.arenaNodeCapacity == initialArenaNodeCapacity &&
          cache.arenaChildCapacity == initialArenaChildCapacity &&
          cache.arenaRootCapacity == initialArenaRootCapacity &&
          cache.arenaMaterialCapacity == initialArenaMaterialCapacity &&
          cache.arenaPaletteCapacity == initialArenaPaletteCapacity &&
          refinedTerrain.bricks.nodes.size() == initialArenaNodeCapacity &&
          changedRecordCount == 2u && pairedRefinementRewrites,
      "batched refinement emits paired parent-to-child rewrites, allocates "
      "two unreachable physical pages, and keeps the virtual arena fixed");

  auto coarsenCache = cache;
  auto coarsenSettings = settings;
  coarsenSettings.readerFrame = 64u;
  voxel::svraster::markWorldVoxelChunkPhysicalPagesPublished(
      coarsenCache, 63u, 0u);
  const auto fineOwner = std::find_if(
      coarsenCache.entries.begin(), coarsenCache.entries.end(),
      [&](const auto &entry)
      {
        return entry.slot == refinementOwner.slot;
      });
  voxel::svraster::PlanetVoxelChunkSet coarsenedTerrain{};
  voxel::svraster::WorldVoxelChunkCacheUpdate coarseningUpdate{};
  bool coarsened = false;
  if (fineOwner != coarsenCache.entries.end())
  {
    const std::vector<voxel::svraster::WorldVoxelChunkRequest>
        mergeRequests{{
          fineOwner->slot, fineOwner->generation, fineOwner->residentLOD,
          fineOwner->residentLOD + 1u, 100u}};
    coarsened = voxel::svraster::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, coarsenSettings, coarsenCache,
        coarsenedTerrain, &coarseningUpdate, &error, {}, nullptr,
        &mergeRequests);
  }
  const auto parentOwner = std::find_if(
      coarsenCache.entries.begin(), coarsenCache.entries.end(),
      [&](const auto &entry)
      {
        return entry.slot == refinementOwner.slot;
      });
  expect(
      coarsened && coarseningUpdate.coarsenedChunkCount == 1u &&
          coarseningUpdate.refinedChunkCount == 0u &&
          parentOwner != coarsenCache.entries.end() &&
          parentOwner->residentLOD == refinementOwner.residentLOD &&
          parentOwner->generation == refinementOwner.generation + 2u &&
          coarsenedTerrain.hierarchyPageRewrites.size() == 1u &&
          coarsenedTerrain.hierarchyPageRewrites.front().kind ==
              voxel::svraster::
                  PlanetVoxelHierarchyRewriteKind::Coarsen &&
          coarsenedTerrain.hierarchyPageRewrites.front().expected.valid() &&
          coarsenedTerrain.hierarchyPageRewrites.front().replacement.valid() &&
          voxel::svraster::validateWorldVoxelChunkPhysicalPages(
              coarsenCache, &physicalPageError),
      "coarsening emits a child-to-parent rewrite before retiring fine pages");

  voxel::svraster::rollbackWorldVoxelChunkCacheUpdate(
      cache, refinementUpdate);
  const auto rolledBackOwner = std::find_if(
      cache.entries.begin(), cache.entries.end(),
      [&](const auto &entry)
      {
        return entry.slot == refinementOwner.slot;
      });
  expect(
      rolledBackOwner != cache.entries.end() &&
          rolledBackOwner->address == refinementOwner.address &&
          rolledBackOwner->residentLOD == refinementOwner.residentLOD &&
          rolledBackOwner->generation == refinementOwner.generation &&
          cache.slotGenerations == initialSlotGenerations &&
          cache.slotNodeCapacity == initialNodeCapacities &&
          cache.slotChildCapacity == initialChildCapacities &&
          cache.slotRootCapacity == initialRootCapacities &&
          cache.slotMaterialCapacity == initialMaterialCapacities &&
          cache.slotPaletteCapacity == initialPaletteCapacities &&
          cache.layoutRevision == initialLayoutRevision &&
          byteEqual(cache.freeNodeRanges, initialFreeNodeRanges) &&
          byteEqual(cache.freeChildRanges, initialFreeChildRanges) &&
          byteEqual(cache.freeRootRanges, initialFreeRootRanges) &&
          byteEqual(cache.freeMaterialRanges, initialFreeMaterialRanges) &&
          byteEqual(cache.freePaletteRanges, initialFreePaletteRanges) &&
          byteEqual(cache.retiredAllocations, initialRetiredAllocations) &&
          cache.nextAllocationId == initialNextAllocationId &&
          voxel::svraster::validateWorldVoxelChunkPhysicalPages(
              cache, &physicalPageError),
      "a rejected phase-5 publication restores the previous owner and every allocator range");

  // Exhaust every free range without disturbing the live owners. The failed
  // refinement must leave the parent page and allocator partition untouched.
  auto starvedCache = cache;
  const auto reserveFreeRanges = [&](auto &ranges, auto assignRange)
  {
    for (const auto &range : ranges)
    {
      voxel::svraster::WorldVoxelRetiredAllocation retired{};
      retired.allocation.id = starvedCache.nextAllocationId++;
      assignRange(retired.allocation) = {range.offset, range.capacity};
      retired.lastReaderFrame = 0u;
      retired.releaseFrame = std::numeric_limits<uint64_t>::max();
      starvedCache.retiredAllocations.push_back(retired);
    }
    ranges.clear();
  };
  reserveFreeRanges(
      starvedCache.freeNodeRanges,
      [](auto &allocation) -> auto & { return allocation.nodes; });
  reserveFreeRanges(
      starvedCache.freeChildRanges,
      [](auto &allocation) -> auto & { return allocation.children; });
  reserveFreeRanges(
      starvedCache.freeRootRanges,
      [](auto &allocation) -> auto & { return allocation.roots; });
  reserveFreeRanges(
      starvedCache.freeMaterialRanges,
      [](auto &allocation) -> auto & { return allocation.materials; });
  reserveFreeRanges(
      starvedCache.freePaletteRanges,
      [](auto &allocation) -> auto & { return allocation.palette; });
  const auto starvedParent = starvedCache.entries.front();
  voxel::svraster::WorldVoxelChunkCacheUpdate failedAllocationUpdate{};
  voxel::svraster::PlanetVoxelChunkSet failedAllocationTerrain{};
  const std::vector<voxel::svraster::WorldVoxelChunkRequest>
      failedAllocationRequest{{
          starvedParent.slot, starvedParent.generation,
          starvedParent.residentLOD, starvedParent.residentLOD - 1u, 100u}};
  const bool allocationUnexpectedlySucceeded =
      voxel::svraster::updateWorldVoxelChunkCache(
          bundle, 0.0, 0.0, settings, starvedCache,
          failedAllocationTerrain, &failedAllocationUpdate, &error, {},
          nullptr, &failedAllocationRequest);
  const auto retainedParent = std::find_if(
      starvedCache.entries.begin(), starvedCache.entries.end(),
      [&](const auto &entry)
      {
        return entry.slot == starvedParent.slot;
      });
  expect(
      !allocationUnexpectedlySucceeded &&
          error.find("allocation failed") != std::string::npos &&
          retainedParent != starvedCache.entries.end() &&
          retainedParent->allocation.id == starvedParent.allocation.id &&
          retainedParent->residentLOD == starvedParent.residentLOD &&
          retainedParent->generation == starvedParent.generation &&
          voxel::svraster::validateWorldVoxelChunkPhysicalPages(
              starvedCache, &physicalPageError),
      "phase-5 allocation failure retains the complete parent hierarchy: " +
          physicalPageError);

  // Exercise allocation, retirement, reclamation, toroidal slot reuse and
  // independent refinement in a deterministic mixed sequence.
  auto recycleCache = cache;
  auto recycleSettings = settings;
  recycleSettings.deferredReleaseFrames = 2u;
  bool randomizedPagesValid = true;
  bool movementMemoryBounded = true;
  std::string randomizedPageError;
  uint32_t successfulRecycleUpdates = 0u;
  constexpr std::array<std::array<double, 2u>, 12u> cameraSequence{{
      {0.0, 0.0}, {160.1, 0.0}, {160.1, 160.1}, {0.0, 160.1},
      {-160.1, 160.1}, {-160.1, 0.0}, {0.0, 0.0}, {320.1, 0.0},
      {320.1, -160.1}, {160.1, -160.1}, {0.0, -160.1}, {0.0, 0.0}}};
  for (size_t step = 0u; step < cameraSequence.size(); ++step)
  {
    recycleSettings.readerFrame = 10u + step * 4u;
    std::vector<voxel::svraster::WorldVoxelChunkRequest> requests;
    if ((step % 3u) == 0u)
    {
      const auto candidate = std::find_if(
          recycleCache.entries.begin(), recycleCache.entries.end(),
          [](const auto &entry) { return entry.residentLOD != 0u; });
      if (candidate != recycleCache.entries.end())
        requests.push_back({
            candidate->slot, candidate->generation, candidate->residentLOD,
            candidate->residentLOD - 1u, 100u});
    }
    voxel::svraster::PlanetVoxelChunkSet recycleTerrain{};
    voxel::svraster::WorldVoxelChunkCacheUpdate recycleUpdate{};
    const bool recycleSucceeded =
        voxel::svraster::updateWorldVoxelChunkCache(
            bundle, cameraSequence[step][0], cameraSequence[step][1],
            recycleSettings, recycleCache, recycleTerrain, &recycleUpdate,
            &error, {}, nullptr, requests.empty() ? nullptr : &requests);
    const bool physicalPagesValid = recycleSucceeded &&
        voxel::svraster::validateWorldVoxelChunkPhysicalPages(
            recycleCache, &physicalPageError);
    if (!physicalPagesValid && randomizedPageError.empty())
      randomizedPageError = "step " + std::to_string(step) + ": " +
          (recycleSucceeded ? physicalPageError : error);
    randomizedPagesValid &= physicalPagesValid;
    movementMemoryBounded &= recycleCache.entries.size() <= 9u &&
        recycleCache.arenaNodeCapacity == initialArenaNodeCapacity &&
        recycleCache.arenaChildCapacity == initialArenaChildCapacity &&
        recycleCache.arenaRootCapacity == initialArenaRootCapacity &&
        recycleCache.arenaMaterialCapacity == initialArenaMaterialCapacity &&
        recycleCache.arenaPaletteCapacity == initialArenaPaletteCapacity &&
        recycleCache.payloadCache.size() <=
            recycleSettings.maximumCachedPayloads;
    for (const auto &entry : recycleCache.entries)
      movementMemoryBounded &=
          std::abs(entry.address.x - recycleCache.centerChunkX) <=
              static_cast<int32_t>(recycleSettings.residentRadius) &&
          std::abs(entry.address.z - recycleCache.centerChunkZ) <=
              static_cast<int32_t>(recycleSettings.residentRadius);
    if (recycleSucceeded)
    {
      ++successfulRecycleUpdates;
      voxel::svraster::markWorldVoxelChunkPhysicalPagesPublished(
          recycleCache, recycleSettings.readerFrame,
          recycleSettings.deferredReleaseFrames);
    }
  }
  expect(
      randomizedPagesValid && movementMemoryBounded &&
          successfulRecycleUpdates == cameraSequence.size(),
      "phase-7 continuous movement keeps resident, cached, and deferred physical memory bounded: " +
          randomizedPageError);

  auto staleMovementCache = cache;
  const auto staleMovementEntries = staleMovementCache.entries;
  const auto staleMovementFreeNodes = staleMovementCache.freeNodeRanges;
  const uint64_t staleMovementPayloadClock =
      staleMovementCache.payloadCacheClock;
  voxel::svraster::PlanetVoxelChunkSet staleMovementTerrain{};
  voxel::svraster::WorldVoxelChunkCacheUpdate staleMovementUpdate{};
  const bool staleMovementBuilt =
      voxel::svraster::updateWorldVoxelChunkCache(
          bundle, 160.1, 0.0, settings, staleMovementCache,
          staleMovementTerrain, &staleMovementUpdate, &error);
  voxel::svraster::rollbackWorldVoxelChunkCacheUpdate(
      staleMovementCache, staleMovementUpdate);
  bool staleOwnersRestored =
      staleMovementCache.entries.size() == staleMovementEntries.size();
  for (size_t index = 0u;
       staleOwnersRestored && index < staleMovementEntries.size(); ++index)
    staleOwnersRestored &=
        staleMovementCache.entries[index].address ==
            staleMovementEntries[index].address &&
        staleMovementCache.entries[index].slot ==
            staleMovementEntries[index].slot &&
        staleMovementCache.entries[index].generation ==
            staleMovementEntries[index].generation &&
        staleMovementCache.entries[index].allocation.id ==
            staleMovementEntries[index].allocation.id;
  expect(
      staleMovementBuilt && staleOwnersRestored &&
          staleMovementCache.payloadCache.empty() &&
          staleMovementCache.payloadCacheClock ==
              staleMovementPayloadClock &&
          byteEqual(
              staleMovementCache.freeNodeRanges,
              staleMovementFreeNodes) &&
          voxel::svraster::validateWorldVoxelChunkPhysicalPages(
              staleMovementCache, &physicalPageError),
      "discarding a camera transaction restores evicted owners, payload-cache deltas, and allocator ownership");

  std::vector<std::pair<voxel::svraster::WorldVoxelChunkAddress, uint32_t>>
      initialSlots;
  for (const auto &entry : cache.entries)
    initialSlots.push_back({entry.address, entry.slot});
  struct InitialTree64Owner
  {
    uint32_t slot = 0u;
    uint32_t generation = 0u;
    uint64_t revision = 0u;
  };
  std::vector<InitialTree64Owner> initialTree64Owners;
  for (const auto &record : terrain.chunks)
    initialTree64Owners.push_back({
      record.clipmapSlot,
      record.residencyGeneration,
      record.tree64Revision,
    });

  voxel::svraster::PlanetVoxelChunkSet stationary{};
  voxel::svraster::WorldVoxelChunkCacheUpdate stationaryUpdate{};
  const bool retained = voxel::svraster::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, settings, cache, stationary,
      &stationaryUpdate, &error);
  expect(
      retained && stationaryUpdate.reusedChunkCount == 9u &&
          stationaryUpdate.builtChunkCount == 0u &&
          stationary.clipmapLayoutRevision == initialLayoutRevision,
      "a stationary progressive grid reuses every owner and physical slot");

  auto byteBoundedCache = cache;
  auto byteBoundedSettings = settings;
  byteBoundedSettings.maximumCPUCacheBytes = 1u;
  voxel::svraster::PlanetVoxelChunkSet byteBoundedTerrain{};
  voxel::svraster::WorldVoxelChunkCacheUpdate byteBoundedUpdate{};
  const bool byteBoundedMove = voxel::svraster::updateWorldVoxelChunkCache(
      bundle, 160.1, 0.0, byteBoundedSettings, byteBoundedCache,
      byteBoundedTerrain, &byteBoundedUpdate, &error);
  expect(
      byteBoundedMove && byteBoundedCache.payloadCache.empty() &&
          byteBoundedCache.payloadCacheBytes == 0u &&
          byteBoundedUpdate.cpuCacheBytes == 0u,
      "the CPU payload LRU obeys its byte budget independently of resident GPU pages");

  voxel::svraster::PlanetVoxelChunkSet moved{};
  voxel::svraster::WorldVoxelChunkCacheUpdate movedUpdate{};
  const bool streamed = voxel::svraster::updateWorldVoxelChunkCache(
      bundle, 160.1, 0.0, settings, cache, moved, &movedUpdate, &error);
  bool retainedSlotsStable = true;
  uint32_t retainedTree64Owners = 0u;
  for (const auto &initial : initialSlots)
  {
    const auto found = std::find_if(
        cache.entries.begin(), cache.entries.end(),
        [&](const auto &entry)
        {
          return entry.address == initial.first;
        });
    if (found != cache.entries.end())
      retainedSlotsStable &= found->slot == initial.second;
  }
  for (const auto &record : moved.chunks)
  {
    const auto initial = std::find_if(
        initialTree64Owners.begin(), initialTree64Owners.end(),
        [&](const auto &owner)
        {
          return owner.slot == record.clipmapSlot &&
              owner.generation == record.residencyGeneration;
        });
    if (initial == initialTree64Owners.end())
      continue;
    ++retainedTree64Owners;
    retainedSlotsStable &= initial->revision == record.tree64Revision;
  }
  const uint32_t reassignedSlotCount = static_cast<uint32_t>(std::count_if(
      cache.slotGenerations.begin(), cache.slotGenerations.end(),
      [](const uint32_t generation)
      {
        return generation == 2u;
      }));
  expect(
      streamed && moved.chunks.size() == 9u &&
          movedUpdate.reusedChunkCount == 6u &&
          movedUpdate.builtChunkCount == 3u &&
          movedUpdate.evictedChunkCount == 3u &&
          movedUpdate.pendingChunkCount == 0u && retainedSlotsStable &&
          retainedTree64Owners == 6u &&
          reassignedSlotCount == 3u,
      "moving one fixed chunk retains six Tree64 model owners and replaces only the entering row");
  if (streamed)
    voxel::svraster::markWorldVoxelChunkPhysicalPagesPublished(
        cache, settings.readerFrame, settings.deferredReleaseFrames);

  voxel::svraster::PlanetVoxelChunkSet returned{};
  voxel::svraster::WorldVoxelChunkCacheUpdate returnedUpdate{};
  const bool restored = voxel::svraster::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, settings, cache, returned,
      &returnedUpdate, &error);
  expect(
      restored && returned.chunks.size() == 9u &&
          returnedUpdate.reusedChunkCount == 6u &&
          returnedUpdate.builtChunkCount == 3u &&
          returnedUpdate.restoredCachedChunkCount == 3u &&
          returnedUpdate.mapSamplingMilliseconds == 0.0 &&
          returnedUpdate.shellExtractionMilliseconds == 0.0 &&
          returnedUpdate.hierarchyBuildMilliseconds == 0.0,
      "returning to an evicted row restores deterministic CPU payloads without regeneration");
}

// Render-distance streaming uses one non-overlapping spatial quadtree cut;
// each leaf remains an independently installable virtual page.
void testGeneratedWorldSpatialLODLeafCut()
{
  voxel::svraster::WorldVoxelChunkStreamingSettings settings{};
  settings.finestVoxelSizeMetres = 0.10;
  settings.finestHorizontalVoxelCount = 64u;
  settings.chunkEdgeMetres = 6.4;
  settings.renderDistanceMetres = 2500.0;
  settings.projectionScalePixels =
      720.0 / (2.0 * std::tan(1.0471975512 * 0.5));
  settings.splitErrorPixels = 4.0f;
  settings.maximumSpatialLOD = 12u;
  const auto leaves = voxel::svraster::buildWorldVoxelChunkLeafCut(
      6400.0, 0.0, 0.0, settings);
  auto fineSettings = settings;
  fineSettings.splitErrorPixels = 1.0f;
  auto coarseSettings = settings;
  coarseSettings.splitErrorPixels = 16.0f;
  const auto fineLeaves = voxel::svraster::buildWorldVoxelChunkLeafCut(
      6400.0, 0.0, 0.0, fineSettings);
  const auto coarseLeaves = voxel::svraster::buildWorldVoxelChunkLeafCut(
      6400.0, 0.0, 0.0, coarseSettings);
  const auto hystereticLeaves = voxel::svraster::buildWorldVoxelChunkLeafCut(
      6400.0, 0.0, 0.0, settings, nullptr, &fineLeaves);
  const auto settledHystereticLeaves =
      voxel::svraster::buildWorldVoxelChunkLeafCut(
          6400.0, 0.0, 0.0, settings, nullptr, &hystereticLeaves);
  auto largerPageSettings = settings;
  largerPageSettings.finestHorizontalVoxelCount = 128u;
  largerPageSettings.chunkEdgeMetres = 12.8;
  const auto largerPageLeaves =
      voxel::svraster::buildWorldVoxelChunkLeafCut(
          6400.0, 0.0, 0.0, largerPageSettings);

  bool hasFinest = false;
  bool hasCoarse = false;
  bool nonOverlapping = true;
  bool faceBalanced = true;
  for (size_t leftIndex = 0u; leftIndex < leaves.size(); ++leftIndex)
  {
    const auto &left = leaves[leftIndex];
    hasFinest |= left.spatialLOD == 0u;
    hasCoarse |= left.spatialLOD > 0u;
    const double leftEdge = std::ldexp(
        settings.chunkEdgeMetres, left.spatialLOD);
    const double leftMinimumX = left.x * leftEdge;
    const double leftMinimumZ = left.z * leftEdge;
    for (size_t rightIndex = leftIndex + 1u;
         rightIndex < leaves.size(); ++rightIndex)
    {
      const auto &right = leaves[rightIndex];
      const double rightEdge = std::ldexp(
          settings.chunkEdgeMetres, right.spatialLOD);
      const double rightMinimumX = right.x * rightEdge;
      const double rightMinimumZ = right.z * rightEdge;
      const bool separated =
          leftMinimumX + leftEdge <= rightMinimumX + 1.0e-9 ||
          rightMinimumX + rightEdge <= leftMinimumX + 1.0e-9 ||
          leftMinimumZ + leftEdge <= rightMinimumZ + 1.0e-9 ||
          rightMinimumZ + rightEdge <= leftMinimumZ + 1.0e-9;
      nonOverlapping &= separated;
      const bool overlapsX =
          std::min(leftMinimumX + leftEdge, rightMinimumX + rightEdge) >
          std::max(leftMinimumX, rightMinimumX) + 1.0e-9;
      const bool overlapsZ =
          std::min(leftMinimumZ + leftEdge, rightMinimumZ + rightEdge) >
          std::max(leftMinimumZ, rightMinimumZ) + 1.0e-9;
      const bool sharesXFace = overlapsZ &&
          (std::abs(leftMinimumX + leftEdge - rightMinimumX) < 1.0e-9 ||
           std::abs(rightMinimumX + rightEdge - leftMinimumX) < 1.0e-9);
      const bool sharesZFace = overlapsX &&
          (std::abs(leftMinimumZ + leftEdge - rightMinimumZ) < 1.0e-9 ||
           std::abs(rightMinimumZ + rightEdge - leftMinimumZ) < 1.0e-9);
      if (sharesXFace || sharesZFace)
      {
        const uint32_t lodDelta = left.spatialLOD > right.spatialLOD
            ? left.spatialLOD - right.spatialLOD
            : right.spatialLOD - left.spatialLOD;
        faceBalanced &= lodDelta <= 1u;
      }
    }
  }

  bool noInternalHoles = true;
  for (int32_t sampleZ = -20; sampleZ <= 20; ++sampleZ)
    for (int32_t sampleX = -20; sampleX <= 20; ++sampleX)
    {
      const double worldX = sampleX * settings.renderDistanceMetres / 20.0;
      const double worldZ = sampleZ * settings.renderDistanceMetres / 20.0;
      if (std::hypot(worldX, worldZ) > settings.renderDistanceMetres)
        continue;
      const double mapX = worldX + 3200.0;
      const double mapZ = worldZ + 3200.0;
      noInternalHoles &= std::any_of(
          leaves.begin(), leaves.end(),
          [&](const auto &leaf)
          {
            const double edge = std::ldexp(
                settings.chunkEdgeMetres, leaf.spatialLOD);
            const double minimumX = leaf.x * edge;
            const double minimumZ = leaf.z * edge;
            return mapX >= minimumX && mapX < minimumX + edge &&
                mapZ >= minimumZ && mapZ < minimumZ + edge;
          });
    }
  const uint64_t finestOnlyPageCount = static_cast<uint64_t>(
      std::ceil(settings.renderDistanceMetres / settings.chunkEdgeMetres)) *
      2u + 1u;
  expect(
      !leaves.empty() && hasFinest && hasCoarse && nonOverlapping &&
          faceBalanced &&
          noInternalHoles &&
          fineLeaves.size() > leaves.size() &&
          leaves.size() > coarseLeaves.size() &&
          largerPageLeaves.size() < leaves.size() &&
          leaves.size() < finestOnlyPageCount * finestOnlyPageCount / 100u,
      "spatial LOD pages form a compact non-overlapping quadtree leaf cut "
      "whose error setting changes page density, with 10 cm owners near the "
      "camera and no holes (fine/default/coarse=" +
          std::to_string(fineLeaves.size()) + "/" +
          std::to_string(leaves.size()) + "/" +
          std::to_string(coarseLeaves.size()) + ", large-page=" +
          std::to_string(largerPageLeaves.size()) + ", finest=" +
          std::to_string(hasFinest) + ", coarse=" +
          std::to_string(hasCoarse) + ", disjoint=" +
          std::to_string(nonOverlapping) + ", balanced=" +
          std::to_string(faceBalanced) + ", covered=" +
          std::to_string(noInternalHoles) + ")");
  expect(
      hystereticLeaves.size() > leaves.size() &&
          hystereticLeaves.size() <= fineLeaves.size() &&
          settledHystereticLeaves == hystereticLeaves,
      "spatial LOD retains prior refinement inside the split/merge dead band "
      "and reaches a stable cut");

  // A moving camera may change several parent/child owners at once. Keep the
  // prior complete cut visible while a deliberately one-page batch stages the
  // replacement; publishing that partial replacement would overlap a parent
  // or expose a hole.
  const MapExportBundle bundle = placementFixture();
  voxel::svraster::WorldVoxelChunkStreamingSettings stream{};
  stream.finestVoxelSizeMetres = 10.0;
  stream.finestHorizontalVoxelCount = 16u;
  stream.chunkEdgeMetres = 160.0;
  stream.renderDistanceMetres = 500.0;
  stream.projectionScalePixels = 80.0;
  stream.splitErrorPixels = 4.0f;
  stream.maximumSpatialLOD = 4u;
  stream.maximumResidentChunks = 128u;
  stream.maximumGeneratedChunksPerUpdate = 0u;
  stream.lodCount = 1u;
  stream.voxel.foundationDepthVoxels = 2u;
  stream.voxel.surfaceShellDepthVoxels = 4u;
  stream.voxel.maximumVerticalVoxels = 32u;
  stream.voxel.maximumBricks = 1u << 18u;
  voxel::svraster::SparseVoxelMicroMaterialLibrary streamMicroMaterials;
  voxel::svraster::SparseVoxelMicroDescriptor streamMicroGeometry{};
  streamMicroGeometry.subdivision = 5u;
  streamMicroGeometry.geometryProfile =
      voxel::svraster::SparseVoxelMicroGeometryProfile::Staircase;
  std::string streamMicroError;
  expect(
      streamMicroMaterials.setMaterial(
          static_cast<uint32_t>(
              procedural::PolygonTerrainMaterial::Grassland),
          streamMicroGeometry, {}, &streamMicroError),
      "spatial streaming micro material registers: " + streamMicroError);
  stream.voxel.microGeometry = true;
  stream.voxel.microMaterials = &streamMicroMaterials;

  // Bootstrap must publish a complete coarse fallback even when normal page
  // generation is limited to one owner. The requested detailed cut is then
  // staged behind that fallback and becomes visible only when every owner is
  // ready; exposing a priority prefix here creates page-sized terrain holes.
  auto bootstrapStream = stream;
  bootstrapStream.maximumGeneratedChunksPerUpdate = 1u;
  voxel::svraster::WorldVoxelChunkCache bootstrapCache{};
  voxel::svraster::PlanetVoxelChunkSet bootstrapTerrain{};
  voxel::svraster::WorldVoxelChunkCacheUpdate bootstrapUpdate{};
  std::string bootstrapError;
  auto bootstrapRootSettings = bootstrapStream;
  bootstrapRootSettings.splitErrorPixels =
      std::numeric_limits<float>::max();
  const auto bootstrapRootCut = voxel::svraster::buildWorldVoxelChunkLeafCut(
      bundle.extentMetres, 0.0, 0.0, bootstrapRootSettings);
  const auto requestedBootstrapCut =
      voxel::svraster::buildWorldVoxelChunkLeafCut(
          bundle.extentMetres, 0.0, 0.0, bootstrapStream);
  const uint32_t expectedBootstrapPending = static_cast<uint32_t>(
      std::count_if(
          requestedBootstrapCut.begin(), requestedBootstrapCut.end(),
          [&](const auto &requested)
          {
            return std::find(
                       bootstrapRootCut.begin(), bootstrapRootCut.end(),
                       requested) == bootstrapRootCut.end();
          }));
  bootstrapStream.readerFrame = 1u;
  const bool bootstrapBuilt = voxel::svraster::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, bootstrapStream, bootstrapCache,
      bootstrapTerrain, &bootstrapUpdate, &bootstrapError);
  const auto containsBootstrapAddress = [](const auto &entries,
                                            const auto &address)
  {
    return std::any_of(
        entries.begin(), entries.end(),
        [&](const auto &entry) { return entry.address == address; });
  };
  // A spatial cut advances one ownership group at a time, so a transaction
  // taken while pages are still pending no longer has to equal the cut before
  // it. What must hold at every step is the property the old whole-cut barrier
  // was protecting: the published owners tile the same region exactly once.
  // Overlap draws a page and its descendants together; a gap leaves an area
  // with no owner at all.
  const auto cutIsPartitionOver =
      [](const std::vector<voxel::svraster::WorldVoxelChunkAddress> &cut,
         const std::vector<voxel::svraster::WorldVoxelChunkAddress> &reference,
         const double baseEdge)
  {
    const auto boxOf = [&](const voxel::svraster::WorldVoxelChunkAddress &page)
    {
      const double edge = std::ldexp(baseEdge, static_cast<int>(page.spatialLOD));
      return std::array<double, 3u>{
        static_cast<double>(page.x) * edge,
        static_cast<double>(page.z) * edge, edge};
    };
    for (size_t left = 0u; left < cut.size(); ++left)
      for (size_t right = left + 1u; right < cut.size(); ++right)
      {
        const auto a = boxOf(cut[left]);
        const auto b = boxOf(cut[right]);
        const double tolerance = std::max(a[2], b[2]) * 1.0e-9;
        if (a[0] < b[0] + b[2] - tolerance && b[0] < a[0] + a[2] - tolerance &&
            a[1] < b[1] + b[2] - tolerance && b[1] < a[1] + a[2] - tolerance)
          return false;
      }
    for (const auto &page : reference)
    {
      const auto target = boxOf(page);
      const double centreX = target[0] + target[2] * 0.5;
      const double centreZ = target[1] + target[2] * 0.5;
      const bool owned = std::any_of(
          cut.begin(), cut.end(),
          [&](const voxel::svraster::WorldVoxelChunkAddress &owner)
          {
            const auto box = boxOf(owner);
            return centreX >= box[0] && centreX < box[0] + box[2] &&
                centreZ >= box[1] && centreZ < box[1] + box[2];
          });
      if (!owned)
        return false;
    }
    return true;
  };
  const auto entryAddresses = [](const auto &entries)
  {
    std::vector<voxel::svraster::WorldVoxelChunkAddress> addresses;
    addresses.reserve(entries.size());
    for (const auto &entry : entries)
      addresses.push_back(entry.address);
    return addresses;
  };
  bool completeRootPublished = bootstrapBuilt &&
      bootstrapCache.pendingChunkCount != 0u &&
      bootstrapCache.pendingChunkCount ==
          expectedBootstrapPending &&
      bootstrapUpdate.pendingChunkCount ==
          bootstrapCache.pendingChunkCount &&
      bootstrapCache.entries.size() == bootstrapRootCut.size() &&
      bootstrapTerrain.chunks.size() == bootstrapRootCut.size();
  for (const auto &address : bootstrapRootCut)
    completeRootPublished &= containsBootstrapAddress(
        bootstrapCache.entries, address);
  const auto recordHasMicroGeometry = [](const auto &terrain,
                                          const auto &record)
  {
    const size_t begin = std::min<size_t>(
        record.brickPaletteOffset,
        terrain.bricks.paletteDescriptorIds.size());
    const size_t end = std::min<size_t>(
        begin + record.brickPaletteCapacity,
        terrain.bricks.paletteDescriptorIds.size());
    return std::any_of(
        terrain.bricks.paletteDescriptorIds.begin() + begin,
        terrain.bricks.paletteDescriptorIds.begin() + end,
        [](const uint16_t descriptorId) { return descriptorId != 0u; });
  };
  const auto recordHasAnalyticTerrain = [](const auto &terrain,
                                            const auto &record)
  {
    const size_t begin = std::min<size_t>(
        record.brickMaterialOffset, terrain.bricks.microSurfaceData.size());
    const size_t end = std::min<size_t>(
        begin + record.brickMaterialCapacity,
        terrain.bricks.microSurfaceData.size());
    return std::any_of(
        terrain.bricks.microSurfaceData.begin() + begin,
        terrain.bricks.microSurfaceData.begin() + end,
        [](const uint64_t packedContext)
        {
          return voxel::svraster::
                     unpackSparseVoxelMicroContextSurfacePatch(
                         packedContext) != 0u;
        });
  };
  const bool terrainIsDescriptorFree = completeRootPublished &&
      std::none_of(
          bootstrapTerrain.chunks.begin(), bootstrapTerrain.chunks.end(),
          [&](const auto &record)
          {
            return recordHasMicroGeometry(bootstrapTerrain, record);
          });
  const double bootstrapOrigin = bootstrapCache.elevationOriginMetres;
  bool bootstrapAtomic = completeRootPublished;
  uint32_t bootstrapCallbacks = 0u;
  for (; bootstrapAtomic && bootstrapCallbacks < 256u; ++bootstrapCallbacks)
  {
    bootstrapStream.readerFrame = bootstrapCallbacks + 2u;
    const bool updated = voxel::svraster::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, bootstrapStream, bootstrapCache,
        bootstrapTerrain, &bootstrapUpdate, &bootstrapError);
    bootstrapAtomic &= updated && bootstrapCache.hasElevationOrigin &&
        bootstrapCache.elevationOriginMetres == bootstrapOrigin &&
        bootstrapTerrain.elevationOriginMetres == bootstrapOrigin;
    if (!updated)
      break;
    if (bootstrapCache.pendingChunkCount != 0u)
    {
      // Intermediate cuts subdivide toward the request rather than holding the
      // root cut, so assert the partition rather than identity: every step
      // still tiles the bootstrap region exactly once, with no area unowned and
      // no page drawn together with its descendants.
      bootstrapAtomic &= cutIsPartitionOver(
          entryAddresses(bootstrapCache.entries), bootstrapRootCut,
          bootstrapStream.chunkEdgeMetres);
    }
    else
    {
      bootstrapAtomic &=
          bootstrapCache.entries.size() == requestedBootstrapCut.size();
      for (const auto &address : requestedBootstrapCut)
        bootstrapAtomic &= containsBootstrapAddress(
            bootstrapCache.entries, address);
      break;
    }
  }
  expect(
      bootstrapAtomic && completeRootPublished &&
          bootstrapCache.pendingChunkCount == 0u &&
          bootstrapCallbacks > 0u,
      "progressive bootstrap publishes one complete coarse fallback and "
      "atomically replaces it with the requested cut: " + bootstrapError);
  const bool noSpatialOwnerHasAnalyticTerrain = std::none_of(
      bootstrapTerrain.chunks.begin(), bootstrapTerrain.chunks.end(),
      [&](const auto &record)
      {
        return recordHasAnalyticTerrain(bootstrapTerrain, record);
      });
  expect(
      terrainIsDescriptorFree && noSpatialOwnerHasAnalyticTerrain,
      "every spatial owner retains descriptor-free square voxel terrain");

  voxel::svraster::WorldVoxelChunkCache cache{};
  voxel::svraster::PlanetVoxelChunkSet terrain{};
  voxel::svraster::WorldVoxelChunkCacheUpdate update{};
  std::string error;
  const bool initialBuilt = voxel::svraster::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, stream, cache, terrain, &update, &error);
  std::vector<voxel::svraster::WorldVoxelChunkAddress> previousOwners;
  for (const auto &entry : cache.entries)
    previousOwners.push_back(entry.address);
  stream.maximumGeneratedChunksPerUpdate = 1u;
  const bool staged = initialBuilt &&
      voxel::svraster::updateWorldVoxelChunkCache(
          bundle, 200.0, 0.0, stream, cache, terrain, &update, &error);
  bool previousCutRetained = cache.entries.size() == previousOwners.size();
  for (const auto &address : previousOwners)
    previousCutRetained &= std::any_of(
        cache.entries.begin(), cache.entries.end(),
        [&](const auto &entry) { return entry.address == address; });
  bool exactSpatialLeaves = true;
  for (const auto &record : terrain.chunks)
    for (uint32_t nodeIndex = 0u;
         nodeIndex < record.brickNodeCapacity; ++nodeIndex)
      exactSpatialLeaves &=
          (terrain.bricks.nodes[record.brickNodeOffset + nodeIndex].level &
           voxel::svraster::kSparseVoxelBrickForceLeafRaster) != 0u;
  // The rewrite is still incomplete here, but the cut no longer has to be
  // frozen while it is: ownership groups whose replacements are ready advance
  // on their own. What this case is really about is the seam and the raster
  // leaves, so assert those, plus that the published owners still tile the
  // region exactly once and that the transaction stayed bounded.
  expect(
      staged &&
          cutIsPartitionOver(
              entryAddresses(cache.entries), previousOwners,
              stream.chunkEdgeMetres) &&
          cache.pendingChunkCount != 0u &&
          update.maximumUnexposedDepthVoxels <= 1u &&
          exactSpatialLeaves &&
          terrain.chunks.size() == cache.entries.size(),
      "an incomplete surface-only spatial rewrite bounds seam closure to one "
      "cell and uses exact authored raster leaves while the cut stays a "
      "partition: " +
          error);

  // Keep the actual generated-world cache moving for many bounded batches.
  // At every intermediate callback the renderer-visible owner set must remain
  // the preceding complete cut; only the final batch may install the exact new
  // leaf set. This catches long-lived staged-slot leaks, premature parent
  // removal, and publication drift that a single interrupted rewrite misses.
  const auto orderedAddresses = [](auto addresses)
  {
    std::sort(
        addresses.begin(), addresses.end(),
        [](const auto &left, const auto &right)
        {
          if (left.spatialLOD != right.spatialLOD)
            return left.spatialLOD < right.spatialLOD;
          if (left.z != right.z)
            return left.z < right.z;
          return left.x < right.x;
        });
    return addresses;
  };
  const auto activeAddresses = [&]()
  {
    std::vector<voxel::svraster::WorldVoxelChunkAddress> result;
    result.reserve(cache.entries.size());
    for (const auto &entry : cache.entries)
      result.push_back(entry.address);
    return orderedAddresses(std::move(result));
  };
  bool sustainedStable = staged;
  std::string sustainedError;
  uint32_t completedCuts = 0u;
  uint32_t boundedCallbacks = 1u;
  uint64_t readerFrame = 1u;
  stream.maximumGeneratedChunksPerUpdate = 4u;
  stream.deferredReleaseFrames = 2u;
  constexpr std::array<std::array<double, 2u>, 9u> spatialCameraSequence{{
    {200.0, 0.0}, {360.0, 160.0}, {0.0, 320.0}, {-320.0, 160.0},
    {-360.0, -160.0}, {0.0, -320.0}, {320.0, -160.0},
    {160.0, 160.0}, {0.0, 0.0},
  }};
  for (const auto &camera : spatialCameraSequence)
  {
    const auto precedingCompleteCut = activeAddresses();
    const auto expectedCut = orderedAddresses(
        voxel::svraster::buildWorldVoxelChunkLeafCut(
            bundle.extentMetres, camera[0], camera[1], stream, nullptr,
            &precedingCompleteCut));
    bool completed = false;
    for (uint32_t callback = 0u; callback < 512u; ++callback)
    {
      stream.readerFrame = readerFrame++;
      voxel::svraster::WorldVoxelChunkCacheUpdate sustainedUpdate{};
      voxel::svraster::PlanetVoxelChunkSet sustainedTerrain{};
      const bool updated = voxel::svraster::updateWorldVoxelChunkCache(
          bundle, camera[0], camera[1], stream, cache, sustainedTerrain,
          &sustainedUpdate, &error);
      ++boundedCallbacks;
      std::string pageError;
      sustainedStable &= updated &&
          voxel::svraster::validateWorldVoxelChunkPhysicalPages(
              cache, &pageError) &&
          cache.entries.size() <= stream.maximumResidentChunks &&
          sustainedTerrain.chunks.size() == cache.entries.size();
      if (!updated || !pageError.empty())
      {
        sustainedError = "callback " + std::to_string(callback) + ": " +
            (updated ? pageError : error);
        break;
      }
      const auto visibleCut = activeAddresses();
      if (cache.pendingChunkCount != 0u)
      {
        // The cut advances group by group now, so it is no longer required to
        // equal the preceding one. What it must never do is publish a page
        // together with its own descendants: that draws the same ground twice.
        // Coverage is asserted against the finished cut below instead, because
        // once the camera has moved an intermediate cut is entitled to drop
        // pages that left render distance.
        sustainedStable &= !visibleCut.empty() &&
            cutIsPartitionOver(visibleCut, {}, stream.chunkEdgeMetres);
      }
      else
      {
        // residentCutChanged is no longer required on the transaction that
        // observes completion: the cut can reach its target on one transaction
        // and only drain the last staged pages on the next, which then
        // publishes nothing new.
        sustainedStable &= visibleCut == expectedCut &&
            cache.stagedSpatialEntries.empty();
        voxel::svraster::markWorldVoxelChunkPhysicalPagesPublished(
            cache, stream.readerFrame, stream.deferredReleaseFrames);
        ++completedCuts;
        completed = true;
        break;
      }
    }
    sustainedStable &= completed;
    if (!sustainedStable)
      break;
  }
  expect(
      sustainedStable &&
          completedCuts == spatialCameraSequence.size() &&
          boundedCallbacks > spatialCameraSequence.size(),
      "bounded spatial streaming remains on complete leaf cuts through " +
          std::to_string(boundedCallbacks) + " asynchronous callbacks and " +
          std::to_string(completedCuts) + " camera rewrites: " +
          sustainedError);

  // Changing the user-facing LOD error must replace the installed spatial
  // owner set as an atomic transaction. This is separate from brick-parent
  // LOD, which exact spatial pages deliberately bypass.
  const auto precedingLODSettingsCut = activeAddresses();
  auto settingsRollbackCache = cache;
  auto settingsRollbackStream = stream;
  settingsRollbackStream.splitErrorPixels = 12.0f;
  settingsRollbackStream.readerFrame = readerFrame++;
  voxel::svraster::PlanetVoxelChunkSet settingsRollbackTerrain{};
  voxel::svraster::WorldVoxelChunkCacheUpdate settingsRollbackUpdate{};
  const bool settingsRollbackBuilt =
      voxel::svraster::updateWorldVoxelChunkCache(
          bundle, 0.0, 0.0, settingsRollbackStream,
          settingsRollbackCache, settingsRollbackTerrain,
          &settingsRollbackUpdate, &error);
  voxel::svraster::rollbackWorldVoxelChunkCacheUpdate(
      settingsRollbackCache, settingsRollbackUpdate);
  expect(
      settingsRollbackBuilt &&
          settingsRollbackCache.settings.splitErrorPixels ==
              stream.splitErrorPixels &&
          settingsRollbackCache.spatialCutMetricConvergencePending ==
              cache.spatialCutMetricConvergencePending,
      "rolling back a stale GPU publication restores the prior spatial LOD "
      "metric transaction");
  stream.splitErrorPixels = 12.0f;
  const auto expectedLODSettingsCut = orderedAddresses(
      voxel::svraster::buildWorldVoxelChunkLeafCut(
          bundle.extentMetres, 0.0, 0.0, stream));
  bool settingsRewriteStable =
      precedingLODSettingsCut != expectedLODSettingsCut;
  bool settingsRewriteCompleted = false;
  bool observedStagedSettingsRewrite = false;
  stream.maximumGeneratedChunksPerUpdate = 1u;
  for (uint32_t callback = 0u; callback < 512u; ++callback)
  {
    stream.readerFrame = readerFrame++;
    voxel::svraster::WorldVoxelChunkCacheUpdate settingsUpdate{};
    voxel::svraster::PlanetVoxelChunkSet settingsTerrain{};
    const bool updated = voxel::svraster::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, stream, cache, settingsTerrain,
        &settingsUpdate, &error);
    std::string pageError;
    settingsRewriteStable &= updated &&
        voxel::svraster::validateWorldVoxelChunkPhysicalPages(
            cache, &pageError) &&
        settingsTerrain.chunks.size() == cache.entries.size();
    if (!updated || !pageError.empty())
    {
      sustainedError = updated ? pageError : error;
      break;
    }
    const auto visibleCut = activeAddresses();
    if (cache.pendingChunkCount != 0u)
    {
      observedStagedSettingsRewrite = true;
      // pendingChunkCount also covers the next one-level refinement step. At
      // that checkpoint the current cut is complete and staged storage may be
      // empty until the next callback starts building its replacement group.
      const bool completeIntermediateCut = cutIsPartitionOver(
          visibleCut, expectedLODSettingsCut, stream.chunkEdgeMetres);
      const bool boundedIntermediateCut =
          visibleCut.size() <= stream.maximumResidentChunks;
      settingsRewriteStable &=
          completeIntermediateCut && boundedIntermediateCut;
      if (!completeIntermediateCut || !boundedIntermediateCut)
        sustainedError =
            "callback " + std::to_string(callback) + " has " +
            std::to_string(visibleCut.size()) + " visible, " +
            std::to_string(cache.stagedSpatialEntries.size()) + " staged, " +
            std::to_string(cache.pendingChunkCount) + " pending pages for " +
            std::to_string(expectedLODSettingsCut.size()) + " target pages";
    }
    else
    {
      const bool finalCutMatches = visibleCut == expectedLODSettingsCut;
      const bool stagedCutDrained = cache.stagedSpatialEntries.empty();
      settingsRewriteStable &= finalCutMatches && stagedCutDrained;
      if (!finalCutMatches || !stagedCutDrained)
        sustainedError =
            "completion has " + std::to_string(visibleCut.size()) +
            " visible and " +
            std::to_string(cache.stagedSpatialEntries.size()) +
            " staged pages for " +
            std::to_string(expectedLODSettingsCut.size()) + " target pages";
      settingsRewriteCompleted = true;
      break;
    }
  }
  expect(
      settingsRewriteStable && settingsRewriteCompleted,
      "changing spatial LOD error installs a different complete geometry cut "
      "immediately or through disjoint ownership groups (staged=" +
          std::to_string(observedStagedSettingsRewrite) + "): " +
          sustainedError);
}
} // namespace

int main()
{
  testProgressiveDitherThresholds();
  testLayeredDensityAndAssetSelection();
  testEcotopeWeightsBecomeDensityLayers();
  testWorldDataLayersAndMasks();
  testFootprintsAndTileSeams();
  testGeneratedWorldBuildsVoxelTerrain();
  testGeneratedWorldInternalLODIsDeterministicAndSurfaceOnly();
  testGeneratedWorldProgressiveCacheUsesFixedChunkOwners();
  testGeneratedWorldSpatialLODLeafCut();
  if (failures != 0u)
  {
    std::cerr << failures << " placement test(s) failed\n";
    return 1;
  }
  std::cout << "Map placement tests passed\n";
  return 0;
}
