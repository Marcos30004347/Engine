#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory> 
#include <vector>

namespace rhi
{

class GPUHeap;

class GPUBuffer
{
public:
  GPUHeap *heap;
  uint64_t offset;
  uint64_t size;

  GPUBuffer();
  GPUBuffer(GPUHeap *h, uint64_t o, uint64_t s);

  bool isValid() const;
};

class GPUHeap
{
public:
  enum GPUHeapStatus
  {
    OK,
    ERROR,
    OUT_OF_MEMORY
  };

  explicit GPUHeap(uint64_t totalSize);
  virtual ~GPUHeap();

  virtual GPUHeapStatus allocate(size_t size, GPUBuffer &out);
  virtual GPUHeapStatus allocate(size_t requestedSize, size_t alignment, GPUBuffer &out);

  void free(GPUBuffer &buffer);

  uint64_t getTotalSize() const;
  uint64_t getUsedSize() const;
  uint64_t getFreeSize() const;

  bool hasAvailableNodes() const;
  size_t getApproximateFreeBlockCount() const;

protected:
  struct FreeNode;
  struct FreeList;

  static constexpr size_t MAX_NODES = 8192;

  uint64_t totalSize_;
  std::atomic<uint64_t> usedSize_;

  // >>> changed: use a raw array managed by unique_ptr to avoid vector relocation
  std::unique_ptr<FreeNode[]> nodePool_;
  FreeList *freeNodePool_;
  FreeList *freeList_;

  static uint64_t alignUp(uint64_t value, uint64_t alignment);

  FreeNode *allocateNode();
  void freeNode(FreeNode *node);
  
  bool tryRemoveFromFreeList(FreeNode* target, uint64_t &outPacked);
  void attemptMerge(FreeNode *newNode);
};

// Buddy allocator variant
class BuddyGPUHeap : public GPUHeap
{
public:
  explicit BuddyGPUHeap(uint64_t totalSize);
  ~BuddyGPUHeap() override;

  GPUHeapStatus allocate(size_t size, GPUBuffer &out) override;

private:
  int maxOrder_;
  uint64_t actualSize_;

  std::vector<FreeList *> freeLists_;

  static int getOrder(uint64_t size);
  GPUHeap::FreeNode *findFreeBlock(int order);
  GPUHeap::FreeNode *splitBlock(GPUHeap::FreeNode *node, int fromOrder, int toOrder);
};

} // namespace rhi
