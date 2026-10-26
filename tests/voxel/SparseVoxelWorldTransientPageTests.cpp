// Round-trip cover for the generated-world transient page payload.
//
// A generated world page is regenerable, so the compressed store treats it as a
// pure cache. That makes a serialization gap invisible at the point it happens:
// a field added to PlanetVoxelChunkSet and not to the serializer survives every
// run in which the page stays in the uncompressed cache, and only produces
// wrong geometry after the first eviction, in whatever region the camera
// happened to leave. This test is the guard for that.

#include "voxel/svraster/PlanetVoxelChunk.hpp"
#include "voxel/svraster/TransientSparseVoxelPageStore.hpp"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

template <typename T>
bool sameValues(const std::vector<T> &left, const std::vector<T> &right)
{
  if (left.size() != right.size())
    return false;
  if (left.empty())
    return true;
  return std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0;
}

// Every field distinct from its default, so a field the serializer forgets
// comes back as the default and fails its comparison below.
voxel::svraster::PlanetVoxelChunkSet makePopulatedPage()
{
  voxel::svraster::PlanetVoxelChunkSet page{};
  page.centerTile = {0u, 3u, 5u, 6u};

  voxel::svraster::PlanetVoxelChunkRecord record{};
  record.requestedTile = {0u, 3u, 5u, 6u};
  record.sourceTile = {0u, 2u, 2u, 3u};
  record.tileOffsetX = -4;
  record.tileOffsetY = 7;
  record.clipmapLOD = 2u;
  record.clipmapSlot = 9u;
  record.regionMappingId = 41u;
  record.measuredErrorMetres = 0.375f;
  record.worldCenterXMetres = 1234.5;
  record.worldCenterZMetres = -987.25;
  record.edgeMetres = 12.8;
  record.voxelSizeMetres = 0.1;
  record.brickNodeOffset = 16u;
  record.brickNodeCapacity = 64u;
  record.brickChildOffset = 32u;
  record.brickChildCapacity = 128u;
  record.brickMaterialOffset = 48u;
  record.brickMaterialCapacity = 256u;
  record.brickPaletteOffset = 8u;
  record.brickPaletteCapacity = 24u;
  record.brickPayloadRevision = 111u;
  record.brickNodeRevision = 222u;
  record.tree64Revision = 333u;
  record.firstBrickRoot = 3u;
  record.brickRootCapacity = 12u;
  record.brickRootCount = 2u;
  record.elevationOriginMetres = -409.6;
  record.minimumSurfaceMetres = 12.0;
  record.maximumSurfaceMetres = 800.0;
  record.centerSurfaceMetres = 700.95;
  record.centerMaterial = 20u;
  record.leafVoxelCount = 17021u;
  record.brickCount = 266u;
  record.residencyGeneration = 5u;
  record.streamingPriority = 77u;
  page.chunks.push_back(record);

  page.edgeMetres = 12.8;
  page.voxelSizeMetres = 0.1;
  page.elevationOriginMetres = -409.6;
  page.centerSurfaceMetres = 700.95;
  page.centerMaterial = 20u;
  page.leafVoxelCount = 17021u;
  page.clipmapLayoutRevision = 8765u;
  page.progressiveGeneratedWorld = true;
  page.progressiveVirtualPages = true;
  page.progressiveSlotCapacity = 512u;
  page.progressiveWindowExtentMetres = 32000.0;
  page.progressiveSplitErrorPixels = 1.0f;
  page.progressiveMergeErrorPixels = 0.75f;
  page.progressiveDistanceErrorGrowthPerKilometre = 0.1f;
  page.progressiveDistanceErrorExponent = 1.5f;
  page.progressiveMinimumResidentFrames = 4u;
  page.progressiveCoarseningGraceFrames = 30u;

  voxel::svraster::PlanetVoxelHierarchyPageRewrite rewrite{};
  rewrite.kind = voxel::svraster::PlanetVoxelHierarchyRewriteKind::Refine;
  rewrite.expected = {7u, 1u, 0u, 4242u, 99u};
  rewrite.replacement = {7u, 2u, 0u, 4343u, 100u};
  page.hierarchyPageRewrites.push_back(rewrite);

  page.surfaceHeightSide = 4u;
  page.surfaceHeightHalo = 1u;
  page.surfaceHeights = {1.0f, 2.0f, 3.0f, 4.0f};
  page.surfaceEnvelope = {5.0f, 6.0f};
  page.surfaceMaterialSide = 2u;
  page.surfaceMaterials = {7u, 8u, 9u, 10u};

  voxel::svraster::PlanetVoxelMaterialFieldRegion region{};
  region.tile = {0u, 1u, 1u, 0u};
  region.regionMappingId = 13u;
  region.canonicalParentOrigin = {-3, 5};
  region.canonicalParentExtent = {8u, 9u};
  region.values[0] = {1u, 2u, 3u};
  page.materialFieldRegions.push_back(region);

  page.minimumWorldMetres = {-1.0, -2.0, -3.0};
  page.maximumWorldMetres = {4.0, 5.0, 6.0};

  auto &bricks = page.bricks;
  voxel::svraster::SparseVoxelBrickNodeGPU node{};
  node.minimumVoxelSize = {1.0f, 2.0f, 3.0f, 0.1f};
  node.packedOccupiedBounds = 0x1234u;
  node.coarseOccupancyLow = 0xabcdu;
  node.coarseOccupancyHigh = 0xef01u;
  node.occupancyIndex = 1u;
  node.materialBase = 2u;
  node.parentIndex = 0u;
  node.childCount = 1u;
  node.level = 3u;
  node.occupiedCount = 4u;
  node.firstChild = 5u;
  node.instanceIndex = 6u;
  node.paletteBase = 7u;
  node.regionMappingId = 13u;
  node.canonicalGridOriginScale = {8, 9, 10, 11};
  bricks.nodes.push_back(node);
  bricks.nodes.push_back(node);
  bricks.occupancy.resize(bricks.nodes.size());
  bricks.occupancy[0].cells[0] = {0xdeadbeefu, 3u};
  bricks.materialIndices = {1u, 2u, 3u};
  bricks.microSurfaceData = {10u, 20u, 30u};
  bricks.palette.resize(2u);
  bricks.palette[0].packed[0] = 0x11223344u;
  bricks.paletteDescriptorIds = {1u, 2u};
  bricks.paletteStride = 256u;
  bricks.paletteInstanceCount = 1u;
  bricks.paletteExactInstances = 1u;
  bricks.paletteMeanError = 0.25;
  bricks.paletteMaximumError = 1.5;
  bricks.paletteEmittedVoxels = 3u;
  bricks.paletteEmittedApproximated = 1u;
  bricks.paletteEmittedMeanError = 0.5;
  bricks.paletteEmittedMaximumError = 2.5;
  bricks.paletteStraddlingBricks = 1u;
  bricks.paletteStraddlingVoxels = 2u;
  bricks.childIndices = {0u, 1u};
  bricks.rootIndices = {1u};
  bricks.rootCount = 1u;
  bricks.levelCount = 2u;
  bricks.finestVoxelSize = 0.1f;
  return page;
}

bool sameBricks(
    const voxel::svraster::SparseVoxelBrickHierarchy &left,
    const voxel::svraster::SparseVoxelBrickHierarchy &right)
{
  return sameValues(left.nodes, right.nodes) &&
      sameValues(left.occupancy, right.occupancy) &&
      sameValues(left.materialIndices, right.materialIndices) &&
      sameValues(left.microSurfaceData, right.microSurfaceData) &&
      sameValues(left.palette, right.palette) &&
      sameValues(left.paletteDescriptorIds, right.paletteDescriptorIds) &&
      sameValues(left.childIndices, right.childIndices) &&
      sameValues(left.rootIndices, right.rootIndices) &&
      left.paletteStride == right.paletteStride &&
      left.paletteInstanceCount == right.paletteInstanceCount &&
      left.paletteExactInstances == right.paletteExactInstances &&
      left.paletteMeanError == right.paletteMeanError &&
      left.paletteMaximumError == right.paletteMaximumError &&
      left.paletteEmittedVoxels == right.paletteEmittedVoxels &&
      left.paletteEmittedApproximated == right.paletteEmittedApproximated &&
      left.paletteEmittedMeanError == right.paletteEmittedMeanError &&
      left.paletteEmittedMaximumError == right.paletteEmittedMaximumError &&
      left.paletteStraddlingBricks == right.paletteStraddlingBricks &&
      left.paletteStraddlingVoxels == right.paletteStraddlingVoxels &&
      left.rootCount == right.rootCount &&
      left.levelCount == right.levelCount &&
      left.finestVoxelSize == right.finestVoxelSize;
}

bool samePages(
    const voxel::svraster::PlanetVoxelChunkSet &left,
    const voxel::svraster::PlanetVoxelChunkSet &right)
{
  if (left.materialFieldRegions.size() != right.materialFieldRegions.size())
    return false;
  for (size_t index = 0u; index < left.materialFieldRegions.size(); ++index)
  {
    const auto &leftRegion = left.materialFieldRegions[index];
    const auto &rightRegion = right.materialFieldRegions[index];
    if (leftRegion.regionMappingId != rightRegion.regionMappingId ||
        leftRegion.canonicalParentOrigin != rightRegion.canonicalParentOrigin ||
        leftRegion.canonicalParentExtent != rightRegion.canonicalParentExtent)
      return false;
    for (size_t field = 0u; field < leftRegion.values.size(); ++field)
      if (!sameValues(leftRegion.values[field], rightRegion.values[field]))
        return false;
  }
  return sameValues(left.chunks, right.chunks) &&
      sameValues(left.hierarchyPageRewrites, right.hierarchyPageRewrites) &&
      sameValues(left.surfaceHeights, right.surfaceHeights) &&
      sameValues(left.surfaceEnvelope, right.surfaceEnvelope) &&
      sameValues(left.surfaceMaterials, right.surfaceMaterials) &&
      sameBricks(left.bricks, right.bricks) &&
      left.edgeMetres == right.edgeMetres &&
      left.voxelSizeMetres == right.voxelSizeMetres &&
      left.elevationOriginMetres == right.elevationOriginMetres &&
      left.centerSurfaceMetres == right.centerSurfaceMetres &&
      left.centerMaterial == right.centerMaterial &&
      left.leafVoxelCount == right.leafVoxelCount &&
      left.clipmapLayoutRevision == right.clipmapLayoutRevision &&
      left.progressiveGeneratedWorld == right.progressiveGeneratedWorld &&
      left.progressiveVirtualPages == right.progressiveVirtualPages &&
      left.progressiveSlotCapacity == right.progressiveSlotCapacity &&
      left.progressiveWindowExtentMetres ==
          right.progressiveWindowExtentMetres &&
      left.progressiveSplitErrorPixels == right.progressiveSplitErrorPixels &&
      left.progressiveMergeErrorPixels == right.progressiveMergeErrorPixels &&
      left.progressiveDistanceErrorGrowthPerKilometre ==
          right.progressiveDistanceErrorGrowthPerKilometre &&
      left.progressiveDistanceErrorExponent ==
          right.progressiveDistanceErrorExponent &&
      left.progressiveMinimumResidentFrames ==
          right.progressiveMinimumResidentFrames &&
      left.progressiveCoarseningGraceFrames ==
          right.progressiveCoarseningGraceFrames &&
      left.surfaceHeightSide == right.surfaceHeightSide &&
      left.surfaceHeightHalo == right.surfaceHeightHalo &&
      left.surfaceMaterialSide == right.surfaceMaterialSide &&
      left.minimumWorldMetres == right.minimumWorldMetres &&
      left.maximumWorldMetres == right.maximumWorldMetres;
}

} // namespace

int main()
{
  std::cout << "generated-world transient page payload\n";

  const auto original = makePopulatedPage();
  const auto payload =
      voxel::svraster::terrainVirtualPagePayloadFromChunkSet(original);
  check(
      !payload.records.empty() &&
          payload.records.front().localBrickNodeOffset == 0u &&
          payload.records.front().localBrickNodeCount ==
              original.bricks.nodes.size(),
      "the virtual page payload stores local brick ranges");
  check(
      !payload.records.empty() &&
          payload.records.front().localBrickNodeOffset !=
              original.chunks.front().brickNodeOffset,
      "physical brick placement is stripped from the transient payload");

  const std::vector<uint8_t> bytes =
      voxel::svraster::serializeTerrainVirtualPagePayload(payload);
  check(!bytes.empty(), "a populated page serializes to a non-empty payload");

  voxel::svraster::TerrainVirtualPagePayload restoredPayload{};
  std::string error;
  check(
      voxel::svraster::deserializeTerrainVirtualPagePayload(
          bytes, restoredPayload, &error),
      "the payload round-trips without an error");
  const auto expectedPage =
      voxel::svraster::terrainVirtualPagePayloadToChunkSet(payload);
  const auto restored =
      voxel::svraster::terrainVirtualPagePayloadToChunkSet(restoredPayload);
  check(
      samePages(expectedPage, restored),
      "every serialized field survives the round trip");

  voxel::svraster::PlanetVoxelChunkSet restoredThroughCompatibilityWrapper{};
  check(
      voxel::svraster::deserializeWorldVoxelTransientPage(
          voxel::svraster::serializeWorldVoxelTransientPage(original),
          restoredThroughCompatibilityWrapper, &error) &&
          samePages(expectedPage, restoredThroughCompatibilityWrapper),
      "the legacy chunk-set wrapper uses the virtual page payload format");

  // Serializing the restored page must reproduce the same bytes. This catches a
  // field that is written but read back into the wrong member, which the
  // comparison above can miss when both members hold the same value.
  check(
      voxel::svraster::serializeTerrainVirtualPagePayload(restoredPayload) ==
          bytes,
      "re-serializing the restored page reproduces the payload");

  std::vector<uint8_t> truncated = bytes;
  truncated.resize(bytes.size() - 1u);
  voxel::svraster::TerrainVirtualPagePayload rejected{};
  check(
      !voxel::svraster::deserializeTerrainVirtualPagePayload(
          truncated, rejected, &error) && !error.empty(),
      "a truncated payload is rejected rather than partially applied");

  std::vector<uint8_t> wrongMagic = bytes;
  wrongMagic[0] ^= 0xffu;
  check(
      !voxel::svraster::deserializeTerrainVirtualPagePayload(
          wrongMagic, rejected, &error),
      "a payload from another writer is rejected by its magic");

  // End to end through the store the streamer actually uses, including the
  // compressor and the owned spill directory.
  voxel::svraster::TransientSparseVoxelPageStore store;
  voxel::svraster::TransientSparseVoxelPageStoreSettings storeSettings{};
  storeSettings.maximumMemoryBytes = 1ull * 1024ull * 1024ull;
  storeSettings.maximumDiskBytes = 8ull * 1024ull * 1024ull;
  check(store.initialize(storeSettings, &error), "the page store initializes");
  const std::filesystem::path spillDirectory = store.spillDirectory();

  const voxel::svraster::TransientSparseVoxelPageKey key{0x1234u, 0x5678u};
  check(store.put(key, bytes, &error), "a serialized page enters the store");

  std::vector<uint8_t> fetched;
  const auto source = store.get(key, fetched, &error);
  check(
      source != voxel::svraster::TransientSparseVoxelPageSource::Missing &&
          fetched == bytes,
      "the store returns the payload it was given");

  voxel::svraster::TerrainVirtualPagePayload fromStore{};
  check(
      voxel::svraster::deserializeTerrainVirtualPagePayload(
          fetched, fromStore, &error) &&
          samePages(
              expectedPage,
              voxel::svraster::terrainVirtualPagePayloadToChunkSet(fromStore)),
      "a page survives compression and restoration through the store");

  std::vector<uint8_t> absent;
  check(
      store.get({0x1234u, 0x9999u}, absent, &error) ==
          voxel::svraster::TransientSparseVoxelPageSource::Missing,
      "an unknown key misses instead of returning another page");

  store.clear();
  check(
      store.get(key, absent, &error) ==
          voxel::svraster::TransientSparseVoxelPageSource::Missing,
      "clearing the store drops its pages");

  return failures == 0 ? 0 : 1;
}
