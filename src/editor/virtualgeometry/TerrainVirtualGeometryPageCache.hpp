#pragma once

#include "editor/virtualgeometry/TerrainTriangleVirtualGeometryBridge.hpp"
#include "editor/virtualgeometry/TerrainVirtualGeometryScratchFile.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace voxel::svraster
{

// Revision-tracked cache of per-chunk terrain virtual geometry assets.
//
// The sample built terrain VG assets once, during scene construction, and never
// again. Terrain chunks refine *in place*: ProgressiveLOD rewrites the same slot
// at a finer internal resolution and bumps the chunk's brickPayloadRevision. So
// after the first refinement the renderer was still drawing the asset built from
// the coarse payload - which reads exactly like "chunks never refine" and, where
// the refined page grew geometry the coarse one did not have, like missing
// faces.
//
// This cache closes that loop: every update compares each chunk's payload
// identity against what was built, rebuilds only what changed, and bounds how
// much rebuilding one update may do so a burst of refinements cannot stall a
// frame.
struct SparseVoxelTerrainVirtualGeometryPageCacheSettings
{
  SparseVoxelOctreeVirtualGeometrySettings octree{};
  virtualgeometry::VirtualGeometryBuildSettings build{};
  virtualgeometry::QuantizationConfig quantization{};
  // Maximum pages rebuilt by one update. Zero rebuilds every stale page, which
  // is what an offline or test caller wants. A rotating cursor guarantees that a
  // bounded caller still reaches every stale page: no page can be starved by a
  // neighbour that keeps changing.
  uint32_t maximumRebuildsPerUpdate = 4u;
  // Optional world-space focus. Stale pages are rebuilt nearest first, so the
  // page the camera is standing on refines before the horizon does.
  bool useFocusPosition = false;
  std::array<double, 3> focusPosition{};
  // Encode each built chunk for the scratch file (phase 8 of
  // docs/terrain-vg-unified-hierarchy-plan.md). The encode runs on the worker
  // that meshed the chunk; the write happens at publication, on whichever
  // thread calls `installChunkBuild`, and only for builds that are published.
  bool writeScratchChunks = false;
};

// One cached page. `generation` counts rebuilds, so a consumer that installed
// `generation` can tell in one comparison whether its copy is current.
struct SparseVoxelTerrainVirtualGeometryPageEntry
{
  uint32_t chunkIndex = 0xffffffffu;
  uint32_t clipmapLOD = 0u;
  uint32_t clipmapSlot = 0xffffffffu;
  uint32_t regionMappingId = 0u;
  uint64_t brickPayloadRevision = 0u;
  uint64_t brickNodeRevision = 0u;
  double voxelSizeMetres = 0.0;
  double edgeMetres = 0.0;
  std::array<double, 3> pageOrigin{};
  std::array<double, 2> worldCenter{};
  virtualgeometry::VirtualGeometryEncodedData asset;
  // The asset's shape, kept out of the asset so a consumer can reason about
  // the chunk - how many pages it owns, how many hierarchy nodes its slice
  // needs, what scale its vertices decode at - without the geometry being
  // resident. Phase 8b serves the geometry itself from the scratch file, and
  // then `asset` is empty on a written chunk while these stay true.
  uint32_t pageCount = 0u;
  uint32_t rootPageIndex = 0u;
  uint32_t hierarchyNodeCount = 0u;
  // The scene's runtime pools are budgeted from a per-chunk average of these
  // two. They are recorded here rather than measured by walking the asset,
  // because the asset is released once the scratch file holds the chunk - and
  // a sizing pass that silently fell back to its 64 KiB floor gave the page
  // buffer 32 MiB instead of 244, which saturates at ~180 pages and freezes
  // the cut at half the resident chunks with no error anywhere.
  uint64_t encodedPageBytes = 0u;
  uint64_t hierarchyBytes = 0u;
  virtualgeometry::QuantizationConfig quantization{};
  SparseVoxelTerrainOctreeVirtualGeometryBuildStats stats;
  uint64_t generation = 0u;
  bool valid = false;
  // This chunk's geometry in the scratch file, or kInvalidChunk when nothing
  // was written. An adoption moves the handle with the entry: the ground and
  // its geometry are the same, only the slot changed.
  uint32_t scratchChunk = TerrainVirtualGeometryScratchFile::kInvalidChunk;
};

struct SparseVoxelTerrainVirtualGeometryPageCacheUpdate
{
  uint32_t inspectedChunks = 0u;
  // Chunks whose payload identity no longer matches what was built.
  uint32_t staleChunks = 0u;
  // Why they were stale: never built, brick payload revision changed, only the
  // brick node revision changed (physical brick relocation with the same
  // payload), or only the slot identity changed (mapping id, LOD, slot).
  uint32_t staleNeverBuilt = 0u;
  uint32_t stalePayloadRevision = 0u;
  uint32_t staleNodeRevisionOnly = 0u;
  uint32_t staleSlotIdentityOnly = 0u;
  // Stale slots served by copying the page another slot already holds for the
  // same ground and payload (the chunk moved slots; its geometry did not).
  uint32_t adoptedChunks = 0u;
  uint32_t rebuiltChunks = 0u;
  // Stale chunks the per-update budget deferred to a later call.
  uint32_t deferredChunks = 0u;
  // Cached pages dropped because the chunk no longer carries bricks.
  uint32_t evictedChunks = 0u;
  uint32_t failedChunks = 0u;
  // Chunks that were already current.
  uint32_t reusedChunks = 0u;
  // Scratch file accounting, all zero unless `writeScratchChunks` is set.
  uint32_t scratchWrittenChunks = 0u;
  uint32_t scratchFailedChunks = 0u;
  uint32_t scratchVerifiedChunks = 0u;
  uint32_t scratchVerifyMismatches = 0u;
  // Chunks whose encoded geometry was released because the file holds it.
  uint32_t droppedAssetChunks = 0u;
  // Phase 8d. Stale chunks served from a retained scratch chunk with the same
  // payload identity instead of a build; chunks whose scratch bytes were
  // retained when their entry died; retained chunks retired to make room for
  // an append under the file budget.
  uint32_t revivedChunks = 0u;
  uint32_t retainedChunks = 0u;
  uint32_t retainedEvictedChunks = 0u;
  double scratchWriteMilliseconds = 0.0;
  std::vector<uint32_t> rebuiltChunkIndices;
  std::vector<uint32_t> evictedChunkIndices;
  double rebuildMilliseconds = 0.0;
  uint32_t rebuiltMeshletCount = 0u;
  uint32_t rebuiltTriangleCount = 0u;
};

// One chunk's meshing work, split out so it can run on a worker.
//
// Everything the build reads is either in the chunk set or in the settings, and
// everything it produces lands here, so `buildChunk` touches no cache state and
// several can run at once. The identity fields are captured at submission and
// re-checked at install: a job whose chunk moved on while it was meshing is
// dropped rather than publishing geometry for terrain that no longer exists.
struct SparseVoxelTerrainVirtualGeometryChunkBuild
{
  uint32_t chunkIndex = 0xffffffffu;
  uint32_t regionMappingId = 0u;
  uint64_t brickPayloadRevision = 0u;
  uint64_t brickNodeRevision = 0u;
  double distanceSquared = 0.0;

  bool completed = false;
  bool success = false;
  std::string error;
  virtualgeometry::VirtualGeometryEncodedData asset;
  std::array<double, 3> pageOrigin{};
  SparseVoxelTerrainOctreeVirtualGeometryBuildStats stats;
  double buildMilliseconds = 0.0;
  // Encoded on the worker when `writeScratchChunks` is set, written at
  // publication. Empty otherwise.
  TerrainVirtualGeometryScratchBlob scratch;
  double scratchEncodeMilliseconds = 0.0;
};

class SparseVoxelTerrainVirtualGeometryPageCache
{
public:
  // Bring the cache in line with `chunks`. Returns false only on a build error;
  // a failed page is left invalid and counted, and the remaining pages still
  // update, so one bad chunk cannot freeze terrain refinement.
  bool update(
      const PlanetVoxelChunkSet &chunks,
      const SparseVoxelTerrainVirtualGeometryPageCacheSettings &settings,
      SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update,
      std::string *error = nullptr);

  // Main thread. Retires entries the chunk set no longer has and returns the
  // stale chunks in priority order, nearest to the focus position first. Does
  // no meshing, so it is cheap enough to run every frame.
  void collectStaleChunks(
      const PlanetVoxelChunkSet &chunks,
      const SparseVoxelTerrainVirtualGeometryPageCacheSettings &settings,
      std::vector<SparseVoxelTerrainVirtualGeometryChunkBuild> &stale,
      SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update);

  // Worker-safe: reads `chunks` and writes only into `build`.
  static void buildChunk(
      const PlanetVoxelChunkSet &chunks,
      const SparseVoxelTerrainVirtualGeometryPageCacheSettings &settings,
      SparseVoxelTerrainVirtualGeometryChunkBuild &build);

  // Main thread. Publishes a finished build, or drops it when the chunk it was
  // meshed from has since been replaced. Returns true when the entry changed.
  bool installChunkBuild(
      const PlanetVoxelChunkSet &chunks,
      SparseVoxelTerrainVirtualGeometryChunkBuild &&build,
      SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update,
      std::string *error = nullptr);

  // The file built chunks are written to. Null (the default) writes nothing
  // and leaves every entry's `scratchChunk` invalid. The cache does not own it;
  // it must outlive the cache.
  // `verifyWrites` reads every page back after writing it and compares it
  // against the asset the encode came from - the phase 8a gate, and far too
  // slow for a shipping run.
  void setScratchFile(TerrainVirtualGeometryScratchFile *file, bool verifyWrites = false)
  {
    scratchFile_ = file;
    verifyScratchWrites_ = verifyWrites;
  }
  // Release a chunk's encoded geometry once the file holds it - the point of
  // phase 8. The entry keeps its shape (page count, hierarchy node count,
  // quantization) so consumers can still reason about the chunk; only the
  // megabytes go. On by default with a file; `--terrain-vg-scratch-keep-assets`
  // holds them for an A/B.
  void setDropWrittenAssets(bool drop) { dropWrittenAssets_ = drop; }
  TerrainVirtualGeometryScratchFile *scratchFile() const { return scratchFile_; }
  // Phase 8d: keep a dead entry's scratch chunk, keyed by its payload
  // identity, so the same ground coming back into the cut is a revive from the
  // file rather than a build. A chunk's payload identity is a content hash
  // mixed with its tile address, so a regenerated chunk with the same content
  // reproduces it. Retained chunks are retired least-recently-retained first
  // when an append needs room under the file budget. On by default with a
  // file; `--terrain-vg-no-scratch-retain` turns it off for an A/B.
  void setRetainRetiredScratchChunks(bool retain) { retainRetired_ = retain; }
  uint32_t retainedChunkCount() const { return static_cast<uint32_t>(retained_.size()); }
  uint64_t retainedChunkBytes() const { return retainedBytes_; }

  const SparseVoxelTerrainVirtualGeometryPageEntry *find(uint32_t chunkIndex) const;
  // Takes the pages displaced since the last call, for release off-thread.
  std::vector<SparseVoxelTerrainVirtualGeometryPageEntry> takePendingRelease()
  {
    return std::move(pendingRelease_);
  }

  const std::vector<SparseVoxelTerrainVirtualGeometryPageEntry> &entries() const
  {
    return entries_;
  }

  // True when no chunk in `chunks` differs from what the cache holds.
  bool isCurrent(const PlanetVoxelChunkSet &chunks) const;

  uint32_t validPageCount() const;
  uint64_t totalGenerations() const { return totalGenerations_; }
  void clear();

private:
  // Retires an entry's scratch chunk, so the bytes of a chunk that no longer
  // exists become reusable and a handle can never resolve to another chunk's
  // geometry. Idempotent: the handle is invalidated in the entry.
  void releaseScratchChunk(SparseVoxelTerrainVirtualGeometryPageEntry &entry);

  // Writes an entry's encoded blob and records the handle. The entry's
  // previous scratch chunk, if any, must have been retired by the caller.
  // Returns false when nothing was written.
  bool writeScratchChunk(
      SparseVoxelTerrainVirtualGeometryPageEntry &entry,
      SparseVoxelTerrainVirtualGeometryChunkBuild &build,
      SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update);

  // Phase 8d. The payload identity a scratch chunk is retained under: the
  // ground's tile and the content hash of what was generated there, which is
  // what `sparseVoxelTerrainVirtualGeometryPageIsStale` compares first.
  struct PayloadIdentity
  {
    uint32_t regionMappingId = 0u;
    uint32_t clipmapLOD = 0u;
    uint64_t brickPayloadRevision = 0u;

    bool operator==(const PayloadIdentity &other) const
    {
      return regionMappingId == other.regionMappingId &&
          clipmapLOD == other.clipmapLOD &&
          brickPayloadRevision == other.brickPayloadRevision;
    }
  };
  struct PayloadIdentityHash
  {
    size_t operator()(const PayloadIdentity &key) const noexcept;
  };
  struct RetainedChunk
  {
    // The entry as it died: its shape, its build stats (the parent republish
    // needs the cluster boxes) and its scratch handle. Never its asset.
    SparseVoxelTerrainVirtualGeometryPageEntry entry;
    uint64_t retainedAt = 0u;
    uint64_t bytes = 0u;
  };
  // Retires a dead entry's scratch bytes: retained under its identity when
  // retention is on and the file holds it, released otherwise. The entry's
  // asset, if any, goes to `pendingRelease_` either way, and the entry is
  // left cleared.
  void retireEntry(
      SparseVoxelTerrainVirtualGeometryPageEntry &entry,
      SparseVoxelTerrainVirtualGeometryPageCacheUpdate *update);
  // Serves a stale slot from a retained chunk with the record's identity.
  bool reviveFromRetained(
      const PlanetVoxelChunkSet &chunks,
      uint32_t chunkIndex,
      SparseVoxelTerrainVirtualGeometryPageCacheUpdate &update);
  // Releases the least recently retained chunk. False when none is retained.
  bool evictOldestRetained(SparseVoxelTerrainVirtualGeometryPageCacheUpdate *update);
  void releaseAllRetained();

  TerrainVirtualGeometryScratchFile *scratchFile_ = nullptr;
  bool verifyScratchWrites_ = false;
  bool dropWrittenAssets_ = true;
  bool retainRetired_ = true;
  std::unordered_map<PayloadIdentity, RetainedChunk, PayloadIdentityHash> retained_;
  uint64_t retainedBytes_ = 0u;
  uint64_t retainTick_ = 0u;
  // Pages displaced by a slot adoption or an eviction. An entry owns its
  // encoded geometry, so releasing one is a multi-megabyte free; 119 of them
  // in a single refresh cost 43 ms on the render thread. The caller drains
  // this and destroys it wherever it likes - the work is unavoidable, being on
  // the render thread is not.
  std::vector<SparseVoxelTerrainVirtualGeometryPageEntry> pendingRelease_;
  std::vector<SparseVoxelTerrainVirtualGeometryPageEntry> entries_;
  // Round-robin start for bounded updates.
  uint32_t cursor_ = 0u;
  uint64_t totalGenerations_ = 0u;
};

// True when the cached entry no longer describes the chunk's current payload.
bool sparseVoxelTerrainVirtualGeometryPageIsStale(
    const PlanetVoxelChunkRecord &record,
    const SparseVoxelTerrainVirtualGeometryPageEntry &entry);

} // namespace voxel::svraster
