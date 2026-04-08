#pragma once

#include <cstdint>
#include <map>
namespace rendering
{
struct Allocation
{
  uint64_t offset;
  uint64_t size;
};

class BufferManager
{
public:
  explicit BufferManager(uint64_t totalSize);

  Allocation allocate(uint64_t size);
  void deallocate(const Allocation &allocation);
  bool canAllocate(uint64_t size) const;

  uint64_t getTotalSize() const;
  uint64_t getAllocatedSize() const;

private:
  void coalesce(uint64_t offset);

  uint64_t totalSize_;
  std::map<uint64_t, uint64_t> freeBlocks_;
  std::map<uint64_t, uint64_t> allocatedBlocks_;
};
} // namespace rendering
