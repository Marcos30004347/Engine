#include "editor/virtualgeometry/TerrainVirtualGeometryPageCache.hpp"

#include "virtualgeometry/VirtualGeometryFile.hpp"

#include "os/Logger.hpp"
#include <cstdlib>
#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace voxel::svraster
{
namespace
{

bool sameDouble(const double left, const double right)
{
  const double scale = std::max(1.0, std::max(std::fabs(left), std::fabs(right)));
  return std::fabs(left - right) <= scale * 1e-12;
}

bool chunkHasGeometry(const PlanetVoxelChunkRecord &record)
{
  return record.brickNodeCapacity != 0u && record.voxelSizeMetres > 0.0 &&
      record.edgeMetres > 0.0;
}

} // namespace

bool sparseVoxelTerrainVirtualGeometryPageIsStale(
    const PlanetVoxelChunkRecord &record,
    const SparseVoxelTerrainVirtualGeometryPageEntry &entry)
{
  if (!entry.valid)
    return true;
  // Payload identity first: this is the word the terrain builder bumps when it
  // rewrites a slot at a finer resolution.
  if (entry.brickPayloadRevision != record.brickPayloadRevision ||
      entry.brickNodeRevision != record.brickNodeRevision)
    return true;
  // Then slot identity: a toroidal grid can remap a slot onto a different tile
  // without touching the payload of the tile that left.
  if (entry.regionMappingId != record.regionMappingId ||
      entry.clipmapLOD != record.clipmapLOD ||
      entry.clipmapSlot != record.clipmapSlot)
    return true;
  return !sameDouble(entry.voxelSizeMetres, record.voxelSizeMetres) ||
      !sameDouble(entry.edgeMetres, record.edgeMetres) ||
      !sameDouble(entry.worldCenter[0], record.worldCenterXMetres) ||
      !sameDouble(entry.worldCenter[1], record.worldCenterZMetres);
}

const SparseVoxelTerrainVirtualGeometryPageEntry *
SparseVoxelTerrainVirtualGeometryPageCache::find(const uint32_t chunkIndex) const
{
  if (chunkIndex >= entries_.size() || !entries_[chunkIndex].valid)
    return nullptr;
  return &entries_[chunkIndex];
}

bool SparseVoxelTerrainVirtualGeometryPageCache::isCurrent(
    const PlanetVoxelChunkSet &chunks) const
{
  if (entries_.size() < chunks.chunks.size())
    return false;
  for (uint32_t chunkIndex = 0u;
       chunkIndex < static_cast<uint32_t>(chunks.chunks.size());
       ++chunkIndex)
  {
    const auto &record = chunks.chunks[chunkIndex];
    if (!chunkHasGeometry(record))
    {
      if (entries_[chunkIndex].valid)
        return false;
      continue;
    }
    if (sparseVoxelTerrainVirtualGeometryPageIsStale(record, entries_[chunkIndex]))
      return false;
  }
  return true;
}

uint32_t SparseVoxelTerrainVirtualGeometryPageCache::validPageCount() const
{
  uint32_t count = 0u;
  for (const auto &entry : entries_)
    count += entry.valid ? 1u : 0u;
  return count;
}

void SparseVoxelTerrainVirtualGeometryPageCache::releaseScratchChunk(
    SparseVoxelTerrainVirtualGeometryPageEntry &entry)
{
  if (entry.scratchChunk == TerrainVirtualGeometryScratchFile::kInvalidChunk)
    return;
  if (scratchFile_ != nullptr)
    scratchFile_->releaseChunk(entry.scratchChunk);
  entry.scratchChunk = TerrainVirtualGeometryScratchFile::kInvalidChunk;
}

size_t SparseVoxelTerrainVirtualGeometryPageCache::PayloadIdentityHash::operator()(
    const PayloadIdentity &key) const noexcept
{
  uint64_t hash = 1469598103934665603ull;
  const auto mix = [&hash](const uint64_t value)
  {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(key.regionMappingId);
  mix(key.clipmapLOD);
  mix(key.brickPayloadRevision);
  return static_cast<size_t>(hash);
}

void SparseVoxelTerrainVirtualGeometryPageCache::retireEntry(
    SparseVoxelTerrainVirtualGeometryPageEntry &entry,
    SparseVoxelTerrainVirtualGeometryPageCacheUpdate *update)
{
  // The asset is the multi-megabyte part and it is freed off-thread; nothing
  // retained ever carries one.
  if (!entry.asset.pages.empty() || !entry.asset.hierarchy.empty())
  {
    SparseVoxelTerrainVirtualGeometryPageEntry shell{};
    shell.asset = std::move(entry.asset);
    entry.asset = virtualgeometry::VirtualGeometryEncodedData{};
    pendingRelease_.push_back(std::move(shell));
  }
  const bool retain = retainRetired_ && scratchFile_ != nullptr && entry.valid &&
      entry.scratchChunk != TerrainVirtualGeometryScratchFile::kInvalidChunk &&
      scratchFile_->isLive(entry.scratchChunk);
  if (!retain)
  {
    releaseScratchChunk(entry);
    entry = {};
    return;
  }
  const PayloadIdentity identity{
    entry.regionMappingId, entry.clipmapLOD, entry.brickPayloadRevision};
  // Same identity, same geometry: the newer copy wins and the older one's
  // bytes go back to the file.
  const auto existing = retained_.find(identity);
  if (existing != retained_.end())
  {
    retainedBytes_ -= std::min(retainedBytes_, existing->second.bytes);
    releaseScratchChunk(existing->second.entry);
    retained_.erase(existing);
  }
  RetainedChunk retained{};
  retained.bytes = scratchFile_->chunkBytesOf(entry.scratchChunk);
  retained.retainedAt = ++retainTick_;
  retained.entry = std::move(entry);
  retained.entry.valid = false;
  retainedBytes_ += retained.bytes;
  retained_.emplace(identity, std::move(retained));
  entry = {};
  if (update != nullptr)
    ++update->retainedChunks;
}

bool SparseVoxelTerrainVirtualGeometryPageCache::reviveFromRetained(
    const PlanetVoxelChunkSet &chunks,
    const uint32_t chunkIndex,
    SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update)
{
  if (retained_.empty() || scratchFile_ == nullptr || chunkIndex >= chunks.chunks.size())
    return false;
  const auto &record = chunks.chunks[chunkIndex];
  const auto found = retained_.find(
      PayloadIdentity{record.regionMappingId, record.clipmapLOD, record.brickPayloadRevision});
  if (found == retained_.end())
    return false;
  RetainedChunk retained = std::move(found->second);
  retained_.erase(found);
  retainedBytes_ -= std::min(retainedBytes_, retained.bytes);
  // A handle the file no longer holds is nothing to revive from.
  if (!scratchFile_->isLive(retained.entry.scratchChunk))
    return false;
  auto &entry = entries_[chunkIndex];
  const uint64_t previousGeneration = entry.generation;
  if (entry.valid)
    retireEntry(entry, &update);
  entry = std::move(retained.entry);
  entry.chunkIndex = chunkIndex;
  entry.clipmapSlot = record.clipmapSlot;
  entry.brickNodeRevision = record.brickNodeRevision;
  // The slot-derived fields are the record's, exactly as a build would set
  // them; the geometry is in the object's space and does not depend on them.
  entry.voxelSizeMetres = record.voxelSizeMetres;
  entry.edgeMetres = record.edgeMetres;
  entry.worldCenter = {record.worldCenterXMetres, record.worldCenterZMetres};
  entry.pageOrigin = {
    record.worldCenterXMetres - record.edgeMetres * 0.5,
    record.elevationOriginMetres - chunks.elevationOriginMetres,
    record.worldCenterZMetres - record.edgeMetres * 0.5};
  entry.generation = previousGeneration + 1u;
  entry.valid = true;
  ++totalGenerations_;
  ++update.revivedChunks;
  return true;
}

bool SparseVoxelTerrainVirtualGeometryPageCache::evictOldestRetained(
    SparseVoxelTerrainVirtualGeometryPageCacheUpdate *update)
{
  if (retained_.empty())
    return false;
  auto oldest = retained_.begin();
  for (auto it = retained_.begin(); it != retained_.end(); ++it)
    if (it->second.retainedAt < oldest->second.retainedAt)
      oldest = it;
  retainedBytes_ -= std::min(retainedBytes_, oldest->second.bytes);
  releaseScratchChunk(oldest->second.entry);
  retained_.erase(oldest);
  if (update != nullptr)
    ++update->retainedEvictedChunks;
  return true;
}

void SparseVoxelTerrainVirtualGeometryPageCache::releaseAllRetained()
{
  for (auto &retained : retained_)
    releaseScratchChunk(retained.second.entry);
  retained_.clear();
  retainedBytes_ = 0u;
}

void SparseVoxelTerrainVirtualGeometryPageCache::clear()
{
  for (auto &entry : entries_)
    releaseScratchChunk(entry);
  for (auto &entry : pendingRelease_)
    releaseScratchChunk(entry);
  releaseAllRetained();
  entries_.clear();
  cursor_ = 0u;
  totalGenerations_ = 0u;
}

void SparseVoxelTerrainVirtualGeometryPageCache::collectStaleChunks(
    const PlanetVoxelChunkSet &chunks,
    const SparseVoxelTerrainVirtualGeometryPageCacheSettings &settings,
    std::vector<SparseVoxelTerrainVirtualGeometryChunkBuild> &stale,
    SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update)
{
  stale.clear();

  const uint32_t chunkCount = static_cast<uint32_t>(chunks.chunks.size());
  // A shrinking chunk set drops its tail entries outright.
  for (uint32_t chunkIndex = chunkCount;
       chunkIndex < static_cast<uint32_t>(entries_.size());
       ++chunkIndex)
    if (entries_[chunkIndex].valid)
    {
      // The resize below destroys these outright, so their scratch bytes have
      // to be retired here or the file grows by every chunk the set ever shed.
      retireEntry(entries_[chunkIndex], &update);
      update.evictedChunkIndices.push_back(chunkIndex);
      ++update.evictedChunks;
    }
  auto adoptDetectEnd = std::chrono::steady_clock::now();
  size_t adoptionCount = 0u;
  const auto staleT0 = std::chrono::steady_clock::now();
  entries_.resize(chunkCount);
  const auto staleT1 = std::chrono::steady_clock::now();
  update.inspectedChunks = chunkCount;
  if (chunkCount == 0u)
    return;

  // The streamer moves chunks between slots as the cut changes; a slot-keyed
  // cache then sees a "new" chunk in the slot and re-meshes geometry it already
  // holds elsewhere (786 of 944 rebuilds on the slope test were an unchanged
  // payload in a new slot). The page depends on the ground and the brick
  // payload only - never on the physical brick offsets the node revision
  // hashes - so an entry for the same ground and payload is this slot's page
  // too. Sources are resolved against the entries as they were before any
  // adoption: two chunks swapping slots each adopt the other's page, and
  // writing one in place first would lose the other.
  {
    // Indexed by the page identity rather than scanned. The search used to be
    // a linear pass over every entry for every stale chunk, which is quadratic
    // in resident pages and showed up as tens of milliseconds on the main
    // thread once residency passed a few hundred.
    struct AdoptionKey
    {
      uint32_t regionMappingId;
      uint32_t clipmapLOD;
      uint64_t brickPayloadRevision;

      bool operator==(const AdoptionKey &other) const
      {
        return regionMappingId == other.regionMappingId &&
            clipmapLOD == other.clipmapLOD &&
            brickPayloadRevision == other.brickPayloadRevision;
      }
    };
    struct AdoptionKeyHash
    {
      size_t operator()(const AdoptionKey &key) const
      {
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](const uint64_t value)
        {
          hash ^= value;
          hash *= 1099511628211ull;
        };
        mix(key.regionMappingId);
        mix(key.clipmapLOD);
        mix(key.brickPayloadRevision);
        return static_cast<size_t>(hash);
      }
    };
    std::unordered_map<AdoptionKey, uint32_t, AdoptionKeyHash> entryByIdentity;
    entryByIdentity.reserve(chunkCount);
    for (uint32_t other = 0u; other < chunkCount; ++other)
    {
      const auto &candidate = entries_[other];
      if (!candidate.valid)
        continue;
      entryByIdentity.emplace(
          AdoptionKey{
            candidate.regionMappingId, candidate.clipmapLOD,
            candidate.brickPayloadRevision},
          other);
    }
    std::vector<std::pair<uint32_t, uint32_t>> adoptions; // slot, source
    for (uint32_t chunkIndex = 0u; chunkIndex < chunkCount; ++chunkIndex)
    {
      const auto &record = chunks.chunks[chunkIndex];
      if (!chunkHasGeometry(record) ||
          !sparseVoxelTerrainVirtualGeometryPageIsStale(record, entries_[chunkIndex]))
        continue;
      const auto found = entryByIdentity.find(
          AdoptionKey{
            record.regionMappingId, record.clipmapLOD,
            record.brickPayloadRevision});
      if (found == entryByIdentity.end() || found->second == chunkIndex)
        continue;
      adoptions.emplace_back(chunkIndex, found->second);
    }
    // Moved, not copied. An entry owns its encoded page, so copying one here
    // duplicated megabytes of geometry per adoption and made this the slowest
    // thing on the main thread - up to 1568 ms in a single refresh. Moving is
    // also the correct semantics: `regionMappingId` is the ground's identity
    // and is unique, so a source matches exactly one destination, and the page
    // belongs to the ground that is now at that destination slot. The source
    // slot is cleared because it now holds different ground, whose own entry is
    // stale and is rebuilt or adopted below.
    adoptDetectEnd = std::chrono::steady_clock::now();
    std::vector<SparseVoxelTerrainVirtualGeometryPageEntry> sources;
    sources.reserve(adoptions.size());
    for (const auto &[slot, source] : adoptions)
    {
      sources.push_back(std::move(entries_[source]));
      entries_[source] = {};
    }
    for (size_t index = 0u; index < adoptions.size(); ++index)
    {
      const uint32_t slot = adoptions[index].first;
      const auto &record = chunks.chunks[slot];
      SparseVoxelTerrainVirtualGeometryPageEntry adopted = std::move(sources[index]);
      adopted.chunkIndex = slot;
      adopted.clipmapSlot = record.clipmapSlot;
      adopted.brickNodeRevision = record.brickNodeRevision;
      if (entries_[slot].valid)
        retireEntry(entries_[slot], &update);
      entries_[slot] = std::move(adopted);
      ++update.adoptedChunks;
      ++adoptionCount;
    }
  }

  const auto staleT2 = std::chrono::steady_clock::now();
  for (uint32_t offset = 0u; offset < chunkCount; ++offset)
  {
    // Start from the rotating cursor so a bounded budget still walks the whole
    // set; without it the first few chunks would monopolise every update.
    const uint32_t chunkIndex = (cursor_ + offset) % chunkCount;
    const auto &record = chunks.chunks[chunkIndex];
    auto &entry = entries_[chunkIndex];

    if (!chunkHasGeometry(record))
    {
      if (entry.valid)
      {
        retireEntry(entry, &update);
        update.evictedChunkIndices.push_back(chunkIndex);
        ++update.evictedChunks;
      }
      continue;
    }

    if (!sparseVoxelTerrainVirtualGeometryPageIsStale(record, entry))
    {
      ++update.reusedChunks;
      continue;
    }

    // Phase 8d: the same ground with the same content, seen before and still
    // in the file, is a revive rather than a build.
    if (reviveFromRetained(chunks, chunkIndex, update))
      continue;

    ++update.staleChunks;
    if (!entry.valid)
      ++update.staleNeverBuilt;
    else if (entry.brickPayloadRevision != record.brickPayloadRevision)
      ++update.stalePayloadRevision;
    else if (entry.brickNodeRevision != record.brickNodeRevision)
      ++update.staleNodeRevisionOnly;
    else
      ++update.staleSlotIdentityOnly;
    SparseVoxelTerrainVirtualGeometryChunkBuild candidate{};
    candidate.chunkIndex = chunkIndex;
    candidate.regionMappingId = record.regionMappingId;
    candidate.brickPayloadRevision = record.brickPayloadRevision;
    candidate.brickNodeRevision = record.brickNodeRevision;
    if (settings.useFocusPosition)
    {
      const double dx = record.worldCenterXMetres - settings.focusPosition[0];
      const double dz = record.worldCenterZMetres - settings.focusPosition[2];
      candidate.distanceSquared = dx * dx + dz * dz;
    }
    stale.push_back(std::move(candidate));
  }

  const auto staleT3 = std::chrono::steady_clock::now();
  if (settings.useFocusPosition)
    std::stable_sort(
        stale.begin(),
        stale.end(),
        [](const SparseVoxelTerrainVirtualGeometryChunkBuild &left,
           const SparseVoxelTerrainVirtualGeometryChunkBuild &right)
        { return left.distanceSquared < right.distanceSquared; });
  if (std::getenv("SVR_TRACE_TERRAIN") != nullptr)
  {
    const auto ms = [](const auto begin, const auto end)
    { return std::chrono::duration<double, std::milli>(end - begin).count(); };
    os::Logger::logf(
        "Terrain VG stale: resize=%.1f detect=%.1f apply=%.1f scan=%.1f "
        "sort=%.1f chunks=%u adoptions=%zu",
        ms(staleT0, staleT1), ms(staleT1, adoptDetectEnd),
        ms(adoptDetectEnd, staleT2), ms(staleT2, staleT3),
        ms(staleT3, std::chrono::steady_clock::now()), chunkCount,
        adoptionCount);
  }
}

void SparseVoxelTerrainVirtualGeometryPageCache::buildChunk(
    const PlanetVoxelChunkSet &chunks,
    const SparseVoxelTerrainVirtualGeometryPageCacheSettings &settings,
    SparseVoxelTerrainVirtualGeometryChunkBuild &build)
{
  const auto start = std::chrono::steady_clock::now();
  build.completed = true;
  build.success = false;
  build.error.clear();
  if (build.chunkIndex >= chunks.chunks.size())
  {
    build.error = "terrain chunk index is out of range";
    return;
  }

  // One runtime object holds every chunk (absoluteOrigin): a chunk's finest
  // clusters carry its voxel size as their error, so the culler can reject
  // them where a resident finer chunk draws. The frontier keeps drawing through
  // the streaming-leaf flag residency sets there. Per-object assets keep the
  // LOD0 convention (zero), which is what makes a lone asset always renderable.
  auto octree = settings.octree;
  octree.leafSelfError = octree.absoluteOrigin
      ? static_cast<float>(chunks.chunks[build.chunkIndex].voxelSizeMetres)
      : 0.0f;
  // Diagnostic A/B: SVR_VG_VANISHING_CONSUME=1 restores the old behaviour of
  // consuming children whose parent block carries no surface.
  if (const char *vanishing = std::getenv("SVR_VG_VANISHING_CONSUME"))
    octree.consumeVanishingGroups = std::atoi(vanishing) != 0;
  // Diagnostic A/B: SVR_VG_LEAF_ERROR=0 restores the LOD0 convention.
  if (const char *leafOverride = std::getenv("SVR_VG_LEAF_ERROR"))
    octree.leafSelfError = static_cast<float>(std::atof(leafOverride));
  // Each terrain chunk must retain a complete internal LOD hierarchy. The
  // resident child cut replaces its parent's covered clusters at runtime;
  // clamping this asset to [voxel, 2*voxel) made it disappear whenever the
  // camera needed a coarser page and no active parent covered that interval.
  // The generic builder still supports a finite band for explicitly linked
  // assets, but terrain's independently generated chunks have no such edge.
  build.success = buildSparseVoxelTerrainOctreeVirtualGeometryAsset(
      chunks,
      build.chunkIndex,
      octree,
      settings.build,
      settings.quantization,
      build.asset,
      &build.pageOrigin,
      &build.stats,
      &build.error);
  build.buildMilliseconds =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start)
          .count();

  // Encoding the chunk for the scratch file is the expensive half of writing
  // it (PageBuffer::encode plus LZ4 over a few hundred KB), and it needs
  // nothing but the asset - so it belongs on the worker that just meshed it,
  // not on the thread that publishes it.
  if (settings.writeScratchChunks && build.success && !build.asset.pages.empty())
  {
    const auto encodeStart = std::chrono::steady_clock::now();
    std::string scratchError;
    if (!TerrainVirtualGeometryScratchFile::encodeChunk(
            build.asset, build.scratch, &scratchError))
    {
      // Not a build failure: the in-memory asset is still good, so the chunk
      // draws. The publish counts it and the run says so.
      build.scratch.clear();
    }
    build.scratchEncodeMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - encodeStart)
            .count();
  }
}

bool SparseVoxelTerrainVirtualGeometryPageCache::writeScratchChunk(
    SparseVoxelTerrainVirtualGeometryPageEntry &entry,
    SparseVoxelTerrainVirtualGeometryChunkBuild &build,
    SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update)
{
  if (scratchFile_ == nullptr || build.scratch.empty())
    return false;
  const auto start = std::chrono::steady_clock::now();
  // The entry's previous chunk was retired by the caller under its own
  // identity; a handle still here would be a leak of the file.
  releaseScratchChunk(entry);
  // Phase 8d: under the file budget, retained chunks are what gives way. A
  // live chunk is never touched, so a budget below the live working set still
  // degrades to a refused append and an asset kept in memory.
  const uint64_t bytes = build.scratch.bytes.size();
  while (!scratchFile_->canAppend(bytes) && evictOldestRetained(&update))
  {
  }
  uint32_t handle = TerrainVirtualGeometryScratchFile::kInvalidChunk;
  std::string error;
  const bool written = scratchFile_->appendEncodedChunk(build.scratch, handle, &error);
  if (!written)
  {
    ++update.scratchFailedChunks;
    return false;
  }
  entry.scratchChunk = handle;
  ++update.scratchWrittenChunks;
  update.scratchWriteMilliseconds +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start)
          .count();

  if (!verifyScratchWrites_)
    return true;

  // The 8a gate: every page read back must be the words the install path would
  // have taken from the asset. A dropped or reordered word here installs
  // cleanly and draws nothing.
  ++update.scratchVerifiedChunks;
  TerrainVirtualGeometryScratchChunk header;
  if (!scratchFile_->readChunk(handle, header, &error) ||
      header.pageCount != entry.asset.pages.size() ||
      header.rootPageIndex != entry.asset.rootPageIndex ||
      header.hierarchy.size() != entry.asset.hierarchy.size() ||
      header.quantization.unit_scale != entry.asset.quantizationConfig.unit_scale)
  {
    ++update.scratchVerifyMismatches;
    return true;
  }
  for (size_t node = 0u; node < header.hierarchy.size(); ++node)
  {
    if (std::memcmp(
            &header.hierarchy[node], &entry.asset.hierarchy[node],
            sizeof(virtualgeometry::VirtualGeometryHierarchy)) != 0)
    {
      ++update.scratchVerifyMismatches;
      return true;
    }
  }
  for (size_t pageIndex = 0u; pageIndex < entry.asset.pages.size(); ++pageIndex)
  {
    TerrainVirtualGeometryScratchPage page;
    if (!scratchFile_->readPage(handle, static_cast<uint32_t>(pageIndex), page, &error))
    {
      ++update.scratchVerifyMismatches;
      return true;
    }
    const virtualgeometry::PageBuffer encoded =
        virtualgeometry::PageBuffer::encode(entry.asset.pages[pageIndex]);
    if (page.encodedWords != encoded.data ||
        page.dependencies != entry.asset.pages[pageIndex].dependencies ||
        page.installUpdates.size() !=
            entry.asset.pages[pageIndex].installUpdates.hierarchyUpdates.size())
    {
      ++update.scratchVerifyMismatches;
      return true;
    }
  }
  return true;
}

bool SparseVoxelTerrainVirtualGeometryPageCache::installChunkBuild(
    const PlanetVoxelChunkSet &chunks,
    SparseVoxelTerrainVirtualGeometryChunkBuild &&build,
    SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update,
    std::string *error)
{
  const uint32_t chunkIndex = build.chunkIndex;
  if (chunkIndex >= chunks.chunks.size() || chunkIndex >= entries_.size())
    return false;
  const auto &record = chunks.chunks[chunkIndex];
  // The chunk this was meshed from may have been replaced while the job ran.
  // Publishing it anyway would draw terrain that no longer exists; the next
  // collect pass sees the slot as stale again and re-submits it.
  if (record.regionMappingId != build.regionMappingId ||
      record.brickPayloadRevision != build.brickPayloadRevision ||
      record.brickNodeRevision != build.brickNodeRevision)
  {
    return false;
  }

  auto &entry = entries_[chunkIndex];
  const uint64_t previousGeneration = entry.generation;
  cursor_ = (chunkIndex + 1u) % static_cast<uint32_t>(chunks.chunks.size());
  update.rebuildMilliseconds += build.buildMilliseconds;

  if (!build.success || build.asset.pages.empty())
  {
    // Drop the stale payload rather than keep serving it: a page that failed
    // to rebuild must not keep drawing geometry the chunk no longer has. Its
    // bytes are still that older payload's geometry, and are retained as such.
    retireEntry(entry, &update);
    entry.chunkIndex = chunkIndex;
    ++update.failedChunks;
    if (!build.success && error != nullptr && error->empty())
    {
      *error = "terrain chunk " + std::to_string(chunkIndex) +
          " virtual geometry rebuild failed: " + build.error;
    }
    return true;
  }

  // Whatever this slot held is a different payload now (that is why it was
  // stale), and its bytes are that payload's geometry: retired under the old
  // identity before the entry is rewritten with the new one.
  if (entry.valid)
    retireEntry(entry, &update);
  entry.chunkIndex = chunkIndex;
  entry.clipmapLOD = record.clipmapLOD;
  entry.clipmapSlot = record.clipmapSlot;
  entry.regionMappingId = record.regionMappingId;
  entry.brickPayloadRevision = record.brickPayloadRevision;
  entry.brickNodeRevision = record.brickNodeRevision;
  entry.voxelSizeMetres = record.voxelSizeMetres;
  entry.edgeMetres = record.edgeMetres;
  entry.pageOrigin = build.pageOrigin;
  entry.worldCenter = {record.worldCenterXMetres, record.worldCenterZMetres};
  entry.pageCount = static_cast<uint32_t>(build.asset.pages.size());
  entry.rootPageIndex = build.asset.rootPageIndex;
  entry.hierarchyNodeCount = static_cast<uint32_t>(build.asset.hierarchy.size());
  entry.quantization = build.asset.quantizationConfig;
  entry.hierarchyBytes = static_cast<uint64_t>(build.asset.hierarchy.size()) *
      sizeof(virtualgeometry::VirtualGeometryHierarchy);
  entry.encodedPageBytes = 0u;
  for (const auto &page : build.asset.pages)
  {
    const auto &buffers = page.vertexBuffers;
    entry.encodedPageBytes +=
        static_cast<uint64_t>(buffers.positions.size()) * sizeof(uint32_t);
    entry.encodedPageBytes += buffers.indices.size();
    entry.encodedPageBytes +=
        static_cast<uint64_t>(buffers.normals.size()) * sizeof(uint32_t);
    entry.encodedPageBytes +=
        static_cast<uint64_t>(buffers.uvs.size()) * sizeof(uint32_t);
    entry.encodedPageBytes +=
        static_cast<uint64_t>(buffers.boneWeights.size()) * sizeof(uint32_t);
  }
  entry.asset = std::move(build.asset);
  entry.stats = std::move(build.stats);
  entry.generation = previousGeneration + 1u;
  entry.valid = true;
  // The write reads `entry.asset` to verify, so it happens before the drop.
  if (writeScratchChunk(entry, build, update) && dropWrittenAssets_)
  {
    // The file holds this chunk now. Releasing the asset here is the whole
    // point of phase 8: it is ~9 MiB of CPU per resident chunk against ~179 KB
    // on disk. The shape the consumers need was copied out above.
    entry.asset = virtualgeometry::VirtualGeometryEncodedData{};
    ++update.droppedAssetChunks;
  }

  ++totalGenerations_;
  ++update.rebuiltChunks;
  update.rebuiltChunkIndices.push_back(chunkIndex);
  update.rebuiltMeshletCount += entry.stats.encodedMeshletCount;
  update.rebuiltTriangleCount += entry.stats.build.triangleCount;
  return true;
}

bool SparseVoxelTerrainVirtualGeometryPageCache::update(
    const PlanetVoxelChunkSet &chunks,
    const SparseVoxelTerrainVirtualGeometryPageCacheSettings &settings,
    SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update,
    std::string *error)
{
  update = {};
  if (error != nullptr)
    error->clear();

  std::vector<SparseVoxelTerrainVirtualGeometryChunkBuild> stale;
  collectStaleChunks(chunks, settings, stale, update);

  const uint32_t budget = settings.maximumRebuildsPerUpdate == 0u
      ? static_cast<uint32_t>(stale.size())
      : settings.maximumRebuildsPerUpdate;

  bool anyFailed = false;
  uint32_t rebuilt = 0u;
  for (auto &candidate : stale)
  {
    if (rebuilt >= budget)
    {
      ++update.deferredChunks;
      continue;
    }
    ++rebuilt;
    buildChunk(chunks, settings, candidate);
    const bool built = candidate.success;
    installChunkBuild(chunks, std::move(candidate), update, error);
    anyFailed = anyFailed || !built;
  }

  return !anyFailed;
}

} // namespace voxel::svraster
