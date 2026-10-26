#include "voxel/svraster/TransientSparseVoxelPageStore.hpp"

#include "lz4.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace voxel::svraster
{
namespace
{

constexpr uint64_t kTransientPageMagic = 0x3150475056535455ull; // UTSVPGP1
std::atomic<uint64_t> nextDirectoryId{0u};

uint64_t checksum(const std::vector<uint8_t> &bytes)
{
  uint64_t hash = 1469598103934665603ull;
  for (const uint8_t byte : bytes)
  {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

template <typename T>
bool writeScalar(std::ofstream &stream, const T value)
{
  stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
  return stream.good();
}

template <typename T>
bool readScalar(std::ifstream &stream, T &value)
{
  stream.read(reinterpret_cast<char *>(&value), sizeof(value));
  return stream.good();
}

} // namespace

bool operator==(
    const TransientSparseVoxelPageKey &left,
    const TransientSparseVoxelPageKey &right)
{
  return left.source == right.source && left.page == right.page;
}

size_t TransientSparseVoxelPageStore::KeyHash::operator()(
    const TransientSparseVoxelPageKey &key) const
{
  uint64_t value = key.source ^
      (key.page + 0x9e3779b97f4a7c15ull + (key.source << 6u) +
       (key.source >> 2u));
  value ^= value >> 30u;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27u;
  value *= 0x94d049bb133111ebull;
  value ^= value >> 31u;
  return static_cast<size_t>(value);
}

TransientSparseVoxelPageStore::~TransientSparseVoxelPageStore()
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code ignored;
  if (!spillDirectory_.empty())
    std::filesystem::remove_all(spillDirectory_, ignored);
}

bool TransientSparseVoxelPageStore::initialize(
    const TransientSparseVoxelPageStoreSettings &settings,
    std::string *error)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (error != nullptr)
    error->clear();
  std::error_code filesystemError;
  if (!spillDirectory_.empty())
    std::filesystem::remove_all(spillDirectory_, filesystemError);
  memoryPages_.clear();
  diskPages_.clear();
  memoryBytes_ = 0u;
  diskBytes_ = 0u;
  clock_ = 0u;
  statistics_ = {};
  settings_ = settings;
  spillDirectory_.clear();
  if (settings_.maximumDiskBytes != 0u)
  {
    std::filesystem::path root = settings_.spillRoot;
    if (root.empty())
      root = std::filesystem::temp_directory_path(filesystemError);
    if (filesystemError)
    {
      if (error != nullptr)
        *error = "Could not resolve the transient sparse voxel spill root: " +
            filesystemError.message();
      initialized_ = false;
      return false;
    }
    const uint64_t stamp = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    spillDirectory_ = root /
        ("engine-transient-vsvo-" + std::to_string(stamp) + "-" +
         std::to_string(nextDirectoryId.fetch_add(1u)));
    std::filesystem::create_directories(spillDirectory_, filesystemError);
    if (filesystemError)
    {
      if (error != nullptr)
        *error = "Could not create the transient sparse voxel spill directory: " +
            filesystemError.message();
      spillDirectory_.clear();
      initialized_ = false;
      return false;
    }
  }
  initialized_ = true;
  return true;
}

bool TransientSparseVoxelPageStore::compress(
    const std::vector<uint8_t> &payload,
    MemoryPage &page,
    std::string *error) const
{
  if (payload.empty() || payload.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
  {
    if (error != nullptr)
      *error = "Transient sparse voxel pages must be non-empty and fit LZ4 addressing";
    return false;
  }
  const int inputBytes = static_cast<int>(payload.size());
  page.compressed.resize(static_cast<size_t>(LZ4_compressBound(inputBytes)));
  const int compressedBytes = LZ4_compress_default(
      reinterpret_cast<const char *>(payload.data()),
      reinterpret_cast<char *>(page.compressed.data()), inputBytes,
      static_cast<int>(page.compressed.size()));
  if (compressedBytes <= 0)
  {
    if (error != nullptr)
      *error = "LZ4 could not compress a transient sparse voxel page";
    return false;
  }
  page.compressed.resize(static_cast<size_t>(compressedBytes));
  page.uncompressedBytes = payload.size();
  page.checksum = checksum(payload);
  return true;
}

bool TransientSparseVoxelPageStore::decode(
    const MemoryPage &page,
    std::vector<uint8_t> &payload) const
{
  if (page.uncompressedBytes == 0u ||
      page.uncompressedBytes > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
      page.compressed.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    return false;
  payload.resize(static_cast<size_t>(page.uncompressedBytes));
  const int decoded = LZ4_decompress_safe(
      reinterpret_cast<const char *>(page.compressed.data()),
      reinterpret_cast<char *>(payload.data()),
      static_cast<int>(page.compressed.size()),
      static_cast<int>(page.uncompressedBytes));
  if (decoded != static_cast<int>(page.uncompressedBytes) ||
      checksum(payload) != page.checksum)
  {
    payload.clear();
    return false;
  }
  return true;
}

std::filesystem::path TransientSparseVoxelPageStore::pathFor(
    const TransientSparseVoxelPageKey &key) const
{
  std::ostringstream name;
  name << std::hex << std::setfill('0') << std::setw(16) << key.source
       << '-' << std::setw(16) << key.page << ".lz4page";
  return spillDirectory_ / name.str();
}

void TransientSparseVoxelPageStore::removeDiskPage(
    std::unordered_map<TransientSparseVoxelPageKey, DiskPage, KeyHash>::iterator page)
{
  std::error_code ignored;
  std::filesystem::remove(page->second.path, ignored);
  diskBytes_ -= std::min(diskBytes_, page->second.compressedBytes);
  diskPages_.erase(page);
}

void TransientSparseVoxelPageStore::enforceDiskBudget(
    const uint64_t incomingBytes)
{
  while (!diskPages_.empty() &&
         (incomingBytes > settings_.maximumDiskBytes ||
          diskBytes_ > settings_.maximumDiskBytes - incomingBytes))
  {
    const auto oldest = std::min_element(
        diskPages_.begin(), diskPages_.end(),
        [](const auto &left, const auto &right)
        {
          return left.second.lastUse < right.second.lastUse;
        });
    removeDiskPage(oldest);
    ++statistics_.droppedPages;
  }
}

bool TransientSparseVoxelPageStore::spill(
    const TransientSparseVoxelPageKey &key,
    MemoryPage &&page)
{
  if (settings_.maximumDiskBytes == 0u || spillDirectory_.empty() ||
      page.compressed.size() > settings_.maximumDiskBytes)
  {
    ++statistics_.droppedPages;
    return false;
  }
  enforceDiskBudget(page.compressed.size());
  const std::filesystem::path path = pathFor(key);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  const uint64_t compressedBytes = page.compressed.size();
  if (!stream || !writeScalar(stream, kTransientPageMagic) ||
      !writeScalar(stream, page.uncompressedBytes) ||
      !writeScalar(stream, compressedBytes) ||
      !writeScalar(stream, page.checksum))
  {
    ++statistics_.droppedPages;
    return false;
  }
  stream.write(
      reinterpret_cast<const char *>(page.compressed.data()),
      static_cast<std::streamsize>(page.compressed.size()));
  stream.close();
  if (!stream)
  {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    ++statistics_.droppedPages;
    return false;
  }
  if (const auto existing = diskPages_.find(key); existing != diskPages_.end())
    removeDiskPage(existing);
  diskPages_.emplace(
      key,
      DiskPage{
        path, compressedBytes, page.uncompressedBytes, page.checksum,
        page.lastUse});
  diskBytes_ += compressedBytes;
  ++statistics_.diskSpills;
  return true;
}

void TransientSparseVoxelPageStore::enforceMemoryBudget()
{
  while (!memoryPages_.empty() && memoryBytes_ > settings_.maximumMemoryBytes)
  {
    const auto oldest = std::min_element(
        memoryPages_.begin(), memoryPages_.end(),
        [](const auto &left, const auto &right)
        {
          return left.second.lastUse < right.second.lastUse;
        });
    const uint64_t bytes = oldest->second.compressed.size();
    const TransientSparseVoxelPageKey key = oldest->first;
    MemoryPage page = std::move(oldest->second);
    memoryPages_.erase(oldest);
    memoryBytes_ -= std::min(memoryBytes_, bytes);
    spill(key, std::move(page));
  }
}

bool TransientSparseVoxelPageStore::put(
    const TransientSparseVoxelPageKey &key,
    const std::vector<uint8_t> &payload,
    std::string *error)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (error != nullptr)
    error->clear();
  if (!initialized_)
  {
    if (error != nullptr)
      *error = "Transient sparse voxel page store is not initialized";
    return false;
  }
  MemoryPage page{};
  if (!compress(payload, page, error))
    return false;
  page.lastUse = ++clock_;
  if (const auto existing = memoryPages_.find(key); existing != memoryPages_.end())
  {
    memoryBytes_ -= std::min<uint64_t>(memoryBytes_, existing->second.compressed.size());
    memoryPages_.erase(existing);
  }
  if (const auto existing = diskPages_.find(key); existing != diskPages_.end())
    removeDiskPage(existing);
  memoryBytes_ += page.compressed.size();
  memoryPages_.emplace(key, std::move(page));
  enforceMemoryBudget();
  return true;
}

TransientSparseVoxelPageSource TransientSparseVoxelPageStore::get(
    const TransientSparseVoxelPageKey &key,
    std::vector<uint8_t> &payload,
    std::string *error)
{
  std::lock_guard<std::mutex> lock(mutex_);
  payload.clear();
  if (error != nullptr)
    error->clear();
  if (!initialized_)
  {
    if (error != nullptr)
      *error = "Transient sparse voxel page store is not initialized";
    return TransientSparseVoxelPageSource::Missing;
  }
  if (const auto memory = memoryPages_.find(key); memory != memoryPages_.end())
  {
    memory->second.lastUse = ++clock_;
    if (decode(memory->second, payload))
    {
      ++statistics_.memoryHits;
      return TransientSparseVoxelPageSource::Memory;
    }
    memoryBytes_ -= std::min<uint64_t>(
        memoryBytes_, memory->second.compressed.size());
    memoryPages_.erase(memory);
    ++statistics_.corruptPages;
    ++statistics_.misses;
    return TransientSparseVoxelPageSource::Missing;
  }
  const auto disk = diskPages_.find(key);
  if (disk == diskPages_.end())
  {
    ++statistics_.misses;
    return TransientSparseVoxelPageSource::Missing;
  }
  std::ifstream stream(disk->second.path, std::ios::binary);
  uint64_t magic = 0u;
  uint64_t uncompressedBytes = 0u;
  uint64_t compressedBytes = 0u;
  uint64_t storedChecksum = 0u;
  MemoryPage page{};
  if (!stream || !readScalar(stream, magic) ||
      !readScalar(stream, uncompressedBytes) ||
      !readScalar(stream, compressedBytes) ||
      !readScalar(stream, storedChecksum) || magic != kTransientPageMagic ||
      compressedBytes != disk->second.compressedBytes ||
      uncompressedBytes != disk->second.uncompressedBytes ||
      storedChecksum != disk->second.checksum ||
      compressedBytes > static_cast<uint64_t>(std::numeric_limits<int>::max()))
  {
    removeDiskPage(disk);
    ++statistics_.corruptPages;
    ++statistics_.misses;
    return TransientSparseVoxelPageSource::Missing;
  }
  page.compressed.resize(static_cast<size_t>(compressedBytes));
  stream.read(
      reinterpret_cast<char *>(page.compressed.data()),
      static_cast<std::streamsize>(page.compressed.size()));
  page.uncompressedBytes = uncompressedBytes;
  page.checksum = storedChecksum;
  page.lastUse = ++clock_;
  if (!stream || !decode(page, payload))
  {
    removeDiskPage(disk);
    ++statistics_.corruptPages;
    ++statistics_.misses;
    return TransientSparseVoxelPageSource::Missing;
  }
  ++statistics_.diskHits;
  // Promote if it fits. Otherwise leave the only cached copy on disk.
  if (page.compressed.size() <= settings_.maximumMemoryBytes)
  {
    removeDiskPage(disk);
    memoryBytes_ += page.compressed.size();
    memoryPages_.emplace(key, std::move(page));
    enforceMemoryBudget();
  }
  else
  {
    disk->second.lastUse = clock_;
  }
  return TransientSparseVoxelPageSource::Disk;
}

void TransientSparseVoxelPageStore::erase(
    const TransientSparseVoxelPageKey &key)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto memory = memoryPages_.find(key); memory != memoryPages_.end())
  {
    memoryBytes_ -= std::min<uint64_t>(
        memoryBytes_, memory->second.compressed.size());
    memoryPages_.erase(memory);
  }
  if (const auto disk = diskPages_.find(key); disk != diskPages_.end())
    removeDiskPage(disk);
}

void TransientSparseVoxelPageStore::clear()
{
  std::lock_guard<std::mutex> lock(mutex_);
  memoryPages_.clear();
  diskPages_.clear();
  memoryBytes_ = 0u;
  diskBytes_ = 0u;
  std::error_code ignored;
  if (!spillDirectory_.empty())
  {
    std::filesystem::remove_all(spillDirectory_, ignored);
    std::filesystem::create_directories(spillDirectory_, ignored);
  }
}

bool TransientSparseVoxelPageStore::initialized() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

TransientSparseVoxelPageStoreStatistics
TransientSparseVoxelPageStore::statistics() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = statistics_;
  result.memoryPageCount = memoryPages_.size();
  result.diskPageCount = diskPages_.size();
  result.memoryBytes = memoryBytes_;
  result.diskBytes = diskBytes_;
  return result;
}

} // namespace voxel::svraster
