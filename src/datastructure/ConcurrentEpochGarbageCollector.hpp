#pragma once

#include "MarkedAtomicPointer.hpp"
#include "ThreadLocalStorage.hpp"
#include "algorithm/bit.hpp"
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
    std::atomic<Epoch> epoch;
    T data;

    template <typename... Args, typename = std::enable_if_t<std::is_constructible_v<T, Args...>>>
    explicit Allocation(Epoch e, Args &&...args) : next(nullptr), epoch(e), data(std::forward<Args>(args)...)
    {
    }

    Allocation()
    {
      epoch.store(0, std::memory_order_relaxed);
      new (&data) T();
    }
  };

  struct alignas(64) ThreadRecord
  {
  public:
    std::atomic<ThreadRecord *> next;
    std::atomic<bool> active;
    std::atomic<uint32_t> refCount;

    char pad[256 - sizeof(bool) - sizeof(ThreadRecord *) - sizeof(std::atomic<uint32_t>)];

    std::atomic<Epoch> epoch;
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

      if (is_active)
      {
        Epoch e = curr->epoch.load(std::memory_order_relaxed);
        if (e > 0 && e < m)
        {
          m = e;
        }
      }

      curr = curr->next.load();
    }

    return m;
  }

  void release(ThreadRecord *record)
  {
    if (record->retiredSize < 16)
    {
      return;
    }

    globalEpoch.fetch_add(1);

    uint64_t minimum = minimumEpoch();
    while (record->retiredListHead != nullptr && record->retiredListHead->epoch.load(std::memory_order_relaxed) < minimum)
    {
      auto curr = record->retiredListHead;
      record->retiredListHead = curr->next;
      curr->next = nullptr;
      record->retiredSize -= 1;
      curr->data.~T();

#ifdef CONCURRENT_EGC_DEBUG_LOG
      os::print("[%u] freeing %p freed at epoch %u during epoch %u\n", os::Thread::getCurrentThreadId(), curr, curr->epoch.load(std::memory_order_relaxed), minimum);
#endif

      if (record->cacheSize < CacheSize)
      {
        curr->next = record->cache;
        record->cache = curr;
        record->cacheSize += 1;
      }
      else
      {
        free(static_cast<void *>(curr));
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

  int random16()
  {
    static thread_local std::mt19937 generator(std::random_device{}());
    uint32_t r = generator();
    r |= (1U << (16 - 1));
    int level = countrZero(r) + 1;
    return level;
  }

public:
  struct EpochGuard
  {
    friend class ConcurrentEpochGarbageCollector<T, CacheSize>;

  private:
    ThreadRecord *record;
    ConcurrentEpochGarbageCollector<T, CacheSize> *gc;

  public:
    EpochGuard(ThreadRecord *r, ConcurrentEpochGarbageCollector<T, CacheSize> *gc) : record(r), gc(gc)
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
      allocation->epoch.store(gc->globalEpoch.load(), std::memory_order_relaxed);
#ifdef CONCURRENT_EGC_DEBUG_LOG
      os::print("[%u] retiring %p\n", os::Thread::getCurrentThreadId(), allocation);
#endif
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
      newNode->epoch.store(0, std::memory_order_relaxed);
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

    recordsCache[0].epoch.store(UINT64_MAX, std::memory_order_relaxed);

    head = &recordsCache[0];
  }

  ~ConcurrentEpochGarbageCollector()
  {
    ThreadRecord *curr = head->next.load();

    while (curr != nullptr)
    {
      assert(curr->active.load() == false);

      auto succ = curr->next.load();

      while (curr->retiredListHead)
      {
        auto h = curr->retiredListHead;
        auto next = h->next;
        h->next = nullptr;
        h->data.~T();
#ifdef CONCURRENT_EGC_DEBUG_LOG
        os::print("[%u] freeing %p\n", os::Thread::getCurrentThreadId(), h);
#endif
        free(static_cast<void *>(h));

        curr->retiredListHead = next;
      }

      curr->retiredListTail = nullptr;

      while (curr->cache)
      {
        auto next = curr->cache->next;
        curr->cache->data.~T();
        free(static_cast<void *>(curr->cache));
        curr->cache = next;
      }

      ThreadRecord *begin = recordsCache;
      ThreadRecord *end = recordsCache + localCacheCapacity;

      if (curr < begin || curr >= end)
      {
        free(static_cast<void *>(curr));
      }

      curr = succ;
    }

    delete[] recordsCache;

    recordsCache = nullptr;
    head = nullptr;
  }

  EpochGuard openEpochGuard()
  {
  retry:
    ThreadRecord *newNode = nullptr;

    if (localCache.get(newNode))
    {
      bool expected = false;
      if (newNode->active.load() == false && newNode->active.compare_exchange_strong(expected, true, std::memory_order_release, std::memory_order_acquire))
      {
        newNode->epoch.store(globalEpoch.load(), std::memory_order_relaxed);
        return EpochGuard(newNode, this);
      }

      newNode = nullptr;
    }

    ThreadRecord *curr = head->next.load();

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
      newNode->active.store(true);

      ThreadRecord *oldNext = head->next.load();

      do
      {
        newNode->next.store(oldNext);
      } while (!head->next.compare_exchange_weak(oldNext, newNode));

      capacity.fetch_add(1);
    }

    newNode->epoch.store(globalEpoch.load(), std::memory_order_relaxed);
    localCache.set(newNode);

    if (random16() > 8)
    {
      flush();
    }

    return EpochGuard(newNode, this);
  }

  void flush()
  {
    ThreadRecord *record = head->next.load();
    uint64_t minimum = minimumEpoch();
    globalEpoch.fetch_add(1);

    while (record != nullptr)
    {
      auto succ = record->next.load();
      bool expected = false;

      if (record->active.load() == false && record->active.compare_exchange_strong(expected, true, std::memory_order_release, std::memory_order_acquire))
      {
        while (record->retiredListHead != nullptr && record->retiredListHead->epoch.load(std::memory_order_relaxed) < minimum)
        {
          auto curr = record->retiredListHead;
          record->retiredListHead = record->retiredListHead->next;
          record->retiredSize -= 1;

          curr->data.~T();
#ifdef CONCURRENT_EGC_DEBUG_LOG
          os::print("[%u] freeing %p freed at epoch %u during epoch %u\n", os::Thread::getCurrentThreadId(), curr, curr->epoch.load(std::memory_order_relaxed), minimum);
#endif
          if (record->cacheSize < CacheSize)
          {
            curr->next = record->cache;
            record->cache = curr;
            record->cacheSize += 1;
          }
          else
          {
            free(static_cast<void *>(curr));
          }
        }

        if (record->retiredListHead == nullptr)
        {
          record->retiredListTail = nullptr;
        }

        record->active.store(false);
      }

      record = succ;
    }
  }

  T *allocateUnitialized(EpochGuard &scope)
  {
    // if (scope.record->cache != nullptr)
    // {
    //   Allocation *reused = scope.record->cache;
    //   scope.record->cache = reused->next;
    //   scope.record->cacheSize -= 1;

    //   reused->epoch.store(UINT64_MAX, std::memory_order_relaxed);
    //   new (reused)(Allocation);
    //   return &reused->data;
    // }

    Allocation *allocation = new Allocation(UINT64_MAX); // (Allocation *)malloc(sizeof(Allocation));
#ifdef CONCURRENT_EGC_DEBUG_LOG
    os::print("[%u] allocating %p\n", os::Thread::getCurrentThreadId(), allocation);
#endif

    return &allocation->data;
  }

  template <typename... Args> T *allocate(EpochGuard &scope, Args &&...args)
  {
    if (scope.record->cache != nullptr)
    {
      Allocation *reused = scope.record->cache;
      scope.record->cache = reused->next;
      scope.record->cacheSize -= 1;

      new (reused) Allocation(UINT64_MAX, std::forward<Args>(args)...);
#ifdef CONCURRENT_EGC_DEBUG_LOG
      os::print("[%u] allocating %p\n", os::Thread::getCurrentThreadId(), reused);
#endif
      return &reused->data;
    }

    static_assert(std::is_constructible_v<T, Args...>, "T must be constructible from the provided arguments");
    Allocation *allocation = new Allocation(UINT64_MAX, std::forward<Args>(args)...);
#ifdef CONCURRENT_EGC_DEBUG_LOG
    os::print("[%u] allocating %p\n", os::Thread::getCurrentThreadId(), allocation);
#endif
    return &allocation->data;
  }
};

template <typename T, uint32_t C> typename ConcurrentEpochGarbageCollector<T, C>::EpochGuard nullGuard = {nullptr, nullptr};

} // namespace lib