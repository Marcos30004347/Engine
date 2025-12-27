#pragma once

#include "MarkedAtomicPointer.hpp"
#include "ThreadLocalStorage.hpp"
#include "os/Thread.hpp"
#include "os/print.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace lib
{

template <typename T, uint32_t CacheSize = 8> class ConcurrentEpochGarbageCollector
{
  friend struct EpochGuard;

private:

public:
  using Epoch = uint64_t;
  struct Allocation
  {
    Allocation *next;
    Epoch epoch;
    T data;

    template <typename... Args, typename = std::enable_if_t<std::is_constructible_v<T, Args...>>>
    explicit Allocation(Epoch e, Args &&...args) : next(nullptr), epoch(e), data(std::forward<Args>(args)...)
    {
    }
  };

  struct alignas(64) ThreadRecord
  {
  public:
    std::atomic<ThreadRecord *> next;
    std::atomic<bool> active;
    std::atomic<uint32_t> refCount;

    char pad[256 - sizeof(bool) - sizeof(ThreadRecord *) - sizeof(std::atomic<uint32_t>)];

    Epoch epoch;
    uint64_t retiredSize;
    Allocation *retiredListHead;
    Allocation *retiredListTail;

    Allocation *cache;
    uint64_t cacheSize;

    void free(Allocation *ptr)
    {
      ptr->next = nullptr;

      if (retiredListTail == nullptr)
      {
        retiredListTail = ptr;
      }
      else
      {
        retiredListTail->next = ptr;
        retiredListTail = ptr;
      }
      if (retiredListHead == nullptr)
      {
        retiredListHead = retiredListTail;
      }

      retiredSize += 1;
    }
  };
  ThreadLocalStorage<ThreadRecord *> localCache;

  std::atomic<uint64_t> capacity;
  std::atomic<Epoch> globalEpoch;

  ThreadRecord *head;

  std::atomic<Allocation *> allocationsHead;

  uint32_t localCacheCapacity;
  ThreadRecord *recordsCache;

  uint64_t minimumEpoch()
  {
  retry:
    uint64_t m = UINT64_MAX;

    auto curr = head->next.load();

    while (curr)
    {
      bool is_active = curr->active.load(std::memory_order_acquire);

      if (is_active && curr->epoch > 0 && curr->epoch < m)
      {
        m = curr->epoch;
      }

      curr = curr->next.load();
    }

    return m;
  }

  void release(ThreadRecord *record, bool ignoreThreshold = false)
  {
    if (record->retiredSize < 16 && !ignoreThreshold)
    {
      return;
    }

    globalEpoch.fetch_add(1);

    uint64_t minimum = minimumEpoch();

    while (record->retiredListHead != nullptr && record->retiredListHead->epoch < minimum)
    {
      auto curr = record->retiredListHead;
      record->retiredListHead = record->retiredListHead->next;
      record->retiredSize -= 1;
      curr->data.~T();
      // os::print("freeing %p freed at epoch %u during epoch %u\n", curr, curr->epoch, minimum);

      if (record->cacheSize < CacheSize)
      {
        curr->next = record->cache;
        record->cache = curr;
        record->cacheSize += 1;
      }
      else
      {
        delete curr;
      }
    }

    if (record->retiredListHead == nullptr)
    {
      record->retiredListTail = nullptr;
    }
  }

  void releaseThreadRecord(ThreadRecord *record)
  {
    release(record);

    assert(record->active.load() == true);
    record->active.store(false);
  }

  inline bool shouldRunRelease()
  {
    static thread_local uint32_t rng = 0x12345678u ^ os::Thread::getCurrentThreadId();
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (rng % (2 * os::Thread::getHardwareConcurrency())) == 0;
  }

public:
  struct EpochGuard
  {
    friend class ConcurrentEpochGarbageCollector<T, CacheSize>;

  private:
    ThreadRecord *record;
    ConcurrentEpochGarbageCollector<T> *gc;

  public:
    EpochGuard(ThreadRecord *r, ConcurrentEpochGarbageCollector<T> *gc) : record(r), gc(gc)
    {
      if (record)
      {
        record->refCount.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // Copy constructor
    EpochGuard(const EpochGuard &other) : record(other.record), gc(other.gc)
    {
      if (record)
      {
        record->refCount.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // Copy assignment
    EpochGuard &operator=(const EpochGuard &other)
    {
      if (this != &other)
      {
        // Release current record
        if (record)
        {
          uint32_t oldCount = record->refCount.fetch_sub(1, std::memory_order_acq_rel);
          if (oldCount == 1)
          {
            gc->releaseThreadRecord(record);
          }
        }

        // Acquire new record
        record = other.record;
        gc = other.gc;
        if (record)
        {
          record->refCount.fetch_add(1, std::memory_order_relaxed);
        }
      }
      return *this;
    }

    // Move constructor
    EpochGuard(EpochGuard &&other) noexcept : record(other.record), gc(other.gc)
    {
      other.record = nullptr;
      other.gc = nullptr;
    }

    // Move assignment
    EpochGuard &operator=(EpochGuard &&other) noexcept
    {
      if (this != &other)
      {
        // Release current record
        if (record)
        {
          uint32_t oldCount = record->refCount.fetch_sub(1, std::memory_order_acq_rel);
          if (oldCount == 1)
          {
            gc->releaseThreadRecord(record);
          }
        }

        // Take ownership from other
        record = other.record;
        gc = other.gc;
        other.record = nullptr;
        other.gc = nullptr;
      }
      return *this;
    }

    ~EpochGuard()
    {
      if (record)
      {
        uint32_t oldCount = record->refCount.fetch_sub(1, std::memory_order_acq_rel);
        if (oldCount == 1)
        {
          gc->releaseThreadRecord(record);
        }
      }
    }

    void retire(T *ptr)
    {
      auto allocation = reinterpret_cast<Allocation *>(reinterpret_cast<char *>(ptr) - offsetof(Allocation, data));
      allocation->epoch = gc->globalEpoch.load();
      record->free(allocation);
    }

    void clear()
    {
      if (record)
      {
        uint32_t oldCount = record->refCount.fetch_sub(1, std::memory_order_acq_rel);
        if (oldCount == 1)
        {
          gc->releaseThreadRecord(record);
        }
        record = nullptr;
      }
    }
  };

  ConcurrentEpochGarbageCollector(uint64_t initialRecordsSize = -1) : allocationsHead(nullptr), globalEpoch(1), capacity(0)
  {
    if (initialRecordsSize == -1)
    {
      initialRecordsSize = 2 * os::Thread::getHardwareConcurrency();
    }

    localCacheCapacity = initialRecordsSize + 1;

    recordsCache = new ThreadRecord[initialRecordsSize + 1];

    for (uint64_t i = 0; i < initialRecordsSize + 1; i++)
    {
      auto newNode = &recordsCache[i];

      newNode->retiredSize = 0;
      newNode->epoch = 0;
      newNode->retiredListHead = nullptr;
      newNode->retiredListTail = nullptr;
      newNode->active.store(false);
      newNode->refCount.store(0, std::memory_order_relaxed);
      newNode->cache = nullptr;
      newNode->cacheSize = 0;

      if (i + 1 <= initialRecordsSize)
      {
        newNode->next.store(&recordsCache[i + 1]);
      }
      else
      {
        newNode->next.store(nullptr);
      }
    }

    capacity.fetch_add(initialRecordsSize);

    recordsCache[0].epoch = UINT64_MAX;

    head = &recordsCache[0];
  }

  ~ConcurrentEpochGarbageCollector()
  {
    ThreadRecord *curr = head->next.load();
    ThreadRecord *newNode = nullptr;

    while (curr != nullptr)
    {
      auto succ = curr->next.load();
      release(curr, true);

      while (curr->cache)
      {
        auto next = curr->cache->next;
        curr->cache->data.~T();
        delete curr->cache;
        curr->cache = next;
      }

      ThreadRecord *begin = recordsCache;
      ThreadRecord *end = recordsCache + localCacheCapacity;

      if (curr < begin || curr >= end)
      {
        delete curr; 
      }
   

      curr = succ;
    }

    delete recordsCache;
  }

  EpochGuard openEpochGuard()
  {
  retry:
    ThreadRecord *curr = head->next.load();
    ThreadRecord *newNode = nullptr;

    if (localCache.get(newNode))
    {
      bool expected = false;
      if (newNode->active.load() == false && newNode->active.compare_exchange_strong(expected, true, std::memory_order_release, std::memory_order_acquire))
      {
        return EpochGuard(newNode, this);
      }

      newNode = nullptr;
    }

    while (curr != nullptr)
    {
      bool expected = false;

      if (curr->active.load() == false && curr->active.compare_exchange_strong(expected, true, std::memory_order_release, std::memory_order_acquire))
      {
        newNode = curr;
        break;
      }

      curr = curr->next.load();
    }

    if (newNode == nullptr)
    {
      newNode = new ThreadRecord();
      newNode->retiredSize = 0;
      newNode->retiredListHead = nullptr;
      newNode->retiredListTail = nullptr;
      newNode->refCount.store(0, std::memory_order_relaxed);
      newNode->cache = nullptr;
      newNode->cacheSize = 0;

      ThreadRecord *oldNext = head->next.load();
      do
      {
        newNode->next.store(oldNext);
      } while (!head->next.compare_exchange_weak(oldNext, newNode));

      capacity.fetch_add(1);
    }

    newNode->epoch = globalEpoch.load();
    localCache.set(newNode);

    return EpochGuard(newNode, this);
  }

  void flush()
  {
    ThreadRecord *curr = head->next.load();
    ThreadRecord *newNode = nullptr;

    while (curr != nullptr)
    {
      auto succ = curr->next.load();
      bool expected = false;
      if (curr->active.load() == false && curr->active.compare_exchange_strong(expected, true, std::memory_order_release, std::memory_order_acquire))
      {
        release(curr, true);
        curr->active.store(false);
      }
      curr = succ;
    }
  }

  template <typename... Args> T *allocate(EpochGuard &scope, Args &&...args)
  {
    if (scope.record->cache != nullptr)
    {
      Allocation *reused = scope.record->cache;
      scope.record->cache = reused->next;
      new (reused) Allocation(UINT64_MAX, std::forward<Args>(args)...);
      return &reused->data;
    }

    static_assert(std::is_constructible_v<T, Args...>, "T must be constructible from the provided arguments");
    Allocation *allocation = new Allocation(UINT64_MAX, std::forward<Args>(args)...);
    return &allocation->data;
  }
};

} // namespace lib