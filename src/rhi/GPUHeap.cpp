#include "GPUHeap.hpp"
#include <algorithm> // for std::sort if you use it elsewhere
#include <cassert>

namespace rhi
{
GPUBuffer::GPUBuffer() : heap(nullptr), offset(0), size(0)
{
}
GPUBuffer::GPUBuffer(GPUHeap *h, uint64_t o, uint64_t s) : heap(h), offset(o), size(s)
{
}
bool GPUBuffer::isValid() const
{
  return heap != nullptr && size > 0;
}

struct GPUHeap::FreeNode
{
  std::atomic<uint64_t> offset_size;
  std::atomic<FreeNode *> next;

  FreeNode() : offset_size(0), next(nullptr)
  {
  }
  // note: explicitly delete copy/move to be clear (optional)
  FreeNode(const FreeNode &) = delete;
  FreeNode(FreeNode &&) = delete;
  FreeNode &operator=(const FreeNode &) = delete;
  FreeNode &operator=(FreeNode &&) = delete;

  uint64_t getOffset() const
  {
    uint64_t packed = offset_size.load(std::memory_order_acquire);
    return packed >> 32;
  }

  uint64_t getSize() const
  {
    uint64_t packed = offset_size.load(std::memory_order_acquire);
    return packed & 0xFFFFFFFF;
  }

  void setOffsetSize(uint64_t offset, uint64_t size)
  {
    uint64_t packed = (offset << 32) | (size & 0xFFFFFFFF);
    offset_size.store(packed, std::memory_order_release);
  }

  bool isEmpty() const
  {
    return offset_size.load(std::memory_order_acquire) == 0;
  }
};

struct GPUHeap::FreeList
{
  std::atomic<FreeNode *> head;

  FreeList() : head(nullptr)
  {
  }

  void push(FreeNode *node)
  {
    FreeNode *oldHead = head.load(std::memory_order_acquire);
    do
    {
      node->next.store(oldHead, std::memory_order_relaxed);
    } while (!head.compare_exchange_weak(oldHead, node, std::memory_order_release, std::memory_order_acquire));
  }

  FreeNode *pop()
  {
    FreeNode *oldHead = head.load(std::memory_order_acquire);
    FreeNode *newHead;
    do
    {
      if (oldHead == nullptr)
      {
        return nullptr;
      }
      newHead = oldHead->next.load(std::memory_order_acquire);
    } while (!head.compare_exchange_weak(oldHead, newHead, std::memory_order_release, std::memory_order_acquire));
    return oldHead;
  }

  bool empty() const
  {
    return head.load(std::memory_order_acquire) == nullptr;
  }
};

GPUHeap::GPUHeap(uint64_t totalSize) : totalSize_(totalSize), usedSize_(0)
{
  nodePool_.reset(new FreeNode[MAX_NODES]);

  freeNodePool_ = new FreeList();
  freeList_ = new FreeList();

  // push nodes [0 .. MAX_NODES-2] into free node pool (leave last as reserved maybe)
  for (size_t i = 0; i < MAX_NODES - 1; ++i)
  {
    freeNodePool_->push(&nodePool_[i]);
  }

  FreeNode *initialNode = allocateNode();
  if (initialNode)
  {
    initialNode->setOffsetSize(0, totalSize_);
    freeList_->push(initialNode);
  }
}

GPUHeap::~GPUHeap()
{
  delete freeNodePool_;
  delete freeList_;
}

GPUHeap::GPUHeapStatus GPUHeap::allocate(size_t size, GPUBuffer &out)
{
  return allocate(size, 1, out);
}

GPUHeap::GPUHeapStatus GPUHeap::allocate(size_t requestedSize, size_t alignment, GPUBuffer &out)
{
  if (requestedSize == 0)
    return ERROR;

  uint64_t alignedSize = alignUp(requestedSize, alignment);

  FreeNode *bestNode = nullptr;
  uint64_t bestPacked = 0;

  FreeNode *current = freeList_->head.load(std::memory_order_acquire);

  while (current)
  {
    uint64_t packed = current->offset_size.load(std::memory_order_acquire);
    uint64_t blockOffset = packed >> 32;
    uint64_t blockSize = packed & 0xFFFFFFFFULL;

    if (packed != 0 && blockSize >= alignedSize)
    {
      uint64_t alignedOffset = alignUp(blockOffset, alignment);
      uint64_t padding = alignedOffset - blockOffset;
      uint64_t totalNeeded = padding + alignedSize;

      if (blockSize >= totalNeeded)
      {
        uint64_t removedPacked = 0;
        if (tryRemoveFromFreeList(current, removedPacked))
        {
          // removedPacked contains the node's original packed value
          bestNode = current;
          bestPacked = removedPacked;
          break;
        }
      }
    }

    current = current->next.load(std::memory_order_acquire);
  }

  if (!bestNode)
    return OUT_OF_MEMORY;

  uint64_t blockOffset = bestPacked >> 32;
  uint64_t blockSize = bestPacked & 0xFFFFFFFFULL;
  uint64_t alignedOffset = alignUp(blockOffset, alignment);
  uint64_t padding = alignedOffset - blockOffset;
  uint64_t totalUsed = padding + alignedSize;

  if (padding > 0)
  {
    FreeNode *paddingNode = allocateNode();
    if (paddingNode)
    {
      paddingNode->setOffsetSize(blockOffset, padding);
      freeList_->push(paddingNode);
    }
  }

  uint64_t remainder = blockSize - totalUsed;
  if (remainder > 0)
  {
    FreeNode *remainderNode = allocateNode();
    if (remainderNode)
    {
      remainderNode->setOffsetSize(alignedOffset + alignedSize, remainder);
      freeList_->push(remainderNode);
    }
  }

  freeNode(bestNode);
  usedSize_.fetch_add(alignedSize, std::memory_order_relaxed);

  out = GPUBuffer(this, alignedOffset, alignedSize);
  return OK;
}

void GPUHeap::free(GPUBuffer &buffer)
{
  if (!buffer.isValid() || buffer.heap != this)
    return;

  uint64_t offset = buffer.offset;
  uint64_t size = buffer.size;

  usedSize_.fetch_sub(size, std::memory_order_relaxed);

  FreeNode *newNode = allocateNode();
  if (!newNode)
    return;

  newNode->setOffsetSize(offset, size);
  attemptMerge(newNode);
  freeList_->push(newNode);

  buffer.heap = nullptr;
  buffer.offset = 0;
  buffer.size = 0;
}

uint64_t GPUHeap::getTotalSize() const
{
  return totalSize_;
}
uint64_t GPUHeap::getUsedSize() const
{
  return usedSize_.load(std::memory_order_acquire);
}
uint64_t GPUHeap::getFreeSize() const
{
  return totalSize_ - getUsedSize();
}

bool GPUHeap::hasAvailableNodes() const
{
  return !freeNodePool_->empty();
}

size_t GPUHeap::getApproximateFreeBlockCount() const
{
  size_t count = 0;
  FreeNode *current = freeList_->head.load(std::memory_order_acquire);
  while (current && count < 100)
  {
    if (!current->isEmpty())
      ++count;
    current = current->next.load(std::memory_order_acquire);
  }
  return count;
}

uint64_t GPUHeap::alignUp(uint64_t value, uint64_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

GPUHeap::FreeNode *GPUHeap::allocateNode()
{
  return freeNodePool_->pop();
}

void GPUHeap::freeNode(FreeNode *node)
{
  node->setOffsetSize(0, 0);
  node->next.store(nullptr, std::memory_order_relaxed);
  freeNodePool_->push(node);
}

bool GPUHeap::tryRemoveFromFreeList(FreeNode *target, uint64_t &outPacked)
{
  uint64_t expected = target->offset_size.load(std::memory_order_acquire);
  if (expected == 0)
    return false; // already removed

  if (target->offset_size.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
  {
    outPacked = expected;
    return true;
  }

  return false;
}

void GPUHeap::attemptMerge(FreeNode *newNode)
{
  uint64_t newOffset = newNode->getOffset();
  uint64_t newSize = newNode->getSize();

  FreeNode *current = freeList_->head.load(std::memory_order_acquire);
  while (current)
  {
    if (current->isEmpty())
    {
      current = current->next.load(std::memory_order_acquire);
      continue;
    }

    uint64_t currentOffset = current->getOffset();
    uint64_t currentSize = current->getSize();

    if (currentOffset + currentSize == newOffset)
    {
      uint64_t expectedPackedValue = current->offset_size.load(std::memory_order_acquire);
      uint64_t newPackedValue = (currentOffset << 32) | ((currentSize + newSize) & 0xFFFFFFFF);

      if (current->offset_size.compare_exchange_strong(expectedPackedValue, newPackedValue, std::memory_order_acq_rel))
      {
        freeNode(newNode);
        return;
      }
    }
    else if (newOffset + newSize == currentOffset)
    {
      newNode->setOffsetSize(newOffset, newSize + currentSize);
      current->offset_size.store(0, std::memory_order_release);
      return;
    }

    current = current->next.load(std::memory_order_acquire);
  }
}

BuddyGPUHeap::BuddyGPUHeap(uint64_t totalSize) : GPUHeap(totalSize)
{
  maxOrder_ = 64 - __builtin_clzll(totalSize - 1);
  actualSize_ = 1ULL << maxOrder_;

  freeLists_.resize(maxOrder_ + 1);
  for (auto &list : freeLists_)
    list = new FreeList();

  FreeNode *rootNode = allocateNode();
  if (rootNode)
  {
    rootNode->setOffsetSize(0, actualSize_);
    freeLists_[maxOrder_]->push(rootNode);
  }
}

BuddyGPUHeap::~BuddyGPUHeap()
{
  for (auto p : freeLists_)
    delete p;
  freeLists_.clear();
}

GPUHeap::GPUHeapStatus BuddyGPUHeap::allocate(size_t size, GPUBuffer &out)
{
  if (size == 0)
    return ERROR;

  int order = getOrder(size);
  if (order > maxOrder_)
    return OUT_OF_MEMORY;

  FreeNode *node = findFreeBlock(order);
  if (!node)
    return OUT_OF_MEMORY;

  uint64_t allocSize = 1ULL << order;
  usedSize_.fetch_add(allocSize, std::memory_order_relaxed);

  out = GPUBuffer(this, node->getOffset(), allocSize);
  return OK;
}

int BuddyGPUHeap::getOrder(uint64_t size)
{
  if (size <= 1)
    return 0;
  return 64 - __builtin_clzll(size - 1);
}

GPUHeap::FreeNode *BuddyGPUHeap::findFreeBlock(int order)
{
  FreeNode *node = freeLists_[order]->pop();
  if (node)
    return node;

  for (int largerOrder = order + 1; largerOrder <= maxOrder_; ++largerOrder)
  {
    FreeNode *largerNode = freeLists_[largerOrder]->pop();
    if (largerNode)
    {
      return splitBlock(largerNode, largerOrder, order);
    }
  }
  return nullptr;
}

GPUHeap::FreeNode *BuddyGPUHeap::splitBlock(GPUHeap::FreeNode *node, int fromOrder, int toOrder)
{
  while (fromOrder > toOrder)
  {
    uint64_t offset = node->getOffset();
    uint64_t size = 1ULL << (fromOrder - 1);

    FreeNode *buddy = allocateNode();
    if (!buddy)
      break;

    buddy->setOffsetSize(offset + size, size);
    node->setOffsetSize(offset, size);

    freeLists_[fromOrder - 1]->push(buddy);
    --fromOrder;
  }
  return node;
}

} // namespace rhi
