// #include "BufferAllocator.hpp"
// #include <algorithm>

// namespace rhi
// {

// FreeBlock::FreeBlock(uint64_t off, uint64_t sz) : offset(off), size(sz)
// {
// }
// FreeBlock::FreeBlock() : offset(0), size(0)
// {
// }

// bool FreeBlock::operator==(const FreeBlock &other) const
// {
//   return offset == other.offset && size == other.size;
// }

// bool FreeBlock::operator!=(const FreeBlock &other) const
// {
//   return !(*this == other);
// }

// BufferAllocator::BufferAllocator(Device *device, uint64_t offset, uint64_t size) : device_(device), baseOffset_(offset), totalSize_(size), usedSize_(0)
// {
//   freeBlocks_.push_back({offset, size});
// }

// BufferAllocator::~BufferAllocator() = default;

// BufferAllocateStatus BufferAllocator::allocate(size_t size, BufferViewInfo &out)
// {
//   return allocate(size, 1, out);
// }

// BufferAllocateStatus BufferAllocator::allocate(size_t requestedSize, size_t alignment, BufferViewInfo &out)
// {
//   std::lock_guard<std::mutex> lock(mutex_);

//   for (size_t i = 0; i < freeBlocks_.size(); ++i)
//   {
//     uint64_t alignedOffset = alignUp(freeBlocks_[i].offset, alignment);
//     if (alignedOffset + requestedSize <= freeBlocks_[i].offset + freeBlocks_[i].size)
//     {
//       out.offset = alignedOffset;
//       out.size = requestedSize;
//       out.name.clear(); // optional

//       uint64_t used = alignedOffset + requestedSize - freeBlocks_[i].offset;
//       usedSize_ += requestedSize;

//       if (used < freeBlocks_[i].size)
//       {
//         freeBlocks_[i].offset += used;
//         freeBlocks_[i].size -= used;
//       }
//       else
//       {
//         freeBlocks_.erase(freeBlocks_.begin() + i);
//       }

//       return BufferAllocateStatus_Ok;
//     }
//   }

//   return BufferAllocateStatus_OutOfMemory;
// }

// void BufferAllocator::free(BufferViewInfo &buffer)
// {
//   std::lock_guard<std::mutex> lock(mutex_);
//   FreeBlock block{buffer.offset, buffer.size};
//   usedSize_ -= buffer.size;
//   insertFreeBlock(block);
// }

// uint64_t BufferAllocator::getTotalSize() const
// {
//   return totalSize_;
// }
// uint64_t BufferAllocator::getUsedSize() const
// {
//   return usedSize_.load();
// }
// uint64_t BufferAllocator::getFreeSize() const
// {
//   return totalSize_ - usedSize_.load();
// }

// bool BufferAllocator::hasAvailableBlocks() const
// {
//   std::lock_guard<std::mutex> lock(mutex_);
//   return !freeBlocks_.empty();
// }

// size_t BufferAllocator::getApproximateFreeBlockCount() const
// {
//   std::lock_guard<std::mutex> lock(mutex_);
//   return freeBlocks_.size();
// }

// // ---------------- Private ----------------

// uint64_t BufferAllocator::alignUp(uint64_t value, uint64_t alignment)
// {
//   return (value + alignment - 1) & ~(alignment - 1);
// }

// void BufferAllocator::insertFreeBlock(const FreeBlock &block)
// {
//   auto it = std::lower_bound(
//       freeBlocks_.begin(),
//       freeBlocks_.end(),
//       block,
//       [](const FreeBlock &a, const FreeBlock &b)
//       {
//         return a.offset < b.offset;
//       });
//   freeBlocks_.insert(it, block);

//   for (size_t i = 0; i + 1 < freeBlocks_.size();)
//   {
//     if (freeBlocks_[i].offset + freeBlocks_[i].size == freeBlocks_[i + 1].offset)
//     {
//       freeBlocks_[i].size += freeBlocks_[i + 1].size;
//       freeBlocks_.erase(freeBlocks_.begin() + i + 1);
//     }
//     else
//     {
//       ++i;
//     }
//   }
// }

// } // namespace rhi
