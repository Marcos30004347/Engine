#pragma once

#include "virtualgeometry/VirtualGeometryData.hpp"
#include "virtualgeometry/VirtualGeometryPageSource.hpp"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace voxel::svraster
{

// A scratch file holding meshed terrain chunks as ordinary virtual-geometry
// pages.
//
// See docs/terrain-vg-unified-hierarchy-plan.md, phase 8. Terrain used to keep
// every meshed chunk's `VirtualGeometryEncodedData` resident for the life of
// the chunk, because the culler can ask for any of its pages at any time. That
// is ~9 MiB of CPU per resident chunk and it is why generation and publication
// had to be one transaction. Here a chunk is meshed once, encoded with the
// normal virtual-geometry machinery (`PageBuffer::encode` plus the same LZ4
// block framing `VirtualGeometryFile::write` uses) and appended to a file; a
// page request becomes a bounded read of one byte range.
//
// This is a cache, not a save format: it is truncated at open, its layout
// carries no compatibility promise, and a region a chunk no longer needs is
// free for the next chunk to reuse.
//
// Threading: `appendChunk` and the read calls take an internal mutex and may
// be called from the terrain worker pool. Nothing here touches the render
// thread's structures.

// One page as the scene install path needs it: exactly the three things
// `installSparseVoxelTerrainVGChunkPage` reads out of an in-memory asset.
struct TerrainVirtualGeometryScratchPage
{
  std::vector<uint32_t> encodedWords;
  // Asset-local page indices, mapped to page slots by the caller.
  std::vector<uint32_t> dependencies;
  std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> installUpdates;
  std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> uninstallUpdates;
};

// What a chunk install needs before its pages: the hierarchy slice, the root
// page and the quantization the decoder reconstructs positions with.
struct TerrainVirtualGeometryScratchChunk
{
  uint32_t pageCount = 0u;
  uint32_t rootPageIndex = 0u;
  uint32_t rootNodeIndex = 0u;
  virtualgeometry::QuantizationConfig quantization{};
  std::vector<virtualgeometry::VirtualGeometryHierarchy> hierarchy;
  // Every page's install rewrites, in page order. The parent-republish path
  // needs all of them the moment the chunk installs (it decides, per node,
  // which of a coarse chunk's clusters stop being streaming boundaries as its
  // children arrive), so they live in the header rather than costing one read
  // per page at install time.
  std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> installUpdates;
  // How many of `installUpdates` belong to each page, in page order. The
  // rewrites a page carries are only correct once that page is installed - the
  // builder computes them assuming every page up to it is resident - so a
  // consumer that installs pages lazily has to apply them page by page rather
  // than as one concatenated block.
  std::vector<uint32_t> installUpdateCounts;
  // Every page's dependencies (asset-local page indices), concatenated in page
  // order, with the per-page counts beside them. With these in the header a
  // page install needs nothing from the page's own block but its words - and
  // the scene reads those itself through `pageLocation` (phase 8g), so the
  // caller never touches page bytes at all.
  std::vector<uint32_t> dependencies;
  std::vector<uint32_t> dependencyCounts;
};

struct TerrainVirtualGeometryScratchStats
{
  uint64_t chunksWritten = 0u;
  uint64_t chunksLive = 0u;
  uint64_t pagesWritten = 0u;
  uint64_t bytesWritten = 0u;
  // Bytes the live chunks hold, against the file's high-water mark.
  uint64_t bytesLive = 0u;
  uint64_t fileBytes = 0u;
  // Region reuse (phase 8c).
  uint64_t regionsFree = 0u;
  uint64_t bytesFree = 0u;
  uint64_t appendsReusingRegion = 0u;
  uint64_t appendsExtendingFile = 0u;
  uint64_t appendsRefusedByBudget = 0u;
  uint64_t chunkReads = 0u;
  uint64_t pageReads = 0u;
  // Reads through the page-source interface (phase 8g): the scene reading a
  // page's words at install or after an eviction.
  uint64_t encodedPageReads = 0u;
  uint64_t staleLocationReads = 0u;
  uint64_t bytesRead = 0u;
  double writeMilliseconds = 0.0;
  double readMilliseconds = 0.0;
  uint64_t writeFailures = 0u;
  uint64_t readFailures = 0u;
};

// A chunk encoded and compressed but not yet written.
//
// Encoding is where the work is (PageBuffer::encode plus LZ4 over a few hundred
// KB) and it needs nothing but the asset, so it runs on the terrain worker that
// meshed the chunk. The append is then an fwrite of bytes that are already
// final, which is cheap enough for the thread that publishes the chunk - and a
// build dropped at publication (its chunk moved on while it was meshing) never
// reaches the file at all.
struct TerrainVirtualGeometryScratchBlob
{
  struct Block
  {
    uint64_t offset = 0u;
    uint32_t storedBytes = 0u;
    uint32_t uncompressedBytes = 0u;
  };

  // Block 0 is the chunk header; blocks 1..pageCount are the pages, in order.
  std::vector<Block> blocks;
  std::vector<uint8_t> bytes;
  uint32_t pageCount = 0u;
  uint32_t rootPageIndex = 0u;
  uint32_t unitScaleBits = 0u;

  bool empty() const { return blocks.empty(); }
  void clear()
  {
    blocks.clear();
    bytes.clear();
    bytes.shrink_to_fit();
    pageCount = 0u;
  }
};

class TerrainVirtualGeometryScratchFile : public virtualgeometry::VirtualGeometryPageSource
{
public:
  static constexpr uint32_t kInvalidChunk = 0xffffffffu;

  TerrainVirtualGeometryScratchFile();
  ~TerrainVirtualGeometryScratchFile();

  TerrainVirtualGeometryScratchFile(const TerrainVirtualGeometryScratchFile &) = delete;
  TerrainVirtualGeometryScratchFile &operator=(const TerrainVirtualGeometryScratchFile &) = delete;

  // Creates `path`, and refuses a path that already exists: the file is
  // truncated on create, so a mistyped path would be destroyed before anything
  // could report it, and stale geometry read as current is a hole with no
  // diagnostic. `close` removes it - nothing outlives the process.
  // `maxBytes` bounds the file: an append that neither fits a freed region nor
  // fits under the bound is refused rather than growing without limit. Zero is
  // unbounded. A refused append is not fatal - the caller keeps the chunk's
  // in-memory asset - but a run that hits it constantly is running with a
  // budget below its working set, and the stats say so.
  bool open(const std::string &path, uint64_t maxBytes = 0u, std::string *error = nullptr);
  void close();
  bool isOpen() const;
  const std::string &path() const { return path_; }

  // Encodes and appends every page of `asset` and hands back the handle a read
  // needs. Worker-safe.
  bool appendChunk(
      const virtualgeometry::VirtualGeometryEncodedData &asset,
      uint32_t &chunkHandle,
      std::string *error = nullptr);

  // Phase one of the same thing, touching no file state: safe to call from any
  // worker, on a cache with no file open at all.
  static bool encodeChunk(
      const virtualgeometry::VirtualGeometryEncodedData &asset,
      TerrainVirtualGeometryScratchBlob &blob,
      std::string *error = nullptr);

  // Phase two: writes an already-encoded blob. `blob` is left cleared, so the
  // caller cannot append the same megabytes twice.
  bool appendEncodedChunk(
      TerrainVirtualGeometryScratchBlob &blob,
      uint32_t &chunkHandle,
      std::string *error = nullptr);

  bool readChunk(
      uint32_t chunkHandle,
      TerrainVirtualGeometryScratchChunk &chunk,
      std::string *error = nullptr) const;

  bool readPage(
      uint32_t chunkHandle,
      uint32_t pageIndex,
      TerrainVirtualGeometryScratchPage &page,
      std::string *error = nullptr) const;

  // Phase 8g: the page as a (file, offset, size) the scene reads itself.
  // `key` is the chunk handle and `index` the page, and `readEncodedPage`
  // refuses a location whose chunk is no longer live or whose range disagrees
  // with the record - a region can be reused by another chunk, and reading
  // through a stale location would be that chunk's geometry. Invalid (null
  // source) for a dead handle or a page past the chunk.
  virtualgeometry::VirtualGeometryPageLocation pageLocation(
      uint32_t chunkHandle, uint32_t pageIndex) const;
  bool tryReadEncodedPage(
      const virtualgeometry::VirtualGeometryPageLocation &location,
      std::vector<uint32_t> &words, bool &pending, std::string *error) const override;
  bool readEncodedPage(
      const virtualgeometry::VirtualGeometryPageLocation &location,
      std::vector<uint32_t> &words,
      std::string *error) const override;

  // Pages of a chunk, without re-reading its header. `pageCount` and
  // `rootPageIndex` without a read at all - the install path asks for these on
  // every page.
  uint32_t pageCountOf(uint32_t chunkHandle) const;
  uint32_t rootPageIndexOf(uint32_t chunkHandle) const;
  uint32_t unitScaleBitsOf(uint32_t chunkHandle) const;
  bool isLive(uint32_t chunkHandle) const;

  // Drops a chunk. Its bytes stay where they are until phase 8c reuses them;
  // the handle is retired either way, so a stale handle reads as dead rather
  // than as another chunk's geometry.
  bool releaseChunk(uint32_t chunkHandle);

  // Whether an append of `bytes` would find room under the budget: a free
  // region that fits, or the cursor with the bound not yet reached. Phase 8d
  // asks this before appending so it can retire retained chunks to make room
  // instead of refusing the write.
  bool canAppend(uint64_t bytes) const;
  // Bytes a live chunk occupies in the file; zero for a dead handle.
  uint64_t chunkBytesOf(uint32_t chunkHandle) const;

  TerrainVirtualGeometryScratchStats stats() const;

private:
  struct Range
  {
    uint64_t offset = 0u;
    uint32_t storedBytes = 0u;
    uint32_t uncompressedBytes = 0u;
  };

  // A span of the file no live chunk owns. Kept sorted by offset so adjacent
  // spans coalesce on release; without that a walk fragments the file into
  // thousands of chunk-sized holes and every append extends it instead.
  struct FreeRegion
  {
    uint64_t offset = 0u;
    uint64_t bytes = 0u;
  };

  struct ChunkRecord
  {
    Range header{};
    std::vector<Range> pages;
    uint32_t pageCount = 0u;
    uint32_t rootPageIndex = 0u;
    uint32_t unitScaleBits = 0u;
    uint64_t offset = 0u;
    uint64_t bytes = 0u;
    bool live = false;
  };

  // First fit over the free list, else the write cursor. Returns false when
  // neither has room under the budget.
  bool reserveRegion(uint64_t bytes, uint64_t &offset);
  void releaseRegion(uint64_t offset, uint64_t bytes);

  static bool compressBlock(
      const std::vector<uint8_t> &plain, TerrainVirtualGeometryScratchBlob &blob, std::string *error);
  bool readBlock(const Range &range, std::vector<uint8_t> &plain, std::string *error) const;
  static bool fail(std::string *error, const std::string &message);

  struct AsyncReads;
  mutable std::mutex asyncMutex_;
  mutable std::unique_ptr<AsyncReads> asyncReads_;
  mutable std::mutex mutex_;
  std::string path_;
  FILE *file_ = nullptr;
  uint64_t writeCursor_ = 0u;
  uint64_t maxBytes_ = 0u;
  std::vector<FreeRegion> freeRegions_;
  // Dead records are kept, and their handles never reissued. Reusing a handle
  // index would let a stale handle resolve to whatever chunk took its place -
  // one chunk drawing another chunk's geometry, which reads as a culler bug
  // for days. A dead record with its page table cleared is a few dozen bytes.
  std::vector<ChunkRecord> chunks_;
  mutable TerrainVirtualGeometryScratchStats stats_{};
};

} // namespace voxel::svraster
