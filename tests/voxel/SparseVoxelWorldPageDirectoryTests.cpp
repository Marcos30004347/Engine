// The generated-world page directory.
//
// The directory exists so the streamer can name, order and choose pages whose
// payload is not loaded. Today a page nothing holds is absent from every
// structure, which is why installs cannot be ordered against a parent and why
// refinement has to be published as one whole-world cut. Everything asserted
// here is a property an install order depends on.

#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <memory>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

bool sameKey(
    const svr::TransientSparseVoxelPageKey &left,
    const svr::TransientSparseVoxelPageKey &right)
{
  return left.source == right.source && left.page == right.page;
}

svr::WorldVoxelChunkStreamingSettings makeSettings(
    const double renderDistanceMetres)
{
  svr::WorldVoxelChunkStreamingSettings settings{};
  settings.chunkEdgeMetres = 12.8;
  settings.finestVoxelSizeMetres = 0.1;
  settings.finestHorizontalVoxelCount = 128u;
  settings.renderDistanceMetres = renderDistanceMetres;
  settings.projectionScalePixels = 467.654;
  settings.splitErrorPixels = 1.0f;
  settings.mergeErrorPixels = 0.75f;
  settings.maximumSpatialLOD = svr::kPlanetVoxelMaximumClipmapLevels - 1u;
  settings.maximumResidentChunks = 4096u;
  return settings;
}

// Parent/child links agree in both directions, every entry reaches a root, and
// no address appears twice. A directory that fails any of these cannot order an
// install.
bool structurallySound(const svr::WorldVoxelPageDirectory &directory)
{
  std::set<std::tuple<uint32_t, int32_t, int32_t>> seen;
  for (uint32_t index = 0u;
       index < static_cast<uint32_t>(directory.entries.size()); ++index)
  {
    const auto &entry = directory.entries[index];
    if (!seen
             .insert({entry.address.spatialLOD, entry.address.x,
                      entry.address.z})
             .second)
      return false;

    if (entry.parent == svr::kWorldVoxelInvalidPage)
    {
      if (std::find(directory.roots.begin(), directory.roots.end(), index) ==
          directory.roots.end())
        return false;
    }
    else
    {
      if (entry.parent >= directory.entries.size())
        return false;
      const auto &parent = directory.entries[entry.parent];
      if (parent.address.spatialLOD != entry.address.spatialLOD + 1u)
        return false;
      const auto *const first = parent.children.data();
      if (std::find(first, first + parent.childCount, index) ==
          first + parent.childCount)
        return false;
      // A parent must be strictly coarser, or the cut metric is not monotonic.
      if (!(parent.errorEnvelopeMetres > entry.errorEnvelopeMetres))
        return false;
      // The child has to sit inside its parent's footprint.
      if (entry.minimumXMetres < parent.minimumXMetres - 1.0e-6 ||
          entry.minimumZMetres < parent.minimumZMetres - 1.0e-6 ||
          entry.minimumXMetres + entry.edgeMetres >
              parent.minimumXMetres + parent.edgeMetres + 1.0e-6 ||
          entry.minimumZMetres + entry.edgeMetres >
              parent.minimumZMetres + parent.edgeMetres + 1.0e-6)
        return false;
    }

    for (uint32_t child = 0u; child < entry.childCount; ++child)
    {
      const uint32_t childIndex = entry.children[child];
      if (childIndex >= directory.entries.size() ||
          directory.entries[childIndex].parent != index)
        return false;
    }

    // Reachability: walking parents must terminate at a root.
    uint32_t walk = index;
    uint32_t guard = 0u;
    while (directory.entries[walk].parent != svr::kWorldVoxelInvalidPage)
    {
      walk = directory.entries[walk].parent;
      if (++guard > svr::kPlanetVoxelMaximumClipmapLevels + 2u)
        return false;
    }
    if (std::find(directory.roots.begin(), directory.roots.end(), walk) ==
        directory.roots.end())
      return false;
  }
  return true;
}

// Entries are ordered coarsest first, so a linear walk is already a legal
// install order: a parent is never visited after one of its children.
bool orderedForInstall(const svr::WorldVoxelPageDirectory &directory)
{
  for (uint32_t index = 0u;
       index < static_cast<uint32_t>(directory.entries.size()); ++index)
    if (directory.entries[index].parent != svr::kWorldVoxelInvalidPage &&
        directory.entries[index].parent >= index)
      return false;
  return true;
}

void makeEveryPageCached(svr::WorldVoxelPageDirectory &directory)
{
  for (auto &entry : directory.entries)
    entry.state = svr::WorldVoxelPageState::Cached;
}

bool installPage(
    svr::WorldVoxelPageDirectory &directory,
    const uint32_t pageIndex,
    const uint32_t generation)
{
  svr::TerrainPagePatchBatch batch{};
  std::string error;
  return svr::prepareWorldVoxelPageInstall(
             directory, pageIndex, generation, batch, &error) &&
      svr::commitWorldVoxelPageInstall(directory, batch, &error);
}

bool uninstallPage(
    svr::WorldVoxelPageDirectory &directory,
    const uint32_t pageIndex)
{
  svr::TerrainPagePatchBatch batch{};
  std::string error;
  return svr::prepareWorldVoxelPageUninstall(
             directory, pageIndex, batch, &error) &&
      svr::commitWorldVoxelPageUninstall(directory, batch, &error);
}

bool cutContainsAddress(
    const svr::WorldVoxelPageDirectory &directory,
    const std::vector<uint32_t> &cut,
    const svr::WorldVoxelChunkAddress &address)
{
  return std::any_of(
      cut.begin(), cut.end(),
      [&](const uint32_t pageIndex)
      {
        return pageIndex < directory.entries.size() &&
            directory.entries[pageIndex].address == address;
      });
}

bool parentBeforeChild(
    const svr::WorldVoxelPageDirectory &directory,
    const std::vector<uint32_t> &pages)
{
  for (size_t child = 0u; child < pages.size(); ++child)
  {
    const uint32_t childIndex = pages[child];
    if (childIndex >= directory.entries.size())
      return false;
    uint32_t parent = directory.entries[childIndex].parent;
    while (parent != svr::kWorldVoxelInvalidPage)
    {
      const auto found = std::find(pages.begin(), pages.end(), parent);
      if (found != pages.end() &&
          static_cast<size_t>(found - pages.begin()) > child)
        return false;
      parent = directory.entries[parent].parent;
    }
  }
  return true;
}

bool childBeforeParent(
    const svr::WorldVoxelPageDirectory &directory,
    const std::vector<uint32_t> &pages)
{
  for (size_t parentPosition = 0u; parentPosition < pages.size();
       ++parentPosition)
  {
    const uint32_t parentIndex = pages[parentPosition];
    if (parentIndex >= directory.entries.size())
      return false;
    for (size_t childPosition = parentPosition + 1u;
         childPosition < pages.size(); ++childPosition)
    {
      uint32_t walk = pages[childPosition];
      while (walk != svr::kWorldVoxelInvalidPage &&
             walk < directory.entries.size())
      {
        walk = directory.entries[walk].parent;
        if (walk == parentIndex)
          return false;
      }
    }
  }
  return true;
}

} // namespace

int main()
{
  std::cout << "generated-world page directory\n";

  const double worldExtent = 52428.8;
  const auto settings = makeSettings(16000.0);
  std::string error;

  // The state the acceptance criterion names: a complete directory over a
  // 16 km world with no page payload resident anywhere.
  const auto cut = svr::buildWorldVoxelChunkLeafCut(
      worldExtent, 0.0, 0.0, settings);
  check(cut.size() > 1000u, "the 16 km leaf cut resolves many pages");

  svr::WorldVoxelPageDirectory directory{};
  check(
      svr::buildWorldVoxelPageDirectory(
          worldExtent, settings, cut, directory, &error),
      "the directory builds from a leaf cut");
  check(
      directory.entries.size() > cut.size(),
      "the directory holds more than the cut itself");
  check(structurallySound(directory), "the directory is structurally sound");
  check(orderedForInstall(directory), "entries are ordered parent before child");

  bool everyCutPagePresent = true;
  for (const auto &address : cut)
    if (directory.find(address) == svr::kWorldVoxelInvalidPage)
      everyCutPagePresent = false;
  check(everyCutPagePresent, "every cut page is in the directory");

  // The refinement frontier is what an install decision reads: the pages that
  // could be installed next, present and orderable with no payload.
  uint32_t frontierPresent = 0u;
  uint32_t frontierExpected = 0u;
  for (const auto &address : cut)
  {
    if (address.spatialLOD == 0u)
      continue;
    for (int32_t childZ = 0; childZ != 2; ++childZ)
      for (int32_t childX = 0; childX != 2; ++childX)
      {
        const svr::WorldVoxelChunkAddress child{
          address.x * 2 + childX, address.z * 2 + childZ,
          address.spatialLOD - 1u};
        const double edge = std::ldexp(
            settings.chunkEdgeMetres, static_cast<int>(child.spatialLOD));
        if (static_cast<double>(child.x) * edge >= worldExtent ||
            static_cast<double>(child.z) * edge >= worldExtent)
          continue;
        ++frontierExpected;
        if (directory.find(child) != svr::kWorldVoxelInvalidPage)
          ++frontierPresent;
      }
  }
  check(
      frontierExpected != 0u && frontierPresent == frontierExpected,
      "every child of a cut page is in the directory");

  bool absentWithoutPayload = std::all_of(
      directory.entries.begin(), directory.entries.end(),
      [](const svr::WorldVoxelPageDirectoryEntry &entry)
      {
        return entry.state == svr::WorldVoxelPageState::Absent &&
            entry.residencyGeneration == 0u;
      });
  check(
      absentWithoutPayload,
      "a directory with no payloads is complete and entirely Absent");

  svr::assignWorldVoxelPageDirectoryPayloadKeys(
      0x1234u, settings, 0u, directory);
  const uint32_t keyedIndex = directory.find(cut.front());
  const bool everyEntryKeyed = std::all_of(
      directory.entries.begin(), directory.entries.end(),
      [](const svr::WorldVoxelPageDirectoryEntry &entry)
      { return entry.payloadKey.source != 0u && entry.payloadKey.page != 0u; });
  check(
      keyedIndex != svr::kWorldVoxelInvalidPage && everyEntryKeyed &&
          sameKey(
              directory.entries[keyedIndex].payloadKey,
              svr::worldTransientPageKey(0x1234u, settings, cut.front(), 0u)),
      "directory entries own deterministic page-source keys before payloads");

  directory.entries[keyedIndex].state =
      svr::WorldVoxelPageState::Generating;
  directory.entries[keyedIndex].payloadRevision = 99u;
  directory.entries[keyedIndex].measuredErrorMetres = 0.5f;
  svr::WorldVoxelPageDirectory rebuiltDirectory{};
  check(
      svr::buildWorldVoxelPageDirectory(
          worldExtent, settings, cut, rebuiltDirectory, &error),
      "a directory rebuild can recreate topology");
  svr::preserveWorldVoxelPageDirectoryState(directory, rebuiltDirectory);
  const uint32_t preservedIndex = rebuiltDirectory.find(cut.front());
  check(
      preservedIndex != svr::kWorldVoxelInvalidPage &&
          rebuiltDirectory.entries[preservedIndex].state ==
              svr::WorldVoxelPageState::Generating &&
          rebuiltDirectory.entries[preservedIndex].payloadRevision == 99u &&
          rebuiltDirectory.entries[preservedIndex].measuredErrorMetres == 0.5f,
      "directory page state survives topology rebuilds by address");

  // Annotation from an empty cache must not invent residency.
  svr::WorldVoxelChunkCache emptyCache{};
  svr::annotateWorldVoxelPageDirectory(emptyCache, directory);
  absentWithoutPayload = std::all_of(
      directory.entries.begin(), directory.entries.end(),
      [](const svr::WorldVoxelPageDirectoryEntry &entry)
      { return entry.state == svr::WorldVoxelPageState::Absent; });
  check(absentWithoutPayload, "an empty cache leaves every page Absent");

  // Residency and caching are reported, and residency wins over caching.
  svr::WorldVoxelChunkCache annotatedCache{};
  annotatedCache.bundleSignature = 0x1234u;
  annotatedCache.settings = settings;
  svr::WorldVoxelChunkEntry resident{};
  resident.address = cut.front();
  resident.slot = 5u;
  resident.generation = 7u;
  resident.residentLOD = 0u;
  resident.allocation.id = 17u;
  {
    svr::PlanetVoxelChunkSet payload{};
    payload.clipmapLayoutRevision = 99u;
    payload.chunks.push_back({});
    payload.chunks.back().measuredErrorMetres = 3.5f;
    resident.chunk = std::make_shared<const svr::PlanetVoxelChunkSet>(std::move(payload));
  }
  annotatedCache.entries.push_back(resident);
  svr::WorldVoxelChunkCachedPayload cached{};
  cached.address = cut.back();
  cached.lod = 0u;
  cached.bundleSignature = 0x1234u;
  {
    svr::PlanetVoxelChunkSet payload{};
    payload.clipmapLayoutRevision = 55u;
    payload.chunks.push_back({});
    payload.chunks.back().measuredErrorMetres = 1.25f;
    cached.chunk = std::make_shared<const svr::PlanetVoxelChunkSet>(std::move(payload));
  }
  annotatedCache.payloadCache.push_back(cached);
  svr::WorldVoxelChunkCachedPayload alsoResident{};
  alsoResident.address = cut.front();
  alsoResident.lod = 0u;
  alsoResident.bundleSignature = 0x1234u;
  {
    svr::PlanetVoxelChunkSet payload{};
    payload.clipmapLayoutRevision = 44u;
    payload.chunks.push_back({});
    payload.chunks.back().measuredErrorMetres = 2.0f;
    alsoResident.chunk = std::make_shared<const svr::PlanetVoxelChunkSet>(std::move(payload));
  }
  annotatedCache.payloadCache.push_back(alsoResident);
  svr::WorldVoxelChunkEntry installing{};
  installing.address = cut[cut.size() / 2u];
  installing.slot = 11u;
  installing.residentLOD = 0u;
  installing.generation = 13u;
  installing.allocation.id = 23u;
  {
    svr::PlanetVoxelChunkSet payload{};
    payload.clipmapLayoutRevision = 66u;
    payload.chunks.push_back({});
    payload.chunks.back().measuredErrorMetres = 2.5f;
    installing.chunk = std::make_shared<const svr::PlanetVoxelChunkSet>(std::move(payload));
  }
  annotatedCache.stagedSpatialEntries.push_back(installing);
  svr::WorldVoxelChunkEntry outsideResident{};
  outsideResident.address = {-64, -64, 0u};
  outsideResident.slot = 29u;
  outsideResident.residentLOD = 0u;
  outsideResident.generation = 31u;
  outsideResident.allocation.id = 37u;
  annotatedCache.entries.push_back(outsideResident);

  const auto payloadRecords =
      svr::collectWorldVoxelPagePayloadRecords(annotatedCache);
  const auto residencyRecords =
      svr::collectWorldVoxelPageResidencyRecords(annotatedCache);
  const auto payloadLocationCount =
      [&](const svr::WorldVoxelPagePayloadLocation location)
      {
        return static_cast<uint32_t>(std::count_if(
            payloadRecords.begin(), payloadRecords.end(),
            [&](const svr::WorldVoxelPagePayloadRecord &record)
            { return record.location == location; }));
      };
  const auto residencyLocationCount =
      [&](const svr::WorldVoxelPageResidencyLocation location)
      {
        return static_cast<uint32_t>(std::count_if(
            residencyRecords.begin(), residencyRecords.end(),
            [&](const svr::WorldVoxelPageResidencyRecord &record)
            { return record.location == location; }));
      };
  check(
      payloadRecords.size() == 5u &&
          payloadLocationCount(
              svr::WorldVoxelPagePayloadLocation::FrontCache) == 2u &&
          payloadLocationCount(
              svr::WorldVoxelPagePayloadLocation::Installing) == 1u &&
          payloadLocationCount(
              svr::WorldVoxelPagePayloadLocation::Resident) == 2u,
      "payload records are collected independently from residency");
  check(
      residencyRecords.size() == 3u &&
          residencyLocationCount(
              svr::WorldVoxelPageResidencyLocation::Installing) == 1u &&
          residencyLocationCount(
              svr::WorldVoxelPageResidencyLocation::Resident) == 2u,
      "residency records describe only physical allocations");
  check(
      std::any_of(
          residencyRecords.begin(), residencyRecords.end(),
          [&](const svr::WorldVoxelPageResidencyRecord &record)
          {
            return record.address == outsideResident.address &&
                record.residentSlot == outsideResident.slot &&
                record.allocationId == outsideResident.allocation.id;
          }),
      "physical residency can exist before the directory accepts a page");
  svr::annotateWorldVoxelPageDirectory(annotatedCache, directory);

  const uint32_t residentIndex = directory.find(cut.front());
  const uint32_t cachedIndex = directory.find(cut.back());
  const uint32_t installingIndex = directory.find(installing.address);
  check(
      residentIndex != svr::kWorldVoxelInvalidPage &&
          directory.entries[residentIndex].state ==
              svr::WorldVoxelPageState::Resident &&
          directory.entries[residentIndex].residentSlot == resident.slot &&
          directory.entries[residentIndex].residencyGeneration == 7u &&
          directory.entries[residentIndex].payloadRevision == 99u &&
          directory.entries[residentIndex].measuredErrorMetres == 3.5f,
      "an installed page reports Resident while retaining payload metadata");
  check(
      cachedIndex != svr::kWorldVoxelInvalidPage &&
          directory.entries[cachedIndex].state ==
              svr::WorldVoxelPageState::Cached &&
          directory.entries[cachedIndex].payloadRevision == 55u &&
          directory.entries[cachedIndex].measuredErrorMetres == 1.25f &&
          sameKey(
              directory.entries[cachedIndex].payloadKey,
              svr::worldTransientPageKey(
                  0x1234u, settings, cached.address, cached.lod)),
      "a page held only by the payload cache reports Cached");
  check(
      installingIndex != svr::kWorldVoxelInvalidPage &&
          directory.entries[installingIndex].state ==
              svr::WorldVoxelPageState::Installing &&
          directory.entries[installingIndex].residentSlot == 11u &&
          directory.entries[installingIndex].residencyGeneration == 13u &&
          directory.entries[installingIndex].payloadRevision == 66u &&
          directory.entries[installingIndex].measuredErrorMetres == 2.5f,
      "a staged page reports Installing before publication");
  const auto annotatedRuntime =
      svr::buildWorldVoxelTerrainRuntimePageTable(directory);
  check(
      installingIndex != svr::kWorldVoxelInvalidPage &&
          (annotatedRuntime.pages[installingIndex].flags &
           svr::kWorldVoxelTerrainRuntimePagePayloadInstalled) != 0u &&
          (annotatedRuntime.pages[installingIndex].flags &
           svr::kWorldVoxelTerrainRuntimePageResident) == 0u &&
          (annotatedRuntime.pages[installingIndex].flags &
           svr::kWorldVoxelTerrainRuntimePageStreamingLeaf) == 0u,
      "an installing payload is explicit and cannot enter the visible cut");
  check(
      directory.find(outsideResident.address) == svr::kWorldVoxelInvalidPage,
      "the directory remains authoritative over out-of-topology residency");

  const uint32_t absentCount = static_cast<uint32_t>(std::count_if(
      directory.entries.begin(), directory.entries.end(),
      [](const svr::WorldVoxelPageDirectoryEntry &entry)
      { return entry.state == svr::WorldVoxelPageState::Absent; }));
  check(
      absentCount == directory.entries.size() - 3u,
      "annotation marks exactly the pages something holds");

  // A cold start has no cut at all and must still produce a usable root set.
  svr::WorldVoxelPageDirectory coldDirectory{};
  check(
      svr::buildWorldVoxelPageDirectory(
          worldExtent, settings, {}, coldDirectory, &error) &&
          !coldDirectory.entries.empty() && !coldDirectory.roots.empty() &&
          coldDirectory.roots.size() == coldDirectory.entries.size(),
      "an empty cut still yields the root level");
  check(
      structurallySound(coldDirectory),
      "the cold-start directory is structurally sound");
  svr::assignWorldVoxelPageDirectoryPayloadKeys(
      0x1234u, settings, 0u, coldDirectory);
  const bool coldRootsAreRequestable = std::all_of(
      coldDirectory.entries.begin(), coldDirectory.entries.end(),
      [](const svr::WorldVoxelPageDirectoryEntry &entry)
      {
        return entry.state == svr::WorldVoxelPageState::Absent &&
            entry.payloadKey.source != 0u && entry.payloadKey.page != 0u;
      });
  check(
      coldRootsAreRequestable,
      "cold-start root pages are requestable before payloads exist");

  // The leaf-cut traversal visits pages outside the world and rejects them, so
  // an out-of-world address can reach this builder. It must be skipped rather
  // than becoming an entry with no containing parent, and must not fail the
  // build for the in-world pages beside it.
  const auto smallSettings = makeSettings(400.0);
  const std::vector<svr::WorldVoxelChunkAddress> mixedCut{
    {-1, -1, 0u}, {-1, 0, 0u}, {0, -1, 0u}, {0, 0, 0u}, {1, 0, 0u}};
  svr::WorldVoxelPageDirectory mixedDirectory{};
  const bool mixedBuilt = svr::buildWorldVoxelPageDirectory(
      worldExtent, smallSettings, mixedCut, mixedDirectory, &error);
  check(mixedBuilt, "an out-of-world address does not fail the build");
  check(
      mixedBuilt && structurallySound(mixedDirectory) &&
          orderedForInstall(mixedDirectory),
      "the directory around the world origin is structurally sound");
  check(
      mixedBuilt &&
          mixedDirectory.find({-1, -1, 0u}) == svr::kWorldVoxelInvalidPage &&
          mixedDirectory.find({-1, 0, 0u}) == svr::kWorldVoxelInvalidPage &&
          mixedDirectory.find({0, -1, 0u}) == svr::kWorldVoxelInvalidPage,
      "a page entirely outside the world is omitted");
  check(
      mixedBuilt &&
          mixedDirectory.find({0, 0, 0u}) != svr::kWorldVoxelInvalidPage &&
          mixedDirectory.find({1, 0, 0u}) != svr::kWorldVoxelInvalidPage &&
          mixedDirectory.coveredCount == 2u,
      "the in-world pages beside it are still covered");

  // The page at the world's far edge is the one a truncating parent division
  // would misplace if the convention ever moved off zero.
  const double lastPageEdge = std::ldexp(smallSettings.chunkEdgeMetres, 0);
  const int32_t lastPage =
      static_cast<int32_t>(std::floor(worldExtent / lastPageEdge)) - 1;
  svr::WorldVoxelPageDirectory edgeDirectory{};
  const bool edgeBuilt = svr::buildWorldVoxelPageDirectory(
      worldExtent, smallSettings, {{lastPage, lastPage, 0u}}, edgeDirectory,
      &error);
  check(
      edgeBuilt && structurallySound(edgeDirectory) &&
          edgeDirectory.find({lastPage, lastPage, 0u}) !=
              svr::kWorldVoxelInvalidPage,
      "the page at the far world edge resolves a containing parent chain");

  // CPU-only page patch oracle. This is the Phase 4 boundary: it proves the
  // directory can drive legal install/uninstall transactions before those
  // patches are translated into GPU physical ranges.
  svr::WorldVoxelPageDirectory oracleDirectory{};
  const svr::WorldVoxelChunkAddress parentPage{0, 0, 1u};
  const std::vector<svr::WorldVoxelChunkAddress> childCut{
    {0, 0, 0u}, {1, 0, 0u}, {0, 1, 0u}, {1, 1, 0u}};
  check(
      svr::buildWorldVoxelPageDirectory(
          worldExtent, smallSettings, {parentPage}, oracleDirectory, &error),
      "the oracle fixture directory builds");
  svr::assignWorldVoxelPageDirectoryPayloadKeys(
      0x1234u, smallSettings, 0u, oracleDirectory);
  makeEveryPageCached(oracleDirectory);
  check(
      svr::buildWorldVoxelPageDirectoryPatchLists(
          oracleDirectory, &error),
      "terrain page patch lists are generated from directory topology");

  const uint32_t parentPageIndex = oracleDirectory.find(parentPage);
  const auto installOrder =
      svr::planWorldVoxelPageInstalls(oracleDirectory, childCut);
  check(
      !installOrder.empty() &&
          parentBeforeChild(oracleDirectory, installOrder),
      "install planning orders parents before children");
  const auto coldStreamingPlan =
      svr::planWorldVoxelPageStreaming(oracleDirectory, childCut);
  check(
      !coldStreamingPlan.readyInstalls.empty() &&
          std::all_of(
              coldStreamingPlan.readyInstalls.begin(),
              coldStreamingPlan.readyInstalls.end(),
              [&](const uint32_t pageIndex)
              {
                return pageIndex < oracleDirectory.entries.size() &&
                    oracleDirectory.entries[pageIndex].parent ==
                        svr::kWorldVoxelInvalidPage;
              }) &&
          !coldStreamingPlan.blockedInstalls.empty() &&
          coldStreamingPlan.payloadRequests.empty(),
      "stream planning installs cached roots first and blocks descendants");
  auto missingPayloadDirectory = oracleDirectory;
  const uint32_t missingChildIndex =
      missingPayloadDirectory.find(childCut.front());
  if (missingChildIndex != svr::kWorldVoxelInvalidPage)
    missingPayloadDirectory.entries[missingChildIndex].state =
        svr::WorldVoxelPageState::Absent;
  const auto missingPayloadPlan =
      svr::planWorldVoxelPageStreaming(missingPayloadDirectory, childCut);
  check(
      missingChildIndex != svr::kWorldVoxelInvalidPage &&
          std::find(
              missingPayloadPlan.payloadRequests.begin(),
              missingPayloadPlan.payloadRequests.end(),
              missingChildIndex) != missingPayloadPlan.payloadRequests.end() &&
          std::find(
              missingPayloadPlan.blockedInstalls.begin(),
              missingPayloadPlan.blockedInstalls.end(),
              missingChildIndex) != missingPayloadPlan.blockedInstalls.end(),
      "stream planning requests absent payloads before install");
  bool everyPlannedPageHasPatches = std::all_of(
      installOrder.begin(), installOrder.end(),
      [&](const uint32_t pageIndex)
      {
        return pageIndex < oracleDirectory.entries.size() &&
            oracleDirectory.entries[pageIndex].installPatchCount != 0u &&
            oracleDirectory.entries[pageIndex].uninstallPatchCount != 0u;
      });
  check(
      everyPlannedPageHasPatches,
      "every planned terrain page owns install and uninstall patch ranges");
  const bool childPatchInstallsPayloadBeforeParentRewrite =
      childCut.front().spatialLOD == 0u &&
      oracleDirectory.find(childCut.front()) != svr::kWorldVoxelInvalidPage &&
      [&]()
      {
        const uint32_t childIndex = oracleDirectory.find(childCut.front());
        const auto &entry = oracleDirectory.entries[childIndex];
        if (entry.installPatchCount < 3u)
          return false;
        const auto *patches =
            oracleDirectory.installPatches.data() + entry.installPatchOffset;
        return patches[0u].kind ==
            svr::TerrainPagePatchKind::SetPageInstalled &&
            patches[1u].kind ==
            svr::TerrainPagePatchKind::SetStreamingLeaf &&
            patches[2u].kind ==
            svr::TerrainPagePatchKind::SetChildRange &&
            patches[2u].pageIndex == entry.parent;
      }();
  check(
      childPatchInstallsPayloadBeforeParentRewrite,
      "child page installs publish payload before rewriting the parent range");

  svr::TerrainPagePatchBatch staleInstall{};
  check(
      parentPageIndex != svr::kWorldVoxelInvalidPage &&
          svr::prepareWorldVoxelPageInstall(
              oracleDirectory, installOrder.front(), 77u, staleInstall,
              &error) &&
          svr::buildWorldVoxelPageDirectoryPatchLists(
              oracleDirectory, &error) &&
          !svr::commitWorldVoxelPageInstall(
              oracleDirectory, staleInstall, &error),
      "a stale terrain page patch batch is rejected");

  makeEveryPageCached(oracleDirectory);
  svr::buildWorldVoxelPageDirectoryPatchLists(oracleDirectory, &error);
  uint32_t generation = 1u;
  bool installedAncestors = true;
  for (const uint32_t pageIndex : installOrder)
  {
    const auto &address = oracleDirectory.entries[pageIndex].address;
    if (address.spatialLOD == 0u)
      continue;
    installedAncestors &=
        installPage(oracleDirectory, pageIndex, generation++);
  }
  check(
      installedAncestors &&
          svr::validateWorldVoxelPageResidencyOracle(
              oracleDirectory, &error),
      "ancestor page installs keep the CPU residency oracle valid");
  const auto childStreamingPlan =
      svr::planWorldVoxelPageStreaming(oracleDirectory, childCut);
  const bool everyChildReady = std::all_of(
      childCut.begin(), childCut.end(),
      [&](const svr::WorldVoxelChunkAddress &address)
      {
        const uint32_t pageIndex = oracleDirectory.find(address);
        return pageIndex != svr::kWorldVoxelInvalidPage &&
            std::find(
                childStreamingPlan.readyInstalls.begin(),
                childStreamingPlan.readyInstalls.end(),
                pageIndex) != childStreamingPlan.readyInstalls.end();
      });
  check(
      everyChildReady && childStreamingPlan.payloadRequests.empty(),
      "stream planning exposes cached children once the parent is resident");
  const auto ancestorCut =
      svr::selectedWorldVoxelPageCut(oracleDirectory);
  check(
      cutContainsAddress(oracleDirectory, ancestorCut, parentPage),
      "before child pages are resident the selected cut stays on the parent");
  const auto makeRuntimeChunk =
      [&](const svr::WorldVoxelChunkAddress &address,
          const uint32_t pageIndex,
          const uint32_t physicalBase)
  {
    svr::WorldVoxelChunkEntry entry{};
    entry.address = address;
    entry.slot = oracleDirectory.entries[pageIndex].residentSlot;
    entry.generation = oracleDirectory.entries[pageIndex].residencyGeneration;
    entry.residentLOD = oracleDirectory.entries[pageIndex].residentLOD;
    entry.allocation.id = 1000u + physicalBase;
    entry.allocation.nodes = {physicalBase, 3u};
    entry.allocation.children = {physicalBase + 16u, 2u};
    entry.allocation.roots = {physicalBase + 32u, 1u};
    svr::PlanetVoxelChunkSet payload{};
    payload.minimumWorldMetres = {
      static_cast<double>(address.x) * smallSettings.chunkEdgeMetres,
      -4.0,
      static_cast<double>(address.z) * smallSettings.chunkEdgeMetres};
    payload.maximumWorldMetres = {
      payload.minimumWorldMetres[0] + smallSettings.chunkEdgeMetres,
      12.0,
      payload.minimumWorldMetres[2] + smallSettings.chunkEdgeMetres};
    auto &bricks = payload.bricks;
    bricks.nodes.resize(3u);
    bricks.nodes[0].minimumVoxelSize = {
      static_cast<float>(payload.minimumWorldMetres[0]),
      -4.0f,
      static_cast<float>(payload.minimumWorldMetres[2]),
      2.0f};
    bricks.nodes[0].selfError = 1.0f;
    bricks.nodes[0].parentError = 2.0f;
    bricks.nodes[0].childCount = 2u;
    bricks.nodes[0].firstChild = 0u;
    bricks.nodes[1].minimumVoxelSize = {
      bricks.nodes[0].minimumVoxelSize[0],
      -4.0f,
      bricks.nodes[0].minimumVoxelSize[2],
      1.0f};
    bricks.nodes[1].selfError = 0.25f;
    bricks.nodes[1].parentError = 1.0f;
    bricks.nodes[1].parentIndex = 0u;
    bricks.nodes[2] = bricks.nodes[1];
    bricks.nodes[2].minimumVoxelSize[0] +=
        static_cast<float>(svr::kSparseVoxelBrickSide);
    bricks.childIndices = {1u, 2u};
    bricks.rootIndices = {0u};
    bricks.rootCount = 1u;
    entry.chunk = std::make_shared<const svr::PlanetVoxelChunkSet>(std::move(payload));
    return entry;
  };
  svr::WorldVoxelChunkCache runtimeCache{};
  runtimeCache.entries.push_back(
      makeRuntimeChunk(parentPage, parentPageIndex, 64u));
  const auto makePublishedBricks =
      [](const svr::WorldVoxelChunkCache &cache)
      {
        svr::SparseVoxelBrickHierarchy published{};
        for (const auto &entry : cache.entries)
        {
          published.nodes.resize(
              std::max<size_t>(
                  published.nodes.size(),
                  entry.allocation.nodes.offset +
                      entry.chunk->bricks.nodes.size()));
          for (uint32_t node = 0u;
               node < entry.chunk->bricks.nodes.size(); ++node)
            published.nodes[entry.allocation.nodes.offset + node] =
                entry.chunk->bricks.nodes[node];

          published.rootIndices.resize(
              std::max<size_t>(
                  published.rootIndices.size(),
                  entry.allocation.roots.offset +
                      entry.chunk->bricks.rootIndices.size()));
          for (uint32_t root = 0u;
               root < entry.chunk->bricks.rootIndices.size(); ++root)
            published.rootIndices[entry.allocation.roots.offset + root] =
                entry.allocation.nodes.offset +
                entry.chunk->bricks.rootIndices[root];
        }
        published.rootCount =
            static_cast<uint32_t>(published.rootIndices.size());
        return published;
      };
  const auto makeRuntimeRecords =
      [](const svr::WorldVoxelChunkCache &cache)
      {
        std::vector<svr::PlanetVoxelChunkRecord> records;
        records.reserve(cache.entries.size());
        for (const auto &entry : cache.entries)
        {
          svr::PlanetVoxelChunkRecord record{};
          record.clipmapSlot = entry.slot;
          record.residencyGeneration = entry.generation;
          record.firstBrickRoot = entry.allocation.roots.offset;
          record.brickRootCapacity = entry.allocation.roots.capacity;
          record.brickRootCount = static_cast<uint32_t>(
              entry.chunk->bricks.rootIndices.size());
          records.push_back(record);
        }
        return records;
      };
  auto ancestorRuntime =
      svr::buildWorldVoxelTerrainRuntimePageTable(
          oracleDirectory, runtimeCache);
  check(
      svr::validateWorldVoxelTerrainRuntimePageTable(
          oracleDirectory, ancestorRuntime, &error),
      "runtime page table mirrors the ancestor-only directory");
  const bool parentIsStreamingFallback =
      parentPageIndex != svr::kWorldVoxelInvalidPage &&
      (ancestorRuntime.pages[parentPageIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageResident) != 0u &&
      (ancestorRuntime.pages[parentPageIndex].flags &
       svr::kWorldVoxelTerrainRuntimePagePayloadInstalled) != 0u &&
      (ancestorRuntime.pages[parentPageIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageStreamingLeaf) != 0u &&
      (ancestorRuntime.pages[parentPageIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageChildRangeEnabled) == 0u &&
      ancestorRuntime.pages[parentPageIndex].payloadSlot ==
          oracleDirectory.entries[parentPageIndex].residentSlot &&
      ancestorRuntime.pages[parentPageIndex].residencyGeneration ==
          oracleDirectory.entries[parentPageIndex].residencyGeneration;
  check(
      parentIsStreamingFallback,
      "runtime page table exposes the resident parent as streaming fallback");
  const bool parentRuntimeNodesPresent =
      parentPageIndex != svr::kWorldVoxelInvalidPage &&
      ancestorRuntime.pages[parentPageIndex].rootNode !=
          svr::kWorldVoxelInvalidPage &&
      ancestorRuntime.pages[parentPageIndex].nodeCount == 4u &&
      ancestorRuntime
              .nodes[ancestorRuntime.pages[parentPageIndex].rootNode]
              .childCount == 1u &&
      ancestorRuntime
              .nodes[ancestorRuntime.pages[parentPageIndex].rootNode + 1u]
              .childCount == 2u;
  check(
      parentRuntimeNodesPresent,
      "runtime page table attaches the resident page brick hierarchy");
  auto ancestorPublishedBricks = makePublishedBricks(runtimeCache);
  auto ancestorRecords = makeRuntimeRecords(runtimeCache);
  std::vector<uint32_t> ancestorVisibleRoots;
  const std::vector<uint32_t> expectedAncestorRoots = {64u};
  check(
      svr::compactWorldVoxelTerrainRuntimeVisibleBrickRoots(
          ancestorRuntime, ancestorRecords, ancestorPublishedBricks, &error) &&
          ancestorPublishedBricks.rootCount == expectedAncestorRoots.size() &&
          svr::buildWorldVoxelTerrainRuntimeVisibleBrickRoots(
          ancestorRuntime, ancestorPublishedBricks, ancestorVisibleRoots,
          &error) &&
          ancestorVisibleRoots == expectedAncestorRoots,
      "runtime page table derives the parent compact brick root");
  const uint32_t firstChildIndex = oracleDirectory.find(childCut.front());
  const bool cachedChildIsRequestable =
      firstChildIndex != svr::kWorldVoxelInvalidPage &&
      (ancestorRuntime.pages[firstChildIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageRequestable) != 0u &&
      (ancestorRuntime.pages[firstChildIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageResident) == 0u &&
      (ancestorRuntime.pages[firstChildIndex].flags &
       svr::kWorldVoxelTerrainRuntimePagePayloadInstalled) == 0u &&
      ancestorRuntime.pages[firstChildIndex].payloadSlot ==
          svr::kWorldVoxelInvalidPage;
  check(
      cachedChildIsRequestable,
      "runtime page table keeps cached child payload separate from residency");

  bool installedPartialChildren = true;
  for (size_t index = 0u; index + 1u < childCut.size(); ++index)
  {
    const uint32_t childIndex = oracleDirectory.find(childCut[index]);
    installedPartialChildren &=
        childIndex != svr::kWorldVoxelInvalidPage &&
        installPage(oracleDirectory, childIndex, generation++);
  }
  const auto partialCut = svr::selectedWorldVoxelPageCut(oracleDirectory);
  check(
      installedPartialChildren &&
          cutContainsAddress(oracleDirectory, partialCut, parentPage) &&
          !cutContainsAddress(oracleDirectory, partialCut, childCut.front()),
      "an incomplete child group stays hidden behind the streaming parent");
  check(
      parentPageIndex != svr::kWorldVoxelInvalidPage &&
          !oracleDirectory.entries[parentPageIndex].childRangeEnabled,
      "partial child installs do not publish the parent child range");
  auto corruptPartialDirectory = oracleDirectory;
  if (parentPageIndex != svr::kWorldVoxelInvalidPage)
  {
    auto &parentEntry = corruptPartialDirectory.entries[parentPageIndex];
    parentEntry.childRangeEnabled = true;
    parentEntry.residentFirstChildPage = parentEntry.children[0u];
    parentEntry.residentChildCount = parentEntry.childCount;
  }
  check(
      !svr::validateWorldVoxelPageResidencyOracle(
          corruptPartialDirectory, &error),
      "the oracle rejects a child range with a missing sibling page");

  const uint32_t finalChildIndex = oracleDirectory.find(childCut.back());
  check(
      finalChildIndex != svr::kWorldVoxelInvalidPage &&
          installPage(oracleDirectory, finalChildIndex, generation++) &&
          svr::validateWorldVoxelPageResidencyOracle(
              oracleDirectory, &error),
      "installing the last child page keeps the oracle valid");
  const auto refinedCut = svr::selectedWorldVoxelPageCut(oracleDirectory);
  const bool allChildrenSelected = std::all_of(
      childCut.begin(), childCut.end(),
      [&](const svr::WorldVoxelChunkAddress &address)
      { return cutContainsAddress(oracleDirectory, refinedCut, address); });
  check(
      allChildrenSelected &&
          !cutContainsAddress(oracleDirectory, refinedCut, parentPage),
      "a complete child group becomes the selected terrain cut");
  check(
      parentPageIndex != svr::kWorldVoxelInvalidPage &&
          oracleDirectory.entries[parentPageIndex].childRangeEnabled,
      "complete child installs publish the parent child range");
  for (uint32_t child = 0u; child < childCut.size(); ++child)
  {
    const uint32_t childPageIndex = oracleDirectory.find(childCut[child]);
    if (childPageIndex != svr::kWorldVoxelInvalidPage)
      runtimeCache.entries.push_back(
          makeRuntimeChunk(childCut[child], childPageIndex, 128u + child * 64u));
  }
  auto refinedRuntime =
      svr::buildWorldVoxelTerrainRuntimePageTable(
          oracleDirectory, runtimeCache);
  const bool refinedRuntimeValid =
      svr::validateWorldVoxelTerrainRuntimePageTable(
          oracleDirectory, refinedRuntime, &error);
  const bool runtimeParentExposesChildren =
      refinedRuntimeValid &&
      parentPageIndex != svr::kWorldVoxelInvalidPage &&
      (refinedRuntime.pages[parentPageIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageChildRangeEnabled) != 0u &&
      (refinedRuntime.pages[parentPageIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageStreamingLeaf) == 0u &&
      refinedRuntime.pages[parentPageIndex].childCount == childCut.size() &&
      refinedRuntime.pages[parentPageIndex].firstChildPage !=
          svr::kWorldVoxelInvalidPage &&
      refinedRuntime.pages[parentPageIndex].firstChildPage +
              refinedRuntime.pages[parentPageIndex].childCount <=
          refinedRuntime.childPageIndices.size();
  const bool runtimeChildrenMatchDirectory =
      runtimeParentExposesChildren &&
      [&]()
      {
        const auto &runtimeParent = refinedRuntime.pages[parentPageIndex];
        for (uint32_t child = 0u; child != runtimeParent.childCount; ++child)
        {
          const uint32_t childIndex =
              refinedRuntime
                  .childPageIndices[runtimeParent.firstChildPage + child];
          if (childIndex !=
              oracleDirectory.entries[parentPageIndex].children[child])
            return false;
          if ((refinedRuntime.pages[childIndex].flags &
               svr::kWorldVoxelTerrainRuntimePageResident) == 0u)
            return false;
        }
        return true;
      }();
  check(
      refinedRuntimeValid && runtimeChildrenMatchDirectory,
      "runtime page table publishes complete child groups through references");
  const bool childRuntimeNodesPresent =
      runtimeChildrenMatchDirectory &&
      firstChildIndex != svr::kWorldVoxelInvalidPage &&
      refinedRuntime.pages[firstChildIndex].nodeCount == 4u &&
      refinedRuntime.nodes[refinedRuntime.pages[firstChildIndex].rootNode]
              .owningPage == firstChildIndex &&
      (refinedRuntime.nodes[refinedRuntime.pages[firstChildIndex].rootNode]
           .flags &
       svr::kWorldVoxelTerrainRuntimeNodeResident) != 0u;
  check(
      childRuntimeNodesPresent,
      "runtime node spans are owned by their installed child pages");
  auto refinedPublishedBricks = makePublishedBricks(runtimeCache);
  auto refinedRecords = makeRuntimeRecords(runtimeCache);
  std::vector<uint32_t> refinedVisibleRoots;
  std::vector<uint32_t> expectedRefinedRoots;
  for (uint32_t child = 0u; child < childCut.size(); ++child)
    expectedRefinedRoots.push_back(128u + child * 64u);
  check(
      svr::compactWorldVoxelTerrainRuntimeVisibleBrickRoots(
          refinedRuntime, refinedRecords, refinedPublishedBricks, &error) &&
          refinedPublishedBricks.rootCount == expectedRefinedRoots.size() &&
          svr::buildWorldVoxelTerrainRuntimeVisibleBrickRoots(
          refinedRuntime, refinedPublishedBricks, refinedVisibleRoots,
          &error) &&
          refinedVisibleRoots == expectedRefinedRoots,
      "runtime page table derives the refined child compact brick roots");

  svr::TerrainPagePatchBatch blockedUninstall{};
  check(
      !svr::prepareWorldVoxelPageUninstall(
          oracleDirectory, parentPageIndex, blockedUninstall, &error),
      "a parent page cannot uninstall before resident children");

  const auto uninstallOrder =
      svr::planWorldVoxelPageUninstalls(oracleDirectory, {parentPage});
  check(
      uninstallOrder.size() == childCut.size() &&
          childBeforeParent(oracleDirectory, uninstallOrder),
      "uninstall planning orders children before parents");
  bool uninstalledChildren = true;
  for (const uint32_t pageIndex : uninstallOrder)
    uninstalledChildren &= uninstallPage(oracleDirectory, pageIndex);
  const auto restoredCut = svr::selectedWorldVoxelPageCut(oracleDirectory);
  check(
      uninstalledChildren &&
          svr::validateWorldVoxelPageResidencyOracle(
              oracleDirectory, &error) &&
          cutContainsAddress(oracleDirectory, restoredCut, parentPage),
      "uninstalling children restores the parent streaming leaf");
  const auto restoredRuntime =
      svr::buildWorldVoxelTerrainRuntimePageTable(
          oracleDirectory,
          [&]()
          {
            svr::WorldVoxelChunkCache restoredCache{};
            restoredCache.entries.push_back(
                makeRuntimeChunk(parentPage, parentPageIndex, 64u));
            return restoredCache;
          }());
  const bool restoredRuntimeValid =
      svr::validateWorldVoxelTerrainRuntimePageTable(
          oracleDirectory, restoredRuntime, &error);
  const bool runtimeParentRestored =
      restoredRuntimeValid &&
      parentPageIndex != svr::kWorldVoxelInvalidPage &&
      (restoredRuntime.pages[parentPageIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageStreamingLeaf) != 0u &&
      (restoredRuntime.pages[parentPageIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageChildRangeEnabled) == 0u;
  const bool runtimeChildUninstalled =
      restoredRuntimeValid &&
      firstChildIndex != svr::kWorldVoxelInvalidPage &&
      (restoredRuntime.pages[firstChildIndex].flags &
       svr::kWorldVoxelTerrainRuntimePageResident) == 0u &&
      restoredRuntime.pages[firstChildIndex].payloadSlot ==
          svr::kWorldVoxelInvalidPage;
  check(
      runtimeParentRestored && runtimeChildUninstalled,
      "runtime page table follows child uninstall back to parent fallback");

  // The directory has to stay small. It is resident unconditionally, so a size
  // that scales with the finest level rather than the cut would defeat it.
  const size_t directoryBytes =
      directory.entries.size() * sizeof(svr::WorldVoxelPageDirectoryEntry);
  std::cout << "  directory: " << directory.entries.size() << " entries, "
            << directoryBytes / 1024u << " KiB, covered="
            << directory.coveredCount << " frontier=" << directory.frontierCount
            << '\n';
  check(
      directoryBytes < 4u * 1024u * 1024u,
      "a 16 km directory stays under four megabytes");

  return failures == 0 ? 0 : 1;
}
