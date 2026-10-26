#include "procedural/world/MapExport.hpp"
#include "voxel/svraster/TerrainGPUV2Columns.hpp"
#include "voxel/svraster/TerrainGPUV2WorldMapAtlas.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace world = procedural::world;
namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

std::vector<uint8_t> texel(const std::string &name)
{
  if (name == "refined_elevation")
    return {0u, 0u};
  if (name == "water_type" || name == "river")
    return name == "river" ? std::vector<uint8_t>{0u, 0u, 0u, 0u} : std::vector<uint8_t>{0u};
  if (name == "water_level")
    return {0u, 0u};
  if (name == "substrate_id")
    return {3u};
  if (name == "soil_depth")
    return {0u, 0x3cu};
  if (name == "biome_mix_ids")
    return {13u, 0u, 0u, 0u};
  return {255u, 0u, 0u, 0u};
}

world::MapExportField field(const std::string &name, const world::MapExportFormat format, const world::MapExportFilter filter, const std::string &group = {})
{
  world::MapExportField result{};
  result.name = name;
  result.format = format;
  result.filter = filter;
  result.available = true;
  result.width = 8u;
  result.height = 8u;
  result.samplingGroup = group;
  if (name == "refined_elevation")
    result.offset[0] = 320.0;
  const auto value = texel(name);
  constexpr uint32_t storedSide = 6u;
  for (uint8_t level = 0u; level <= 1u; ++level)
  {
    const uint32_t dimension = 1u << level;
    for (uint32_t y = 0u; y < dimension; ++y)
      for (uint32_t x = 0u; x < dimension; ++x)
      {
        world::MapExportPage page{};
        page.address = {level, x, y};
        page.width = storedSide;
        page.height = storedSide;
        page.data.resize(uint64_t{storedSide} * storedSide * value.size());
        for (uint32_t pixel = 0u; pixel < storedSide * storedSide; ++pixel)
          std::memcpy(page.data.data() + uint64_t{pixel} * value.size(), value.data(), value.size());
        result.pages.push_back(std::move(page));
      }
  }
  return result;
}

world::MapExportBundle bundle()
{
  world::MapExportBundle result{};
  result.resolution = 8u;
  result.pageSizePixels = 4u;
  result.gutterPixels = 1u;
  result.maximumPageLevel = 1u;
  result.extentMetres = 64.0;
  result.seed = 17u;
  result.bundleSignature = 23u;
  result.fields = {
    field("refined_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear),
    field("water_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("water_level", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("river", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("substrate_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("soil_depth", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("biome_mix_ids", world::MapExportFormat::RGBA8Uint, world::MapExportFilter::SparseMixture, "biome"),
    field("biome_mix_weights", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::SparseMixture, "biome"),
  };
  return result;
}

svr::TerrainGPUV2ColumnBuildSettings settings(const int32_t tileX, const uint32_t lod = 0u)
{
  svr::TerrainGPUV2ColumnBuildSettings result{};
  result.key.worldBundleRevision = 41u;
  result.key.tileX = tileX;
    result.key.tileZ = 0;
  result.key.clipmapLOD = lod;
  result.key.terrainSlot = 7u;
  result.key.expectedGeneration = 11u;
  result.key.shape = {8u, 8u, 1u, 0u};
  result.requestedMapLevel = 1u;
  result.worldOriginCanonicalXZ = {0, 0};
  result.worldExtentCanonical = 64u;
  result.heightUnitMetres = 1.0f;
  result.expectedAtlasRevision = 5u;
  result.faultCapacity = 256u;
  return result;
}

void testPlanningOracleAndSharing()
{
  std::cout << "canonical column planning and vertical sharing\n";
  const auto source = bundle();
  world::MapExportGpuPageLayout layout;
  std::string error;
  check(layout.initialize(source, 24u, &error), "virtual atlas layout initializes: " + error);
  svr::TerrainGPUV2ColumnConfigGPU leftConfig{};
  svr::TerrainGPUV2ColumnConfigGPU rightConfig{};
  check(
      svr::planTerrainGPUV2Columns(source, layout, settings(0), leftConfig, &error) && svr::planTerrainGPUV2Columns(source, layout, settings(1), rightConfig, &error),
      "adjacent integer tile configs plan successfully: " + error);
  std::vector<svr::TerrainGPUV2HeightSampleGPU> leftHeights;
  std::vector<svr::TerrainGPUV2HeightSampleGPU> rightHeights;
  std::vector<svr::TerrainGPUV2ColumnGPU> leftColumns;
  std::vector<svr::TerrainGPUV2ColumnGPU> rightColumns;
  check(
      svr::buildTerrainGPUV2ColumnOracle(source, leftConfig, leftHeights, leftColumns, &error) && svr::buildTerrainGPUV2ColumnOracle(source, rightConfig, rightHeights, rightColumns, &error),
      "CPU canonical column oracle builds map fixtures: " + error);
  const uint32_t leftSampleX = leftConfig.columnsX + leftConfig.halo * 2u + 1u;
  const uint32_t rightSampleX = rightConfig.columnsX + rightConfig.halo * 2u + 1u;
  bool exactEdge = true;
  for (uint32_t z = 0u; z <= leftConfig.columnsZ; ++z)
  {
    const auto &left = leftHeights[uint64_t{z + leftConfig.halo} * leftSampleX + leftConfig.halo + leftConfig.columnsX];
    const auto &right = rightHeights[uint64_t{z + rightConfig.halo} * rightSampleX + rightConfig.halo];
    exactEdge &= left.canonicalX == right.canonicalX && left.canonicalZ == right.canonicalZ && left.heightUnits == right.heightUnits;
  }
  check(exactEdge, "adjacent tiles produce bit-identical canonical shared-edge samples");
  svr::TerrainGPUV2ColumnConfigGPU coarseConfig{};
  std::vector<svr::TerrainGPUV2HeightSampleGPU> coarseHeights;
  std::vector<svr::TerrainGPUV2ColumnGPU> coarseColumns;
  bool crossLOD = svr::planTerrainGPUV2Columns(source, layout, settings(0, 1u), coarseConfig, &error) && svr::buildTerrainGPUV2ColumnOracle(source, coarseConfig, coarseHeights, coarseColumns, &error);
  const uint32_t coarseSampleX = coarseConfig.columnsX + coarseConfig.halo * 2u + 1u;
  for (uint32_t z = 0u; crossLOD && z <= 4u; ++z)
    for (uint32_t x = 0u; crossLOD && x <= 4u; ++x)
    {
      const auto &fine = leftHeights[uint64_t{leftConfig.halo + z * 2u} * leftSampleX + leftConfig.halo + x * 2u];
      const auto &coarse = coarseHeights[uint64_t{coarseConfig.halo + z} * coarseSampleX + coarseConfig.halo + x];
      crossLOD &= fine.canonicalX == coarse.canonicalX && fine.canonicalZ == coarse.canonicalZ && fine.heightUnits == coarse.heightUnits;
    }
  check(crossLOD, "fine and coarse LODs quantize common canonical samples identically");
  check(
      std::all_of(
          leftColumns.begin(),
          leftColumns.end(),
          [](const auto &column)
          {
            return column.minimumHeightUnits == 320 && column.maximumHeightUnits == 320 && column.material == 13u && column.flags == svr::TerrainGPUV2ColumnValid;
          }),
      "fixture columns quantize to exact heights and deterministic materials");
  double probedHeight = 0.0;
  check(
      svr::sampleTerrainGPUV2SurfaceHeight(
          source, 1u, 0.5, 0.5, probedHeight, &error) &&
          probedHeight == 320.0,
      "camera/diagnostic height probe matches the V2 column oracle: " +
          error);

  svr::TerrainGPUChunkKey lower{41u, 2, -3, 4, 0u, 3u, 7u, 11u};
  auto upper = lower;
  upper.chunkY = 9;
  svr::TerrainGPUV2ColumnTileKey lowerTile{};
  svr::TerrainGPUV2ColumnTileKey upperTile{};
  check(
      svr::terrainGPUV2ColumnTileKey(lower, {8u, 4u, 8u, 8u}, 2u, lowerTile, &error) && svr::terrainGPUV2ColumnTileKey(upper, {8u, 4u, 8u, 8u}, 2u, upperTile, &error) && lowerTile == upperTile,
      "vertically stacked chunks resolve to one X/Z column tile");
  svr::TerrainGPUV2ColumnTileRegistry registry;
  uint64_t lowerId = 0u;
  uint64_t upperId = 0u;
  bool lowerCreated = false;
  bool upperCreated = false;
  check(
      registry.acquire(101u, lowerTile, lowerId, lowerCreated, &error) && registry.acquire(102u, upperTile, upperId, upperCreated, &error) && lowerId == upperId && lowerCreated && !upperCreated &&
          registry.tile(lowerId)->consumerCount == 2u,
      "vertical jobs share one cached tile and one map-sampling build");
  check(registry.markAtlasReady(lowerId, 5u, &error) && registry.beginBuild(lowerId, &error), "shared tile transitions into an asynchronous build");
  svr::TerrainGPUV2ColumnResultGPU missing{};
  missing.status = static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::MissingPage);
  check(
      registry.completeBuild(lowerId, missing, {{0u, 1u, 1u, 0u}}, &error) && registry.tile(lowerId)->state == svr::TerrainGPUV2ColumnTileState::Faulted && registry.tile(lowerId)->retryCount == 1u,
      "missing pages retain compact faults and move the tile to retry state");
  check(registry.markAtlasReady(lowerId, 6u, &error) && registry.beginBuild(lowerId, &error), "faulted tile retries after a newer atlas commit");
  svr::TerrainGPUV2ColumnResultGPU ready{};
  ready.status = static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready);
  ready.columnCount = 144u;
  ready.writtenColumns = 144u;
  ready.terrainGeneration = 11u;
  ready.atlasRevisionLow = 6u;
  check(registry.completeBuild(lowerId, ready, {}, &error) && registry.tile(lowerId)->state == svr::TerrainGPUV2ColumnTileState::Ready, "successful retry publishes one shared immutable column tile");
  check(registry.release(101u, &error) && registry.tileCount() == 1u && registry.release(102u, &error) && registry.tileCount() == 0u, "shared tile remains cached until its final consumer releases it");
}

void testAtlasPinningFaultsAndBudget()
{
  std::cout << "virtual atlas pinning, transfer budget and faults\n";
  const auto source = bundle();
  svr::TerrainGPUV2WorldMapAtlasConfig atlasConfig{};
  atlasConfig.pageCache.slotCapacity = 24u;
  svr::TerrainGPUV2WorldMapAtlas atlas;
  std::string error;
  check(atlas.initialize(source, atlasConfig, &error), "V2 virtual map atlas initializes: " + error);
  svr::TerrainGPUV2ColumnConfigGPU config{};
  check(svr::planTerrainGPUV2Columns(source, atlas.layout(), settings(1), config, &error), "atlas-backed column config plans: " + error);
  world::MapExportStreamRequest footprint{};
  check(svr::terrainGPUV2ColumnFootprint(source, atlas.layout(), config, footprint, &error) && atlas.pin(1u, footprint, &error), "tile footprint expands required fields and pins its pages: " + error);
  check(!atlas.ready(1u) && atlas.waitForPreparation(std::chrono::seconds(2), &error), "a pinned tile waits while its pages prepare asynchronously");
  uint64_t uploaded = 0u;
  for (uint32_t iteration = 0u; iteration < 32u && !atlas.ready(1u); ++iteration)
  {
    world::MapExportPagePublication publication{};
    const bool published = atlas.publish(300u, publication, &error);
    check(published && publication.uploadedBytes <= 300u, "each atlas publication obeys the independent transfer-byte budget");
    if (!published)
      break;
    uploaded += publication.uploadedBytes;
    check(atlas.commit(publication, &error), "prepared page-table transaction commits atomically: " + error);
    if (!atlas.ready(1u))
      atlas.waitForPreparation(std::chrono::seconds(2), &error);
  }
  check(atlas.ready(1u) && uploaded != 0u, "tile becomes buildable only after every pinned page is committed");
  const auto resident = atlas.diagnostics();
  check(resident.generatedColumnReadbackBytes == 0u && resident.pinnedTiles == 1u && resident.readyTiles == 1u, "runtime atlas tracks compact metadata and reads back no generated columns");

  const auto elevation = atlas.layout().fieldIndex("refined_elevation");
  check(elevation.has_value() && atlas.reportFaults(1u, {{*elevation, 1u, 1u, 0u}}, &error) && !atlas.ready(1u), "unexpected GPU map faults preserve the pin and schedule exact retry pages");
  atlas.waitForPreparation(std::chrono::seconds(2), &error);
  for (uint32_t iteration = 0u; iteration < 8u && !atlas.ready(1u); ++iteration)
  {
    world::MapExportPagePublication publication{};
    check(atlas.publish(300u, publication, &error) && publication.uploadedBytes <= 300u && atlas.commit(publication, &error), "fault retry remains transfer-budgeted");
    atlas.waitForPreparation(std::chrono::seconds(2), &error);
  }
  check(atlas.ready(1u) && atlas.diagnostics().retryRequests == 1u, "faulted tile returns to ready after its retry transaction");

  world::MapExportPagePublication whilePinned{};
  check(atlas.publish(0u, whilePinned, &error) && whilePinned.evictions.empty() && atlas.commit(whilePinned, &error) && atlas.ready(1u), "an atlas update cannot evict or alter a pinned tile's samples");
  check(atlas.unpin(1u, &error), "tile pin releases explicitly");
  world::MapExportPagePublication evicted{};
  check(atlas.publish(0u, evicted, &error) && !evicted.evictions.empty() && atlas.commit(evicted, &error) && atlas.diagnostics().pinnedTiles == 0u, "unpinned detail pages become evictable while required roots remain");
}

void testWorkgroupQualification()
{
  std::cout << "column workgroup qualification\n";
  svr::TerrainGPUV2ColumnWorkgroupCandidate selected{};
  std::string error;
  check(
      svr::terrainGPUV2SelectColumnWorkgroup({{8u, 8u, 80u}, {16u, 8u, 61u}, {8u, 16u, 64u}, {16u, 16u, 20u}}, 128u, selected, &error) && selected.x == 16u && selected.z == 8u,
      "measured sweep selects the fastest compiled shape within device limits");
  check(!svr::terrainGPUV2SelectColumnWorkgroup({{16u, 16u, 20u}}, 128u, selected, &error), "uncompiled or over-limit workgroup shapes fail qualification closed");
}

} // namespace

int main()
{
  testPlanningOracleAndSharing();
  testAtlasPinningFaultsAndBudget();
  testWorkgroupQualification();
  std::cout << (failures == 0 ? "GPU terrain V2 Phase 3 CPU tests passed\n" : "GPU terrain V2 Phase 3 CPU tests failed\n");
  return failures == 0 ? 0 : 1;
}
