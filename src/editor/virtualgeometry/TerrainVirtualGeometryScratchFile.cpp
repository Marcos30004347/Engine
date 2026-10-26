#include <condition_variable>
#include <thread>
#include <deque>
#include <map>
#include "editor/virtualgeometry/TerrainVirtualGeometryScratchFile.hpp"

#include "virtualgeometry/VirtualGeometryFile.hpp"

#include "lz4.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>

namespace voxel::svraster
{

namespace
{

using Clock = std::chrono::steady_clock;

double millisecondsSince(const Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <typename T>
void appendPod(std::vector<uint8_t> &out, const T &value)
{
  const size_t offset = out.size();
  out.resize(offset + sizeof(T));
  std::memcpy(out.data() + offset, &value, sizeof(T));
}

template <typename T>
void appendPodArray(std::vector<uint8_t> &out, const std::vector<T> &values)
{
  appendPod<uint32_t>(out, static_cast<uint32_t>(values.size()));
  if (values.empty())
    return;
  const size_t offset = out.size();
  const size_t bytes = values.size() * sizeof(T);
  out.resize(offset + bytes);
  std::memcpy(out.data() + offset, values.data(), bytes);
}

// A cursor that refuses to read past the block rather than trusting the
// counts it just read: a truncated or reused region must fail the read, not
// hand back geometry made of whatever followed it.
struct Reader
{
  const uint8_t *data = nullptr;
  size_t size = 0u;
  size_t cursor = 0u;
  bool ok = true;

  template <typename T>
  bool pod(T &value)
  {
    if (!ok || cursor + sizeof(T) > size)
      return ok = false;
    std::memcpy(&value, data + cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
  }

  template <typename T>
  bool podArray(std::vector<T> &values)
  {
    uint32_t count = 0u;
    if (!pod(count))
      return false;
    const size_t bytes = static_cast<size_t>(count) * sizeof(T);
    if (cursor + bytes > size)
      return ok = false;
    values.resize(count);
    if (count != 0u)
      std::memcpy(values.data(), data + cursor, bytes);
    cursor += bytes;
    return true;
  }
};

} // namespace

struct TerrainVirtualGeometryScratchFile::AsyncReads
{
  struct Request
  {
    virtualgeometry::VirtualGeometryPageLocation location;
    std::vector<uint32_t> words;
    std::string error;
    bool ready = false, success = false;
  };
  using Key = std::pair<uint64_t, uint32_t>;
  std::mutex mutex;
  std::condition_variable wake;
  std::map<Key, std::shared_ptr<Request>> requests;
  std::deque<std::shared_ptr<Request>> queue;
  bool stopping = false;
  std::thread worker;
  explicit AsyncReads(const TerrainVirtualGeometryScratchFile &source)
      : worker([this, &source]() {
        for (;;)
        {
          std::shared_ptr<Request> request;
          {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&] { return stopping || !queue.empty(); });
            if (stopping) return;
            request = queue.front(); queue.pop_front();
          }
          const bool success = source.readEncodedPage(request->location, request->words, &request->error);
          {
            std::lock_guard<std::mutex> lock(mutex);
            request->success = success; request->ready = true;
          }
        }
      }) {}
  ~AsyncReads()
  {
    { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
    wake.notify_one();
    worker.join();
  }
};

TerrainVirtualGeometryScratchFile::TerrainVirtualGeometryScratchFile() = default;

bool TerrainVirtualGeometryScratchFile::tryReadEncodedPage(
    const virtualgeometry::VirtualGeometryPageLocation &location,
    std::vector<uint32_t> &words, bool &pending, std::string *error) const
{
  words.clear(); pending = false;
  if (error) error->clear();
  // Validate again when consuming: the worker may have completed before a
  // chunk was retired. Handles are never reused, including after eviction.
  {
    std::unique_lock<std::mutex> metadataLock(mutex_, std::try_to_lock);
    if (!metadataLock.owns_lock()) { pending = true; return false; }
    if (location.source != this || location.key >= chunks_.size() ||
        !chunks_[location.key].live || location.index >= chunks_[location.key].pages.size())
      return fail(error, "asynchronous terrain page location is stale");
    const auto &range = chunks_[location.key].pages[location.index];
    if (range.offset != location.offset || range.storedBytes != location.storedBytes ||
        range.uncompressedBytes != location.uncompressedBytes)
      return fail(error, "asynchronous terrain page location range is stale");
  }
  std::lock_guard<std::mutex> stateLock(asyncMutex_);
  if (!asyncReads_) asyncReads_ = std::make_unique<AsyncReads>(*this);
  auto &state = *asyncReads_;
  std::lock_guard<std::mutex> lock(state.mutex);
  const AsyncReads::Key key{location.key, location.index};
  auto it = state.requests.find(key);
  if (it != state.requests.end())
  {
    const auto &request = it->second;
    if (!request->ready) { pending = true; return false; }
    const bool success = request->success;
    words = std::move(request->words);
    if (error) *error = request->error;
    state.requests.erase(it);
    return success;
  }
  // Bound both disk work and completed pages that ceased being requested.
  if (state.requests.size() >= 64u)
  {
    auto expired = std::find_if(state.requests.begin(), state.requests.end(),
        [](const auto &entry) { return entry.second->ready; });
    if (expired != state.requests.end()) state.requests.erase(expired);
    else { pending = true; return false; }
  }
  auto request = std::make_shared<AsyncReads::Request>();
  request->location = location;
  state.requests.emplace(key, request);
  state.queue.push_back(request);
  state.wake.notify_one();
  pending = true;
  return false;
}

TerrainVirtualGeometryScratchFile::~TerrainVirtualGeometryScratchFile()
{
  close();
}

bool TerrainVirtualGeometryScratchFile::fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool TerrainVirtualGeometryScratchFile::open(
    const std::string &path, const uint64_t maxBytes, std::string *error)
{
  { std::lock_guard<std::mutex> lock(asyncMutex_); asyncReads_.reset(); }
  std::lock_guard<std::mutex> lock(mutex_);
  if (error != nullptr)
    error->clear();
  if (file_ != nullptr)
  {
    std::fclose(file_);
    file_ = nullptr;
  }
  chunks_.clear();
  freeRegions_.clear();
  writeCursor_ = 0u;
  maxBytes_ = maxBytes;
  stats_ = {};
  // Refuse a path that already exists. The open below truncates, and a
  // mistyped path would be destroyed before anything could report it; a
  // scratch file the process owns is always a new one.
  if (FILE *existing = std::fopen(path.c_str(), "rb"))
  {
    std::fclose(existing);
    return fail(
        error,
        "terrain virtual-geometry scratch path already exists, refusing to truncate it: " + path);
  }
  // "w+b": create and keep it readable. Reads and writes go through the same
  // handle under the mutex, so a page written this frame is readable the next.
  file_ = std::fopen(path.c_str(), "w+b");
  if (file_ == nullptr)
    return fail(error, "cannot create terrain virtual-geometry scratch file " + path);
  path_ = path;
  return true;
}

void TerrainVirtualGeometryScratchFile::close()
{
  // Join before taking the file lock: the worker may be finishing a read.
  { std::lock_guard<std::mutex> lock(asyncMutex_); asyncReads_.reset(); }
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_ != nullptr)
  {
    std::fclose(file_);
    file_ = nullptr;
    // The file is a cache and nothing outlives the process that wrote it.
    std::remove(path_.c_str());
  }
  chunks_.clear();
  freeRegions_.clear();
  writeCursor_ = 0u;
}

bool TerrainVirtualGeometryScratchFile::reserveRegion(
    const uint64_t bytes, uint64_t &offset)
{
  // First fit. A chunk's blob is written whole, so a region either takes it or
  // it does not; best fit would cost a scan for a placement quality this
  // workload cannot use - chunks of the same LOD are near enough the same size
  // that freed regions are reused by their own kind.
  for (size_t index = 0u; index < freeRegions_.size(); ++index)
  {
    if (freeRegions_[index].bytes < bytes)
      continue;
    offset = freeRegions_[index].offset;
    if (freeRegions_[index].bytes == bytes)
      freeRegions_.erase(freeRegions_.begin() + static_cast<long>(index));
    else
    {
      freeRegions_[index].offset += bytes;
      freeRegions_[index].bytes -= bytes;
    }
    ++stats_.appendsReusingRegion;
    return true;
  }
  if (maxBytes_ != 0u && writeCursor_ + bytes > maxBytes_)
  {
    ++stats_.appendsRefusedByBudget;
    return false;
  }
  offset = writeCursor_;
  writeCursor_ += bytes;
  ++stats_.appendsExtendingFile;
  return true;
}

void TerrainVirtualGeometryScratchFile::releaseRegion(
    const uint64_t offset, const uint64_t bytes)
{
  if (bytes == 0u)
    return;
  // Insert in offset order and coalesce with either neighbour. Without the
  // coalesce a walk leaves the file as thousands of chunk-sized holes and
  // every append extends it instead of reusing one.
  size_t index = 0u;
  while (index < freeRegions_.size() && freeRegions_[index].offset < offset)
    ++index;
  freeRegions_.insert(freeRegions_.begin() + static_cast<long>(index), FreeRegion{offset, bytes});
  if (index + 1u < freeRegions_.size() &&
      freeRegions_[index].offset + freeRegions_[index].bytes == freeRegions_[index + 1u].offset)
  {
    freeRegions_[index].bytes += freeRegions_[index + 1u].bytes;
    freeRegions_.erase(freeRegions_.begin() + static_cast<long>(index) + 1);
  }
  if (index != 0u &&
      freeRegions_[index - 1u].offset + freeRegions_[index - 1u].bytes == freeRegions_[index].offset)
  {
    freeRegions_[index - 1u].bytes += freeRegions_[index].bytes;
    freeRegions_.erase(freeRegions_.begin() + static_cast<long>(index));
  }
  // A region at the very end gives its bytes back to the cursor, so a file
  // that empties shrinks its high-water mark instead of holding it forever.
  if (!freeRegions_.empty() &&
      freeRegions_.back().offset + freeRegions_.back().bytes == writeCursor_)
  {
    writeCursor_ = freeRegions_.back().offset;
    freeRegions_.pop_back();
  }
}

bool TerrainVirtualGeometryScratchFile::isOpen() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return file_ != nullptr;
}

bool TerrainVirtualGeometryScratchFile::compressBlock(
    const std::vector<uint8_t> &plain,
    TerrainVirtualGeometryScratchBlob &blob,
    std::string *error)
{
  if (plain.empty())
    return fail(error, "terrain scratch block is empty");
  if (plain.size() > static_cast<size_t>(INT_MAX))
    return fail(error, "terrain scratch block is too large for LZ4 block compression");

  const int bound = LZ4_compressBound(static_cast<int>(plain.size()));
  const size_t offset = blob.bytes.size();
  blob.bytes.resize(offset + static_cast<size_t>(bound));
  const int storedBytes = LZ4_compress_default(
      reinterpret_cast<const char *>(plain.data()),
      reinterpret_cast<char *>(blob.bytes.data() + offset),
      static_cast<int>(plain.size()),
      bound);
  if (storedBytes <= 0)
  {
    blob.bytes.resize(offset);
    return fail(error, "terrain scratch block failed to LZ4-compress");
  }
  blob.bytes.resize(offset + static_cast<size_t>(storedBytes));

  TerrainVirtualGeometryScratchBlob::Block block{};
  block.offset = offset;
  block.storedBytes = static_cast<uint32_t>(storedBytes);
  block.uncompressedBytes = static_cast<uint32_t>(plain.size());
  blob.blocks.push_back(block);
  return true;
}

bool TerrainVirtualGeometryScratchFile::readBlock(
    const Range &range, std::vector<uint8_t> &plain, std::string *error) const
{
  if (range.storedBytes == 0u || range.uncompressedBytes == 0u)
    return fail(error, "terrain scratch range is empty");
  std::vector<uint8_t> stored(range.storedBytes);
  if (std::fseek(file_, static_cast<long>(range.offset), SEEK_SET) != 0)
    return fail(error, "terrain scratch file seek failed");
  if (std::fread(stored.data(), 1u, stored.size(), file_) != stored.size())
    return fail(error, "terrain scratch file read failed");
  plain.resize(range.uncompressedBytes);
  const int decoded = LZ4_decompress_safe(
      reinterpret_cast<const char *>(stored.data()),
      reinterpret_cast<char *>(plain.data()),
      static_cast<int>(stored.size()),
      static_cast<int>(plain.size()));
  if (decoded != static_cast<int>(range.uncompressedBytes))
    return fail(error, "terrain scratch block failed to decompress");
  stats_.bytesRead += range.storedBytes;
  return true;
}

bool TerrainVirtualGeometryScratchFile::encodeChunk(
    const virtualgeometry::VirtualGeometryEncodedData &asset,
    TerrainVirtualGeometryScratchBlob &blob,
    std::string *error)
{
  blob.clear();
  if (error != nullptr)
    error->clear();
  if (asset.pages.empty())
    return fail(error, "terrain chunk asset has no pages");

  blob.pageCount = static_cast<uint32_t>(asset.pages.size());
  blob.rootPageIndex = asset.rootPageIndex;
  std::memcpy(
      &blob.unitScaleBits, &asset.quantizationConfig.unit_scale,
      sizeof(blob.unitScaleBits));

  // Block 0, the chunk header: everything the slot install needs before any
  // page reaches the scene.
  {
    std::vector<uint8_t> plain;
    appendPod<uint32_t>(plain, blob.pageCount);
    appendPod<uint32_t>(plain, blob.rootPageIndex);
    appendPod<uint32_t>(plain, asset.root_node_index);
    appendPod<uint32_t>(plain, blob.unitScaleBits);
    appendPod<uint8_t>(plain, asset.quantizationConfig.quantization_factor);
    appendPod<uint8_t>(plain, asset.quantizationConfig.padMeshlets ? 1u : 0u);
    appendPodArray(plain, asset.hierarchy);
    std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> installUpdates;
    std::vector<uint32_t> installUpdateCounts;
    installUpdateCounts.reserve(asset.pages.size());
    for (const auto &page : asset.pages)
    {
      installUpdates.insert(
          installUpdates.end(), page.installUpdates.hierarchyUpdates.begin(),
          page.installUpdates.hierarchyUpdates.end());
      installUpdateCounts.push_back(
          static_cast<uint32_t>(page.installUpdates.hierarchyUpdates.size()));
    }
    appendPodArray(plain, installUpdates);
    appendPodArray(plain, installUpdateCounts);
    std::vector<uint32_t> dependencies;
    std::vector<uint32_t> dependencyCounts;
    dependencyCounts.reserve(asset.pages.size());
    for (const auto &page : asset.pages)
    {
      dependencies.insert(
          dependencies.end(), page.dependencies.begin(), page.dependencies.end());
      dependencyCounts.push_back(static_cast<uint32_t>(page.dependencies.size()));
    }
    appendPodArray(plain, dependencies);
    appendPodArray(plain, dependencyCounts);
    if (!compressBlock(plain, blob, error))
    {
      blob.clear();
      return false;
    }
  }

  for (const auto &page : asset.pages)
  {
    // The same encoding the in-memory install path used, so the words that
    // reach the scene are the words the mesher produced either way.
    const virtualgeometry::PageBuffer encoded = virtualgeometry::PageBuffer::encode(page);
    if (encoded.data.empty())
    {
      blob.clear();
      return fail(error, "terrain chunk page encoded to nothing");
    }
    std::vector<uint8_t> plain;
    plain.reserve(encoded.data.size() * sizeof(uint32_t) + 64u);
    appendPodArray(plain, encoded.data);
    appendPodArray(plain, page.dependencies);
    appendPodArray(plain, page.installUpdates.hierarchyUpdates);
    appendPodArray(plain, page.uninstallUpdates.hierarchyUpdates);
    if (!compressBlock(plain, blob, error))
    {
      blob.clear();
      return false;
    }
  }
  return true;
}

bool TerrainVirtualGeometryScratchFile::appendEncodedChunk(
    TerrainVirtualGeometryScratchBlob &blob,
    uint32_t &chunkHandle,
    std::string *error)
{
  const auto start = Clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  chunkHandle = kInvalidChunk;
  if (error != nullptr)
    error->clear();
  if (file_ == nullptr)
    return fail(error, "terrain virtual-geometry scratch file is not open");
  if (blob.blocks.size() != static_cast<size_t>(blob.pageCount) + 1u)
  {
    ++stats_.writeFailures;
    return fail(error, "terrain scratch blob does not hold a header and every page");
  }

  uint64_t regionOffset = 0u;
  if (!reserveRegion(blob.bytes.size(), regionOffset))
    return fail(error, "terrain scratch file is at its size budget");

  // One write for the whole chunk: its blocks are already contiguous and in
  // order, so the directory is the blob's offsets shifted by the region base.
  if (std::fseek(file_, static_cast<long>(regionOffset), SEEK_SET) != 0 ||
      std::fwrite(blob.bytes.data(), 1u, blob.bytes.size(), file_) != blob.bytes.size())
  {
    ++stats_.writeFailures;
    releaseRegion(regionOffset, blob.bytes.size());
    return fail(error, "terrain scratch file write failed");
  }

  ChunkRecord record{};
  record.pageCount = blob.pageCount;
  record.rootPageIndex = blob.rootPageIndex;
  record.unitScaleBits = blob.unitScaleBits;
  record.header = Range{regionOffset + blob.blocks[0].offset, blob.blocks[0].storedBytes,
                        blob.blocks[0].uncompressedBytes};
  record.pages.reserve(blob.pageCount);
  for (uint32_t pageIndex = 0u; pageIndex < blob.pageCount; ++pageIndex)
  {
    const auto &block = blob.blocks[pageIndex + 1u];
    record.pages.push_back(
        Range{regionOffset + block.offset, block.storedBytes, block.uncompressedBytes});
  }
  record.offset = regionOffset;
  record.bytes = blob.bytes.size();
  record.live = true;

  chunkHandle = static_cast<uint32_t>(chunks_.size());
  stats_.bytesWritten += record.bytes;
  stats_.bytesLive += record.bytes;
  stats_.pagesWritten += record.pageCount;
  ++stats_.chunksWritten;
  ++stats_.chunksLive;
  stats_.fileBytes = writeCursor_;
  chunks_.push_back(std::move(record));
  stats_.writeMilliseconds += millisecondsSince(start);
  blob.clear();
  return true;
}

bool TerrainVirtualGeometryScratchFile::appendChunk(
    const virtualgeometry::VirtualGeometryEncodedData &asset,
    uint32_t &chunkHandle,
    std::string *error)
{
  TerrainVirtualGeometryScratchBlob blob;
  if (!encodeChunk(asset, blob, error))
  {
    chunkHandle = kInvalidChunk;
    return false;
  }
  return appendEncodedChunk(blob, chunkHandle, error);
}

bool TerrainVirtualGeometryScratchFile::readChunk(
    const uint32_t chunkHandle,
    TerrainVirtualGeometryScratchChunk &chunk,
    std::string *error) const
{
  const auto start = Clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  if (error != nullptr)
    error->clear();
  if (file_ == nullptr || chunkHandle >= chunks_.size() || !chunks_[chunkHandle].live)
  {
    ++stats_.readFailures;
    return fail(error, "terrain scratch chunk handle is not live");
  }
  const auto &record = chunks_[chunkHandle];
  std::vector<uint8_t> plain;
  if (!readBlock(record.header, plain, error))
  {
    ++stats_.readFailures;
    return false;
  }

  Reader reader{plain.data(), plain.size(), 0u, true};
  uint32_t pageCount = 0u;
  uint32_t rootPageIndex = 0u;
  uint32_t rootNodeIndex = 0u;
  uint32_t unitScaleBits = 0u;
  uint8_t quantizationFactor = 0u;
  uint8_t padMeshlets = 0u;
  reader.pod(pageCount);
  reader.pod(rootPageIndex);
  reader.pod(rootNodeIndex);
  reader.pod(unitScaleBits);
  reader.pod(quantizationFactor);
  reader.pod(padMeshlets);
  reader.podArray(chunk.hierarchy);
  reader.podArray(chunk.installUpdates);
  reader.podArray(chunk.installUpdateCounts);
  reader.podArray(chunk.dependencies);
  reader.podArray(chunk.dependencyCounts);
  if (!reader.ok)
  {
    ++stats_.readFailures;
    return fail(error, "terrain scratch chunk header is malformed");
  }

  chunk.pageCount = pageCount;
  chunk.rootPageIndex = rootPageIndex;
  chunk.rootNodeIndex = rootNodeIndex;
  chunk.quantization.quantization_factor = quantizationFactor;
  chunk.quantization.padMeshlets = padMeshlets != 0u;
  std::memcpy(&chunk.quantization.unit_scale, &unitScaleBits, sizeof(unitScaleBits));
  ++stats_.chunkReads;
  stats_.readMilliseconds += millisecondsSince(start);
  return true;
}

bool TerrainVirtualGeometryScratchFile::readPage(
    const uint32_t chunkHandle,
    const uint32_t pageIndex,
    TerrainVirtualGeometryScratchPage &page,
    std::string *error) const
{
  const auto start = Clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  if (error != nullptr)
    error->clear();
  if (file_ == nullptr || chunkHandle >= chunks_.size() || !chunks_[chunkHandle].live)
  {
    ++stats_.readFailures;
    return fail(error, "terrain scratch chunk handle is not live");
  }
  const auto &record = chunks_[chunkHandle];
  if (pageIndex >= record.pages.size())
  {
    ++stats_.readFailures;
    return fail(error, "terrain scratch page index is out of range");
  }
  std::vector<uint8_t> plain;
  if (!readBlock(record.pages[pageIndex], plain, error))
  {
    ++stats_.readFailures;
    return false;
  }

  Reader reader{plain.data(), plain.size(), 0u, true};
  reader.podArray(page.encodedWords);
  reader.podArray(page.dependencies);
  reader.podArray(page.installUpdates);
  reader.podArray(page.uninstallUpdates);
  if (!reader.ok)
  {
    ++stats_.readFailures;
    return fail(error, "terrain scratch page block is malformed");
  }
  ++stats_.pageReads;
  stats_.readMilliseconds += millisecondsSince(start);
  return true;
}

virtualgeometry::VirtualGeometryPageLocation TerrainVirtualGeometryScratchFile::pageLocation(
    const uint32_t chunkHandle, const uint32_t pageIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  virtualgeometry::VirtualGeometryPageLocation location{};
  if (file_ == nullptr || chunkHandle >= chunks_.size() || !chunks_[chunkHandle].live)
    return location;
  const auto &record = chunks_[chunkHandle];
  if (pageIndex >= record.pages.size())
    return location;
  const Range &range = record.pages[pageIndex];
  location.source = this;
  location.key = chunkHandle;
  location.index = pageIndex;
  location.offset = range.offset;
  location.storedBytes = range.storedBytes;
  location.uncompressedBytes = range.uncompressedBytes;
  return location;
}

bool TerrainVirtualGeometryScratchFile::readEncodedPage(
    const virtualgeometry::VirtualGeometryPageLocation &location,
    std::vector<uint32_t> &words,
    std::string *error) const
{
  const auto start = Clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  if (error != nullptr)
    error->clear();
  if (location.source != this)
  {
    ++stats_.readFailures;
    return fail(error, "terrain scratch page location names another source");
  }
  const uint32_t chunkHandle =
      location.key > UINT32_MAX ? kInvalidChunk : static_cast<uint32_t>(location.key);
  if (file_ == nullptr || chunkHandle >= chunks_.size() || !chunks_[chunkHandle].live)
  {
    ++stats_.readFailures;
    ++stats_.staleLocationReads;
    return fail(error, "terrain scratch page location names a chunk that is not live");
  }
  const auto &record = chunks_[chunkHandle];
  if (location.index >= record.pages.size())
  {
    ++stats_.readFailures;
    ++stats_.staleLocationReads;
    return fail(error, "terrain scratch page location is past the chunk");
  }
  // The location has to be the record's own range. A handle is never
  // reissued, so a mismatch means the location was made up or carried over
  // from a chunk this one replaced; either way, refuse rather than read.
  const Range &range = record.pages[location.index];
  if (range.offset != location.offset || range.storedBytes != location.storedBytes ||
      range.uncompressedBytes != location.uncompressedBytes)
  {
    ++stats_.readFailures;
    ++stats_.staleLocationReads;
    return fail(error, "terrain scratch page location does not match the chunk record");
  }
  std::vector<uint8_t> plain;
  if (!readBlock(range, plain, error))
  {
    ++stats_.readFailures;
    return false;
  }
  // Only the words: the dependencies and rewrites behind them are in the
  // chunk header, and the scene keeps its own copy across an eviction.
  Reader reader{plain.data(), plain.size(), 0u, true};
  reader.podArray(words);
  if (!reader.ok || words.empty())
  {
    ++stats_.readFailures;
    return fail(error, "terrain scratch page block is malformed");
  }
  ++stats_.encodedPageReads;
  stats_.readMilliseconds += millisecondsSince(start);
  return true;
}

uint32_t TerrainVirtualGeometryScratchFile::pageCountOf(const uint32_t chunkHandle) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (chunkHandle >= chunks_.size() || !chunks_[chunkHandle].live)
    return 0u;
  return chunks_[chunkHandle].pageCount;
}

uint32_t TerrainVirtualGeometryScratchFile::rootPageIndexOf(const uint32_t chunkHandle) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (chunkHandle >= chunks_.size() || !chunks_[chunkHandle].live)
    return 0u;
  return chunks_[chunkHandle].rootPageIndex;
}

uint32_t TerrainVirtualGeometryScratchFile::unitScaleBitsOf(const uint32_t chunkHandle) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (chunkHandle >= chunks_.size() || !chunks_[chunkHandle].live)
    return 0u;
  return chunks_[chunkHandle].unitScaleBits;
}

bool TerrainVirtualGeometryScratchFile::isLive(const uint32_t chunkHandle) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return chunkHandle < chunks_.size() && chunks_[chunkHandle].live;
}

bool TerrainVirtualGeometryScratchFile::releaseChunk(const uint32_t chunkHandle)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (chunkHandle >= chunks_.size() || !chunks_[chunkHandle].live)
    return false;
  auto &record = chunks_[chunkHandle];
  record.live = false;
  record.pages.clear();
  record.pages.shrink_to_fit();
  releaseRegion(record.offset, record.bytes);
  stats_.bytesLive -= std::min(stats_.bytesLive, record.bytes);
  --stats_.chunksLive;
  return true;
}

bool TerrainVirtualGeometryScratchFile::canAppend(const uint64_t bytes) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_ == nullptr)
    return false;
  for (const auto &region : freeRegions_)
    if (region.bytes >= bytes)
      return true;
  return maxBytes_ == 0u || writeCursor_ + bytes <= maxBytes_;
}

uint64_t TerrainVirtualGeometryScratchFile::chunkBytesOf(const uint32_t chunkHandle) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (chunkHandle >= chunks_.size() || !chunks_[chunkHandle].live)
    return 0u;
  return chunks_[chunkHandle].bytes;
}

TerrainVirtualGeometryScratchStats TerrainVirtualGeometryScratchFile::stats() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto copy = stats_;
  copy.fileBytes = writeCursor_;
  copy.regionsFree = freeRegions_.size();
  copy.bytesFree = 0u;
  for (const auto &region : freeRegions_)
    copy.bytesFree += region.bytes;
  return copy;
}

} // namespace voxel::svraster
