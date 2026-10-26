// Runtime refinement of terrain virtual geometry pages.
//
// Terrain chunks refine in place: the same slot is rewritten at a finer internal
// resolution and its brickPayloadRevision changes. The sample built its terrain
// VG assets once during scene construction, so after the first refinement the
// renderer kept drawing the coarse page - which looks like chunks that never
// refine and, where the finer page carries surface the coarse one did not, like
// missing faces.
//
// These tests pin the refresh loop: detect the revision change, rebuild only
// what changed, converge under a bounded per-update budget, and never serve a
// page that disagrees with a fresh build of the same payload.

#include "editor/virtualgeometry/TerrainVirtualGeometryPageCache.hpp"
#include "TerrainGenerationPhase0Utils.hpp"
#include "voxel/svraster/SparseVoxelBrickSurfaceMesh.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
namespace vg = virtualgeometry;

namespace
{

int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

bool buildTerrain(
    const terrain_phase0::FixtureKind kind,
    const uint32_t internalLOD,
    svr::PlanetVoxelChunkSet &chunks)
{
  const auto bundle = terrain_phase0::makeFixture(kind);
  const auto settings = terrain_phase0::fixtureSettings(kind, internalLOD);
  std::string error;
  svr::WorldMapVoxelGenerationMetrics metrics{};
  const bool built = svr::buildWorldMapVoxelTerrain(
      bundle, settings, chunks, &error, {}, nullptr, &metrics);
  if (!built)
    std::cout << "  terrain build failed: " << error << '\n';
  return built;
}

svr::SparseVoxelTerrainVirtualGeometryPageCacheSettings cacheSettings(
    const uint32_t budget)
{
  svr::SparseVoxelTerrainVirtualGeometryPageCacheSettings settings{};
  settings.maximumRebuildsPerUpdate = budget;
  settings.build.maxGroupsPerPage = vg::MAX_GROUPS_PER_PAGE;
  settings.quantization.unit_scale = 1.0f;
  settings.quantization.padMeshlets = true;
  return settings;
}

uint32_t pagesWithGeometry(const svr::PlanetVoxelChunkSet &chunks)
{
  uint32_t count = 0u;
  for (const auto &record : chunks.chunks)
    if (record.brickNodeCapacity != 0u && record.voxelSizeMetres > 0.0 &&
        record.edgeMetres > 0.0)
      ++count;
  return count;
}

uint32_t cachedTriangleCount(
    const svr::SparseVoxelTerrainVirtualGeometryPageCache &cache)
{
  uint32_t triangles = 0u;
  for (const auto &entry : cache.entries())
    if (entry.valid)
      triangles += entry.stats.build.triangleCount;
  return triangles;
}

// Exposed voxel faces of one page, per octree level, straight from the
// occupancy words. Used as the oracle for "no faces went missing".
std::map<uint32_t, uint64_t> exposedFacesPerLevel(
    const svr::PlanetVoxelChunkSet &chunks,
    const uint32_t chunkIndex)
{
  const auto &record = chunks.chunks[chunkIndex];
  std::map<uint32_t, std::set<std::array<int64_t, 3>>> levels;
  for (uint32_t node = record.brickNodeOffset;
       node < record.brickNodeOffset + record.brickNodeCapacity;
       ++node)
  {
    const auto &source = chunks.bricks.nodes[node];
    const uint32_t level = source.level & 0xffu;
    const int64_t scale = std::max<int64_t>(1, source.canonicalGridOriginScale[3]);
    const auto &occupancy = chunks.bricks.occupancy[source.occupancyIndex];
    for (uint32_t cell = 0u; cell < svr::kSparseVoxelBrickVoxelCount; ++cell)
    {
      if ((occupancy.cells[cell / 32u].bits & (uint32_t{1u} << (cell % 32u))) == 0u)
        continue;
      levels[level].insert({
          static_cast<int64_t>(source.canonicalGridOriginScale[0]) / scale +
              static_cast<int64_t>(cell % svr::kSparseVoxelBrickSide),
          static_cast<int64_t>(source.canonicalGridOriginScale[1]) / scale +
              static_cast<int64_t>((cell / svr::kSparseVoxelBrickSide) %
                                   svr::kSparseVoxelBrickSide),
          static_cast<int64_t>(source.canonicalGridOriginScale[2]) / scale +
              static_cast<int64_t>(cell / (svr::kSparseVoxelBrickSide *
                                           svr::kSparseVoxelBrickSide)),
      });
    }
  }

  static constexpr std::array<std::array<int64_t, 3>, 6> kOffsets{{
      {{-1, 0, 0}}, {{1, 0, 0}}, {{0, -1, 0}},
      {{0, 1, 0}},  {{0, 0, -1}}, {{0, 0, 1}},
  }};
  std::map<uint32_t, uint64_t> faces;
  for (const auto &[level, cells] : levels)
  {
    uint64_t count = 0u;
    for (const auto &cell : cells)
      for (const auto &offset : kOffsets)
        if (cells.count({cell[0] + offset[0], cell[1] + offset[1], cell[2] + offset[2]}) == 0u)
          ++count;
    faces[level] = count;
  }
  return faces;
}

// ── the cache contract ──────────────────────────────────────────────────────

void testFirstUpdateBuildsEveryPage()
{
  std::cout << "the first update builds every page with geometry\n";

  svr::PlanetVoxelChunkSet chunks{};
  check(buildTerrain(terrain_phase0::FixtureKind::Mountain, 0u, chunks), "terrain builds");
  if (chunks.chunks.empty())
    return;

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate update{};
  std::string error;
  check(
      cache.update(chunks, cacheSettings(0u), update, &error),
      "cache update succeeds: " + error);

  const uint32_t expected = pagesWithGeometry(chunks);
  check(expected > 0u, "the fixture has pages with geometry");
  check(update.staleChunks == expected, "every page starts stale");
  check(update.rebuiltChunks == expected, "every page is built");
  check(update.deferredChunks == 0u, "an unbounded update defers nothing");
  check(update.failedChunks == 0u, "no page fails to build");
  check(cache.validPageCount() == expected, "every page is cached");
  check(cache.isCurrent(chunks), "the cache is current after the first update");

  for (const auto &entry : cache.entries())
    if (entry.valid)
    {
      check(entry.generation == 1u, "a freshly built page is generation one");
      check(!entry.asset.pages.empty(), "a cached page carries encoded pages");
      check(
          entry.stats.build.levelCount > 1u,
          "a cached page carries a multi-level LOD chain");
      break;
    }
}

void testUnchangedChunksAreNotRebuilt()
{
  std::cout << "an unchanged chunk is never rebuilt\n";

  svr::PlanetVoxelChunkSet chunks{};
  if (!buildTerrain(terrain_phase0::FixtureKind::Cliff, 0u, chunks) || chunks.chunks.empty())
  {
    check(false, "terrain builds");
    return;
  }

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate first{};
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate second{};
  std::string error;
  cache.update(chunks, cacheSettings(0u), first, &error);
  const uint64_t generationsAfterFirst = cache.totalGenerations();
  check(
      cache.update(chunks, cacheSettings(0u), second, &error),
      "second update succeeds: " + error);

  check(second.staleChunks == 0u, "nothing is stale when nothing changed");
  check(second.rebuiltChunks == 0u, "nothing is rebuilt when nothing changed");
  check(second.reusedChunks == first.rebuiltChunks, "every page is reused");
  check(
      cache.totalGenerations() == generationsAfterFirst,
      "no page generation is burned on an idle update");
  check(second.rebuildMilliseconds < first.rebuildMilliseconds,
        "an idle update costs less than the building one");
}

void testRefinementIsDetectedAndRebuilt()
{
  std::cout << "a refined chunk is detected and rebuilt at the finer resolution\n";

  svr::PlanetVoxelChunkSet coarse{};
  svr::PlanetVoxelChunkSet fine{};
  const bool coarseBuilt =
      buildTerrain(terrain_phase0::FixtureKind::Mountain, 1u, coarse);
  const bool fineBuilt =
      buildTerrain(terrain_phase0::FixtureKind::Mountain, 0u, fine);
  check(coarseBuilt && fineBuilt, "coarse and fine terrain both build");
  if (!coarseBuilt || !fineBuilt || coarse.chunks.empty() || fine.chunks.empty())
    return;

  check(
      fine.chunks[0].voxelSizeMetres < coarse.chunks[0].voxelSizeMetres,
      "the refined page really is finer");
  check(
      fine.chunks[0].brickPayloadRevision != coarse.chunks[0].brickPayloadRevision,
      "refinement changes the payload revision");

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate update{};
  std::string error;
  cache.update(coarse, cacheSettings(0u), update, &error);
  const uint32_t coarseTriangles = cachedTriangleCount(cache);
  const uint32_t coarseMeshlets = update.rebuiltMeshletCount;
  check(coarseTriangles > 0u, "the coarse page caches geometry");

  // The refinement has to be visible as staleness before any rebuild runs.
  check(!cache.isCurrent(fine), "the refined payload is reported stale");

  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate refined{};
  check(
      cache.update(fine, cacheSettings(0u), refined, &error),
      "refinement update succeeds: " + error);
  check(refined.staleChunks == pagesWithGeometry(fine), "every refined page is stale");
  check(refined.rebuiltChunks == refined.staleChunks, "every stale page is rebuilt");
  check(cache.isCurrent(fine), "the cache is current after refinement");

  const uint32_t fineTriangles = cachedTriangleCount(cache);
  check(fineTriangles > coarseTriangles, "the refined page carries more geometry");
  check(
      refined.rebuiltMeshletCount > coarseMeshlets,
      "the refined page carries more meshlets");

  const auto *entry = cache.find(0u);
  check(entry != nullptr, "the refined page is cached");
  if (entry != nullptr)
  {
    check(entry->generation == 2u, "the refined page advanced one generation");
    check(
        entry->voxelSizeMetres == fine.chunks[0].voxelSizeMetres,
        "the cached page records the finer voxel size");
    check(
        entry->brickPayloadRevision == fine.chunks[0].brickPayloadRevision,
        "the cached page records the refined payload revision");
    check(
        entry->stats.build.levelCount >= 1u &&
            entry->stats.build.clustersPerLevel.size() ==
                entry->stats.build.levelCount,
        "the refined page reports one cluster count per level");
  }

  // Refinement must add surface, never drop it.
  const auto coarseFaces = exposedFacesPerLevel(coarse, 0u);
  const auto fineFaces = exposedFacesPerLevel(fine, 0u);
  check(!coarseFaces.empty() && !fineFaces.empty(), "both pages expose faces");
  if (!coarseFaces.empty() && !fineFaces.empty())
    check(
        fineFaces.begin()->second > coarseFaces.begin()->second,
        "the refined page exposes more voxel faces at its finest level");

  std::cout << "  coarse voxel=" << coarse.chunks[0].voxelSizeMetres
            << " triangles=" << coarseTriangles
            << " -> fine voxel=" << fine.chunks[0].voxelSizeMetres
            << " triangles=" << fineTriangles << '\n';
}

void testCoarseningIsAlsoDetected()
{
  std::cout << "a coarsened chunk is detected and rebuilt too\n";

  svr::PlanetVoxelChunkSet fine{};
  svr::PlanetVoxelChunkSet coarse{};
  if (!buildTerrain(terrain_phase0::FixtureKind::Cliff, 0u, fine) ||
      !buildTerrain(terrain_phase0::FixtureKind::Cliff, 1u, coarse) ||
      fine.chunks.empty() || coarse.chunks.empty())
  {
    check(false, "terrain builds at both resolutions");
    return;
  }

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate update{};
  std::string error;
  cache.update(fine, cacheSettings(0u), update, &error);
  const uint32_t fineTriangles = cachedTriangleCount(cache);

  check(!cache.isCurrent(coarse), "the coarsened payload is reported stale");
  check(
      cache.update(coarse, cacheSettings(0u), update, &error),
      "coarsening update succeeds: " + error);
  check(update.rebuiltChunks > 0u, "the coarsened page is rebuilt");
  check(
      cachedTriangleCount(cache) < fineTriangles,
      "coarsening drops geometry instead of keeping the fine page");
  check(cache.isCurrent(coarse), "the cache is current after coarsening");
}

// The streamer moves chunks between slots as the cut changes. The page for a
// ground depends on the ground and its brick payload, not on the slot or on
// the physical brick offsets the node revision hashes, so a moved chunk adopts
// the page its old slot held instead of meshing it again. On the slope test
// 786 of 944 rebuilds were exactly this: an unchanged payload in a new slot.
void testSlotMoveAdoptsExistingPage()
{
  std::cout << "a chunk that moves slots adopts its page instead of rebuilding\n";

  svr::PlanetVoxelChunkSet chunks{};
  if (!buildTerrain(terrain_phase0::FixtureKind::Cliff, 0u, chunks) || chunks.chunks.empty())
  {
    check(false, "terrain builds");
    return;
  }
  const auto templateRecord = chunks.chunks[0];
  const auto makeRecord = [&](const uint32_t slot, const uint32_t mappingId, const uint64_t nodeRevision)
  {
    auto record = templateRecord;
    record.clipmapSlot = slot;
    record.regionMappingId = mappingId;
    record.brickNodeRevision = nodeRevision;
    record.worldCenterXMetres = templateRecord.worldCenterXMetres + record.edgeMetres * (mappingId - 1000u);
    return record;
  };
  svr::PlanetVoxelChunkSet arranged = chunks;
  arranged.chunks = {makeRecord(0u, 1000u, 11u), makeRecord(1u, 1001u, 12u)};

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  std::string error;
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate first{};
  check(cache.update(arranged, cacheSettings(0u), first, &error), "first update succeeds: " + error);
  check(first.rebuiltChunks == 2u && first.adoptedChunks == 0u, "both grounds build once");
  const uint64_t generations = cache.totalGenerations();

  // Swap the two grounds between the slots; the relocation also rewrites the
  // physical node layout, which is what the node revision tracks.
  arranged.chunks = {makeRecord(0u, 1001u, 21u), makeRecord(1u, 1000u, 22u)};
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate moved{};
  check(cache.update(arranged, cacheSettings(0u), moved, &error), "moved update succeeds: " + error);
  check(moved.rebuiltChunks == 0u, "a slot move rebuilds nothing");
  check(moved.adoptedChunks == 2u, "both pages are adopted by their new slots");
  check(moved.staleChunks == 0u, "an adopted slot is not stale");
  check(cache.isCurrent(arranged), "the cache is current after the move");
  check(cache.totalGenerations() == generations, "no page generation is burned on a move");
  const auto *slot0 = cache.find(0u);
  const auto *slot1 = cache.find(1u);
  check(slot0 != nullptr && slot0->regionMappingId == 1001u && slot0->brickNodeRevision == 21u,
        "slot 0 now holds ground 1001 with the new node revision");
  check(slot1 != nullptr && slot1->regionMappingId == 1000u && slot1->brickNodeRevision == 22u,
        "slot 1 now holds ground 1000 with the new node revision");

  // A payload change is still a rebuild, even when the ground already has a
  // page somewhere.
  arranged.chunks[1].brickPayloadRevision ^= 0x5a5a5a5au;
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate changed{};
  check(cache.update(arranged, cacheSettings(0u), changed, &error), "payload update succeeds: " + error);
  check(changed.rebuiltChunks == 1u && changed.adoptedChunks == 0u, "a changed payload is rebuilt, not adopted");
}

void testBoundedUpdatesConverge()
{
  std::cout << "a bounded update budget still converges, page by page\n";

  svr::PlanetVoxelChunkSet chunks{};
  if (!buildTerrain(terrain_phase0::FixtureKind::Canyon, 0u, chunks) ||
      chunks.chunks.empty())
  {
    check(false, "terrain builds");
    return;
  }

  // The generated fixture is one page, so replicate its record across several
  // slots to exercise the scheduler: the cache only ever reasons about records,
  // and this is exactly the shape a toroidal slot grid presents.
  svr::PlanetVoxelChunkSet replicated = chunks;
  const auto templateRecord = chunks.chunks[0];
  replicated.chunks.clear();
  constexpr uint32_t kSlots = 6u;
  for (uint32_t slot = 0u; slot < kSlots; ++slot)
  {
    auto record = templateRecord;
    record.clipmapSlot = slot;
    record.regionMappingId = 1000u + slot;
    record.worldCenterXMetres =
        templateRecord.worldCenterXMetres + record.edgeMetres * slot;
    replicated.chunks.push_back(record);
  }

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  std::string error;
  uint32_t updates = 0u;
  uint32_t totalRebuilt = 0u;
  while (!cache.isCurrent(replicated) && updates < kSlots * 4u)
  {
    svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate update{};
    if (!cache.update(replicated, cacheSettings(1u), update, &error))
    {
      check(false, "bounded update succeeds: " + error);
      return;
    }
    check(
        update.rebuiltChunks <= 1u,
        "a bounded update never exceeds its budget");
    totalRebuilt += update.rebuiltChunks;
    ++updates;
  }

  check(cache.isCurrent(replicated), "bounded updates converge");
  check(updates == kSlots, "convergence takes exactly one update per page");
  check(totalRebuilt == kSlots, "every page is built exactly once");
  check(cache.validPageCount() == kSlots, "every page ends up cached");

  // No page may be starved: every slot must have been visited.
  bool everySlotBuilt = true;
  for (uint32_t slot = 0u; slot < kSlots; ++slot)
    everySlotBuilt = everySlotBuilt && cache.find(slot) != nullptr;
  check(everySlotBuilt, "the rotating cursor reaches every page");
}

void testFocusOrderingRebuildsNearestFirst()
{
  std::cout << "a focused update rebuilds the nearest stale page first\n";

  svr::PlanetVoxelChunkSet chunks{};
  if (!buildTerrain(terrain_phase0::FixtureKind::Cliff, 0u, chunks) ||
      chunks.chunks.empty())
  {
    check(false, "terrain builds");
    return;
  }

  svr::PlanetVoxelChunkSet replicated = chunks;
  const auto templateRecord = chunks.chunks[0];
  replicated.chunks.clear();
  constexpr uint32_t kSlots = 4u;
  for (uint32_t slot = 0u; slot < kSlots; ++slot)
  {
    auto record = templateRecord;
    record.clipmapSlot = slot;
    record.regionMappingId = 2000u + slot;
    record.worldCenterXMetres =
        templateRecord.worldCenterXMetres + record.edgeMetres * slot * 4.0;
    replicated.chunks.push_back(record);
  }

  auto settings = cacheSettings(1u);
  settings.useFocusPosition = true;
  // Sit on the last slot: it must be the one that refines first.
  settings.focusPosition = {
      replicated.chunks[kSlots - 1u].worldCenterXMetres,
      0.0,
      replicated.chunks[kSlots - 1u].worldCenterZMetres};

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate update{};
  std::string error;
  check(cache.update(replicated, settings, update, &error), "focused update succeeds: " + error);
  check(update.rebuiltChunks == 1u, "the budget is respected");
  check(
      update.rebuiltChunkIndices.size() == 1u &&
          update.rebuiltChunkIndices[0] == kSlots - 1u,
      "the page under the focus point is rebuilt first");
  check(update.deferredChunks == kSlots - 1u, "the rest are deferred, not dropped");
}

void testEvictionWhenAChunkLosesGeometry()
{
  std::cout << "a chunk that loses its bricks drops its cached page\n";

  svr::PlanetVoxelChunkSet chunks{};
  if (!buildTerrain(terrain_phase0::FixtureKind::River, 0u, chunks) ||
      chunks.chunks.empty())
  {
    check(false, "terrain builds");
    return;
  }

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate update{};
  std::string error;
  cache.update(chunks, cacheSettings(0u), update, &error);
  check(cache.find(0u) != nullptr, "the page starts cached");

  svr::PlanetVoxelChunkSet emptied = chunks;
  emptied.chunks[0].brickNodeCapacity = 0u;
  check(
      cache.update(emptied, cacheSettings(0u), update, &error),
      "eviction update succeeds: " + error);
  check(update.evictedChunks == 1u, "the emptied page is evicted");
  check(cache.find(0u) == nullptr, "the emptied page is no longer served");
  check(cache.isCurrent(emptied), "the cache is current after eviction");

  // A shrinking chunk set must drop its tail too.
  svr::PlanetVoxelChunkSet refilled = chunks;
  cache.update(refilled, cacheSettings(0u), update, &error);
  check(cache.find(0u) != nullptr, "the page comes back when the bricks do");
  svr::PlanetVoxelChunkSet shrunk = chunks;
  shrunk.chunks.clear();
  check(
      cache.update(shrunk, cacheSettings(0u), update, &error),
      "shrink update succeeds: " + error);
  check(update.evictedChunks == 1u, "the removed slot is evicted");
  check(cache.validPageCount() == 0u, "nothing is served for a removed slot");
}

void testCachedPagesMatchAFreshBuild()
{
  std::cout << "a cached page always matches a fresh build of the same payload\n";

  for (const auto kind : {terrain_phase0::FixtureKind::Mountain,
                          terrain_phase0::FixtureKind::Cliff,
                          terrain_phase0::FixtureKind::Coastline})
  {
    svr::PlanetVoxelChunkSet coarse{};
    svr::PlanetVoxelChunkSet fine{};
    if (!buildTerrain(kind, 1u, coarse) || !buildTerrain(kind, 0u, fine) ||
        coarse.chunks.empty() || fine.chunks.empty())
    {
      check(false, std::string("terrain builds: ") + terrain_phase0::fixtureName(kind));
      continue;
    }

    svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
    svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate update{};
    std::string error;
    cache.update(coarse, cacheSettings(0u), update, &error);
    cache.update(fine, cacheSettings(0u), update, &error);

    const auto *entry = cache.find(0u);
    if (entry == nullptr)
    {
      check(false, std::string("page cached: ") + terrain_phase0::fixtureName(kind));
      continue;
    }

    auto settings = cacheSettings(0u);
    vg::VirtualGeometryEncodedData fresh{};
    svr::SparseVoxelTerrainOctreeVirtualGeometryBuildStats freshStats{};
    std::array<double, 3> freshOrigin{};
    const bool freshBuilt = svr::buildSparseVoxelTerrainOctreeVirtualGeometryAsset(
        fine,
        0u,
        settings.octree,
        settings.build,
        settings.quantization,
        fresh,
        &freshOrigin,
        &freshStats,
        &error);
    check(freshBuilt, std::string("reference build: ") + terrain_phase0::fixtureName(kind));
    if (!freshBuilt)
      continue;

    check(
        entry->stats.encodedMeshletCount == freshStats.encodedMeshletCount &&
            entry->stats.build.triangleCount == freshStats.build.triangleCount &&
            entry->stats.build.clusterCount == freshStats.build.clusterCount,
        std::string("cached page equals a fresh build: ") +
            terrain_phase0::fixtureName(kind));
    check(
        entry->pageOrigin == freshOrigin,
        std::string("cached page origin matches: ") + terrain_phase0::fixtureName(kind));

    // And the cached geometry must still cover every exposed face of the
    // refined payload, at every level.
    const auto faces = exposedFacesPerLevel(fine, 0u);
    uint64_t oracleFaces = 0u;
    for (const auto &[level, count] : faces)
    {
      (void)level;
      oracleFaces += count;
    }
    check(
        entry->stats.build.exposedFaceCount == oracleFaces,
        std::string("cached page meshes every exposed face: ") +
            terrain_phase0::fixtureName(kind) + " built=" +
            std::to_string(entry->stats.build.exposedFaceCount) + " oracle=" +
            std::to_string(oracleFaces));
  }
}

void testRepeatedRefinementNeverServesStaleGeometry()
{
  std::cout << "alternating refinement never serves a stale page\n";

  svr::PlanetVoxelChunkSet coarse{};
  svr::PlanetVoxelChunkSet fine{};
  if (!buildTerrain(terrain_phase0::FixtureKind::Snow, 1u, coarse) ||
      !buildTerrain(terrain_phase0::FixtureKind::Snow, 0u, fine) ||
      coarse.chunks.empty() || fine.chunks.empty())
  {
    check(false, "terrain builds at both resolutions");
    return;
  }

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  std::string error;
  bool alwaysMatched = true;
  uint64_t previousGeneration = 0u;
  for (uint32_t iteration = 0u; iteration < 6u; ++iteration)
  {
    const svr::PlanetVoxelChunkSet &active = (iteration % 2u) == 0u ? fine : coarse;
    svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate update{};
    if (!cache.update(active, cacheSettings(0u), update, &error))
    {
      check(false, "update succeeds: " + error);
      return;
    }
    const auto *entry = cache.find(0u);
    alwaysMatched = alwaysMatched && entry != nullptr &&
        entry->brickPayloadRevision == active.chunks[0].brickPayloadRevision &&
        entry->voxelSizeMetres == active.chunks[0].voxelSizeMetres &&
        entry->generation == previousGeneration + 1u;
    previousGeneration = entry != nullptr ? entry->generation : previousGeneration;
  }
  check(alwaysMatched, "every served page matches the payload it was built from");
  check(
      cache.totalGenerations() == 6u,
      "each swap rebuilds exactly once");
}

std::string scratchPath(const std::string &name)
{
  const char *tmp = std::getenv("TMPDIR");
  std::string directory = tmp != nullptr ? tmp : "/tmp/";
  if (directory.back() != '/')
    directory.push_back('/');
  return directory + "terrain-vg-refinement-test-" + name + ".vgcache";
}

// Phase 8d of docs/terrain-vg-unified-hierarchy-plan.md: a chunk that leaves
// the cut keeps its geometry in the scratch file under its payload identity,
// and the same ground with the same content coming back is a revive from the
// file rather than a build. Identity is the content hash the terrain builder
// stamps on the record, so a different payload at the same ground is not
// mistaken for it, and both payloads can sit in the file at once.
void testRetiredChunksAreRevivedFromTheScratchFile()
{
  std::cout << "a retired chunk is revived from the scratch file instead of rebuilt (phase 8d)\n";

  svr::PlanetVoxelChunkSet chunks{};
  if (!buildTerrain(terrain_phase0::FixtureKind::River, 0u, chunks) ||
      chunks.chunks.empty())
  {
    check(false, "terrain builds");
    return;
  }

  const std::string path = scratchPath("retain");
  std::remove(path.c_str());
  svr::TerrainVirtualGeometryScratchFile file;
  std::string error;
  check(file.open(path, 0u, &error), "scratch file opens: " + error);

  svr::SparseVoxelTerrainVirtualGeometryPageCache cache;
  cache.setScratchFile(&file);
  auto settings = cacheSettings(0u);
  settings.writeScratchChunks = true;
  svr::SparseVoxelTerrainVirtualGeometryPageCacheUpdate update{};
  check(cache.update(chunks, settings, update, &error), "first update: " + error);
  const uint32_t built = update.rebuiltChunks;
  check(
      built == pagesWithGeometry(chunks) && update.scratchWrittenChunks == built,
      "every chunk is built and written");
  const auto *entry = cache.find(0u);
  if (entry == nullptr || entry->scratchChunk == svr::TerrainVirtualGeometryScratchFile::kInvalidChunk)
  {
    check(false, "chunk 0 has a scratch handle");
    file.close();
    return;
  }
  const uint32_t handle = entry->scratchChunk;
  const uint32_t pageCount = entry->pageCount;
  const uint64_t encodedBytes = entry->encodedPageBytes;
  const uint32_t clusterSources = static_cast<uint32_t>(entry->stats.build.clusterSources.size());

  // The chunk leaves the cut.
  svr::PlanetVoxelChunkSet emptied = chunks;
  emptied.chunks[0].brickNodeCapacity = 0u;
  check(cache.update(emptied, settings, update, &error), "eviction update: " + error);
  check(update.evictedChunks == 1u && update.retainedChunks == 1u, "the chunk is evicted and its scratch bytes retained");
  check(cache.find(0u) == nullptr, "and it is no longer served");
  check(file.isLive(handle), "the file still holds its chunk");
  check(
      cache.retainedChunkCount() == 1u && cache.retainedChunkBytes() == file.chunkBytesOf(handle),
      "the cache accounts the retained bytes");
  check(file.stats().chunksLive == built, "no region was released");

  // The same ground with the same content comes back.
  check(cache.update(chunks, settings, update, &error), "revive update: " + error);
  check(
      update.revivedChunks == 1u && update.rebuiltChunks == 0u && update.scratchWrittenChunks == 0u,
      "the chunk is revived, not rebuilt or rewritten");
  entry = cache.find(0u);
  check(entry != nullptr && entry->valid && entry->scratchChunk == handle, "the revived entry reads from the same scratch chunk");
  check(
      entry != nullptr && entry->pageCount == pageCount && entry->encodedPageBytes == encodedBytes &&
          entry->stats.build.clusterSources.size() == clusterSources,
      "with the shape and cluster boxes it had");
  check(cache.retainedChunkCount() == 0u && cache.retainedChunkBytes() == 0u, "and it is no longer retained");
  check(cache.isCurrent(chunks), "the cache is current after the revive");
  svr::TerrainVirtualGeometryScratchChunk header;
  check(file.readChunk(handle, header, &error) && header.pageCount == pageCount, "its header still reads: " + error);

  // A different payload at the same ground is another chunk, not this one.
  svr::PlanetVoxelChunkSet changed = chunks;
  changed.chunks[0].brickPayloadRevision ^= 0x9e3779b97f4a7c15ull;
  check(cache.update(changed, settings, update, &error), "changed-payload update: " + error);
  check(update.revivedChunks == 0u && update.rebuiltChunks == 1u, "a changed payload is rebuilt");
  check(cache.retainedChunkCount() == 1u, "and the previous payload's geometry is retained under its own identity");
  const auto *changedEntry = cache.find(0u);
  const uint32_t changedHandle = changedEntry != nullptr
      ? changedEntry->scratchChunk
      : svr::TerrainVirtualGeometryScratchFile::kInvalidChunk;
  check(
      changedHandle != handle && file.isLive(handle) && file.isLive(changedHandle),
      "both payloads are in the file");
  check(cache.update(chunks, settings, update, &error), "return update: " + error);
  check(
      update.revivedChunks == 1u && update.rebuiltChunks == 0u && cache.find(0u) != nullptr &&
          cache.find(0u)->scratchChunk == handle,
      "the original payload comes back from the file");
  check(cache.retainedChunkCount() == 1u, "and the changed one is retained in its turn");

  // Retention off: a retired chunk gives its bytes back at once.
  cache.setRetainRetiredScratchChunks(false);
  const uint64_t liveBefore = file.stats().chunksLive;
  check(cache.update(emptied, settings, update, &error), "eviction with retention off: " + error);
  check(update.retainedChunks == 0u && file.stats().chunksLive == liveBefore - 1u, "with retention off the region is released");
  cache.setRetainRetiredScratchChunks(true);
  cache.clear();
  check(file.stats().chunksLive == 0u, "clearing the cache releases every chunk, retained ones included");
  file.close();
  std::remove(path.c_str());

  // Under a file budget, retained chunks are what gives way to a live append.
  {
    const std::string boundedPath = scratchPath("retain-budget");
    std::remove(boundedPath.c_str());
    svr::TerrainVirtualGeometryScratchFile probe;
    check(probe.open(boundedPath, 0u, &error), "probe file opens: " + error);
    svr::SparseVoxelTerrainVirtualGeometryPageCache probeCache;
    probeCache.setScratchFile(&probe);
    check(probeCache.update(chunks, settings, update, &error), "probe update: " + error);
    const uint64_t workingSet = probe.stats().fileBytes;
    probeCache.clear();
    probe.close();
    std::remove(boundedPath.c_str());

    svr::TerrainVirtualGeometryScratchFile bounded;
    check(bounded.open(boundedPath, workingSet, &error), "bounded file opens at exactly the working set: " + error);
    svr::SparseVoxelTerrainVirtualGeometryPageCache boundedCache;
    boundedCache.setScratchFile(&bounded);
    check(boundedCache.update(chunks, settings, update, &error), "bounded first update: " + error);
    check(update.scratchWrittenChunks == built && update.scratchFailedChunks == 0u, "the working set fits exactly");
    check(boundedCache.update(emptied, settings, update, &error) && update.retainedChunks == 1u, "chunk 0 retires and is retained");
    // The same bricks under a new identity encode to the same bytes, so the
    // only way this append fits is by retiring the retained chunk.
    check(boundedCache.update(changed, settings, update, &error), "bounded changed-payload update: " + error);
    check(update.rebuiltChunks == 1u && update.scratchWrittenChunks == 1u, "the new payload is built and written");
    check(update.retainedEvictedChunks == 1u && boundedCache.retainedChunkCount() == 0u, "by retiring the retained chunk");
    check(bounded.stats().appendsRefusedByBudget == 0u && update.scratchFailedChunks == 0u, "and no append was refused");
    check(bounded.stats().fileBytes <= workingSet, "the file stays within its bound");
    boundedCache.clear();
    bounded.close();
    std::remove(boundedPath.c_str());
  }
}

} // namespace

int main()
{
  std::cout << "Sparse voxel terrain virtual geometry runtime refinement tests\n";
  testFirstUpdateBuildsEveryPage();
  testUnchangedChunksAreNotRebuilt();
  testRefinementIsDetectedAndRebuilt();
  testCoarseningIsAlsoDetected();
  testSlotMoveAdoptsExistingPage();
  testBoundedUpdatesConverge();
  testFocusOrderingRebuildsNearestFirst();
  testEvictionWhenAChunkLosesGeometry();
  testCachedPagesMatchAFreshBuild();
  testRepeatedRefinementNeverServesStaleGeometry();
  testRetiredChunksAreRevivedFromTheScratchFile();

  if (failures != 0)
  {
    std::cout << failures << " terrain VG runtime refinement checks failed\n";
    return 1;
  }
  std::cout << "all terrain VG runtime refinement checks passed\n";
  return 0;
}
