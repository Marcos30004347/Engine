// #pragma once

// #include "rhi/Device.hpp"
// #include <atomic>
// #include <cstdint>
// #include <mutex>
// #include <vector>

// namespace rhi
// {

// struct FreeBlock
// {
//   uint64_t offset;
//   uint64_t size;

//   FreeBlock(uint64_t off, uint64_t sz);
//   FreeBlock();

//   bool operator==(const FreeBlock &other) const;
//   bool operator!=(const FreeBlock &other) const;
// };

// enum BufferAllocateStatus
// {
//   BufferAllocateStatus_Ok,
//   BufferAllocateStatus_Error,
//   BufferAllocateStatus_OutOfMemory
// };

// class BufferAllocator
// {
// public:
//   BufferAllocator(Device *device, uint64_t offset, uint64_t size);
//   virtual ~BufferAllocator();

//   BufferAllocateStatus allocate(size_t size, BufferViewInfo &out);
//   BufferAllocateStatus allocate(size_t requestedSize, size_t alignment, BufferViewInfo &out);

//   void free(BufferViewInfo &buffer);

//   uint64_t getTotalSize() const;
//   uint64_t getUsedSize() const;
//   uint64_t getFreeSize() const;

//   bool hasAvailableBlocks() const;
//   size_t getApproximateFreeBlockCount() const;

// private:
//   Device *device_;
//   uint64_t totalSize_;
//   uint64_t baseOffset_;
//   std::atomic<uint64_t> usedSize_;
//   std::vector<FreeBlock> freeBlocks_;
//   mutable std::mutex mutex_;

//   static uint64_t alignUp(uint64_t value, uint64_t alignment);
//   void insertFreeBlock(const FreeBlock &block);
// };

// } // namespace rhi
