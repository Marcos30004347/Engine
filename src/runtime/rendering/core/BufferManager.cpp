#include "BufferManager.hpp"

using namespace rendering;

BufferManager::BufferManager(uint64_t totalSize) : totalSize_(totalSize)
{
  freeBlocks_[0] = totalSize;
}

Allocation BufferManager::allocate(uint64_t size)
{
  if (size == 0)
  {
    return Allocation{
      .offset = UINT64_MAX,
      .size = 0,
    };
  }

  for (auto it = freeBlocks_.begin(); it != freeBlocks_.end(); ++it)
  {
    if (it->second >= size)
    {
      uint64_t offset = it->first;
      uint64_t blockSize = it->second;

      freeBlocks_.erase(it);

      if (blockSize > size)
      {
        freeBlocks_[offset + size] = blockSize - size;
      }

      allocatedBlocks_[offset] = size;

      return {offset, size};
    }
  }

  return Allocation{
    .offset = UINT64_MAX,
    .size = 0,
  };
}

void BufferManager::deallocate(const Allocation &allocation)
{
  auto it = allocatedBlocks_.find(allocation.offset);
  if (it == allocatedBlocks_.end() || it->second != allocation.size)
  {
    throw std::invalid_argument("Invalid allocation");
  }

  allocatedBlocks_.erase(it);
  freeBlocks_[allocation.offset] = allocation.size;
  coalesce(allocation.offset);
}

bool BufferManager::canAllocate(uint64_t size) const
{
  if (size == 0u)
    return false;

  for (const auto &[offset, blockSize] : freeBlocks_)
  {
    (void)offset;
    if (blockSize >= size)
      return true;
  }

  return false;
}

uint64_t BufferManager::getTotalSize() const
{
  return totalSize_;
}

uint64_t BufferManager::getAllocatedSize() const
{
  uint64_t total = 0;
  for (const auto &block : allocatedBlocks_)
  {
    total += block.second;
  }
  return total;
}

void BufferManager::coalesce(uint64_t offset)
{
  auto current = freeBlocks_.find(offset);
  if (current == freeBlocks_.end())
    return;

  uint64_t currentEnd = current->first + current->second;
  auto next = freeBlocks_.find(currentEnd);
  if (next != freeBlocks_.end())
  {
    current->second += next->second;
    freeBlocks_.erase(next);
  }

  if (current != freeBlocks_.begin())
  {
    auto prev = std::prev(current);
    uint64_t prevEnd = prev->first + prev->second;
    if (prevEnd == current->first)
    {
      prev->second += current->second;
      freeBlocks_.erase(current);
    }
  }
}
