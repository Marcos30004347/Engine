#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace voxel::svraster
{

struct TransientSparseVoxelPageKey
{
  uint64_t source = 0u;
  uint64_t page = 0u;
};

bool operator==(
    const TransientSparseVoxelPageKey &left,
    const TransientSparseVoxelPageKey &right);

struct TransientSparseVoxelPageStoreSettings
{
  uint64_t maximumMemoryBytes = 4ull * 1024u * 1024u * 1024u;
  uint64_t maximumDiskBytes = 4ull * 1024u * 1024u * 1024u;
  // A unique owned child directory is created below this root. Empty uses the
  // platform temporary directory. The owned child is always removed when the
  // store is destroyed; the supplied root itself is never removed.
  std::filesystem::path spillRoot;
};

struct TransientSparseVoxelPageStoreStatistics
{
  uint64_t memoryHits = 0u;
  uint64_t diskHits = 0u;
  uint64_t misses = 0u;
  uint64_t diskSpills = 0u;
  uint64_t droppedPages = 0u;
  uint64_t corruptPages = 0u;
  uint64_t memoryPageCount = 0u;
  uint64_t diskPageCount = 0u;
  uint64_t memoryBytes = 0u;
  uint64_t diskBytes = 0u;
};

enum class TransientSparseVoxelPageSource : uint8_t
{
  Missing = 0u,
  Memory,
  Disk,
};

// Runtime-only backing store for deterministic procedural pages. Payloads are
// LZ4-compressed in RAM, least-recently-used pages spill to an owned temporary
// directory, and disk overflow simply drops the oldest page because any page
// can be generated again. It deliberately has no persistent save operation.
class TransientSparseVoxelPageStore
{
public:
  TransientSparseVoxelPageStore() = default;
  TransientSparseVoxelPageStore(const TransientSparseVoxelPageStore &) = delete;
  TransientSparseVoxelPageStore &operator=(const TransientSparseVoxelPageStore &) = delete;
  ~TransientSparseVoxelPageStore();

  bool initialize(
      const TransientSparseVoxelPageStoreSettings &settings = {},
      std::string *error = nullptr);
  bool put(
      const TransientSparseVoxelPageKey &key,
      const std::vector<uint8_t> &payload,
      std::string *error = nullptr);
  TransientSparseVoxelPageSource get(
      const TransientSparseVoxelPageKey &key,
      std::vector<uint8_t> &payload,
      std::string *error = nullptr);
  void erase(const TransientSparseVoxelPageKey &key);
  void clear();

  bool initialized() const;
  const std::filesystem::path &spillDirectory() const { return spillDirectory_; }
  TransientSparseVoxelPageStoreStatistics statistics() const;

private:
  struct KeyHash
  {
    size_t operator()(const TransientSparseVoxelPageKey &key) const;
  };

  struct MemoryPage
  {
    std::vector<uint8_t> compressed;
    uint64_t uncompressedBytes = 0u;
    uint64_t checksum = 0u;
    uint64_t lastUse = 0u;
  };

  struct DiskPage
  {
    std::filesystem::path path;
    uint64_t compressedBytes = 0u;
    uint64_t uncompressedBytes = 0u;
    uint64_t checksum = 0u;
    uint64_t lastUse = 0u;
  };

  bool compress(
      const std::vector<uint8_t> &payload,
      MemoryPage &page,
      std::string *error) const;
  bool decode(
      const MemoryPage &page,
      std::vector<uint8_t> &payload) const;
  bool spill(
      const TransientSparseVoxelPageKey &key,
      MemoryPage &&page);
  void enforceMemoryBudget();
  void enforceDiskBudget(uint64_t incomingBytes);
  void removeDiskPage(
      std::unordered_map<TransientSparseVoxelPageKey, DiskPage, KeyHash>::iterator page);
  std::filesystem::path pathFor(const TransientSparseVoxelPageKey &key) const;

  mutable std::mutex mutex_;
  bool initialized_ = false;
  TransientSparseVoxelPageStoreSettings settings_{};
  std::filesystem::path spillDirectory_;
  uint64_t clock_ = 0u;
  uint64_t memoryBytes_ = 0u;
  uint64_t diskBytes_ = 0u;
  TransientSparseVoxelPageStoreStatistics statistics_{};
  std::unordered_map<TransientSparseVoxelPageKey, MemoryPage, KeyHash> memoryPages_;
  std::unordered_map<TransientSparseVoxelPageKey, DiskPage, KeyHash> diskPages_;
};

} // namespace voxel::svraster
