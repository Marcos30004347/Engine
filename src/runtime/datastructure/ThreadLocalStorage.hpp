#pragma once

#include <assert.h>
#include <atomic>
#include <cstdint>
#include <new>
#include <thread>
#include <utility>

#include "algorithm/random.hpp"
#include "os/Thread.hpp"
#include "os/print.hpp"

namespace lib
{
namespace detail
{

// Align to cache line to prevent false sharing between metadata and data
template <typename V> struct alignas(256) HashTableBucket
{
  static constexpr size_t INVALID_KEY = SIZE_MAX;

  const size_t capacity;
  const size_t mask;

  // SoA (Structure of Arrays): Keys are separated from values.
  // This dramatically improves cache hits during probing.
  std::atomic<size_t> *keys;
  V *values;

  HashTableBucket<V> *prev;

  HashTableBucket(size_t cap)
      : capacity(cap), mask(cap - 1), keys(new std::atomic<size_t>[cap]), values(new V[cap]) // Default construct values
        ,
        prev(nullptr)
  {
    // Initialize keys to INVALID
    for (size_t i = 0; i < capacity; ++i)
    {
      keys[i].store(INVALID_KEY, std::memory_order_relaxed);
    }
  }

  ~HashTableBucket()
  {
    delete[] keys;
    delete[] values;
  }
};

template <typename Value> class ConcurrentLookupTable
{
private:
  std::atomic<detail::HashTableBucket<Value> *> rootTable;
  std::atomic_flag rootTableResizeInProgress = ATOMIC_FLAG_INIT;
  std::atomic<size_t> count;

  // We hold the initial root instance to avoid heap allocation for the first table
  // but we manage it via pointers.
  detail::HashTableBucket<Value> *initialRoot;

  // Helper to hash integer keys
  static inline size_t hashInteger(size_t k)
  {
    // MurmurHash3 fmix64 finalizer (very fast, high entropy)
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccd;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53;
    k ^= k >> 33;
    return k;
  }

  bool insertInTable(detail::HashTableBucket<Value> *hash, size_t key, const Value &value)
  {
    size_t index = hashInteger(key) & hash->mask;
    const size_t probeLimit = hash->capacity;

    for (size_t i = 0; i < probeLimit; ++i)
    {
      size_t expected = detail::HashTableBucket<Value>::INVALID_KEY;

      // Optimistic check using relaxed ordering before attempting atomic CAS
      if (hash->keys[index].load(std::memory_order_relaxed) == expected)
      {
        if (hash->keys[index].compare_exchange_strong(expected, key, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
          hash->values[index] = value;
          return true;
        }
      }

      // Check if key already exists (update)
      if (hash->keys[index].load(std::memory_order_acquire) == key)
      {
        hash->values[index] = value;
        return true;
      }

      index = (index + 1) & hash->mask;
    }
    return false;
  }

  void resizeTableIfNeeded(size_t newCount)
  {
    detail::HashTableBucket<Value> *currentTable = rootTable.load(std::memory_order_acquire);

    // Load factor > 0.75
    if (newCount >= ((currentTable->capacity * 3) >> 2))
    {
      // Try to acquire resize lock
      if (!rootTableResizeInProgress.test_and_set(std::memory_order_acquire))
      {
        // Double check after lock
        currentTable = rootTable.load(std::memory_order_relaxed);
        if (newCount >= ((currentTable->capacity * 3) >> 2))
        {
          size_t newCapacity = currentTable->capacity << 1;

          // Allocate new table
          auto *newTable = new detail::HashTableBucket<Value>(newCapacity);
          newTable->prev = currentTable;

          rootTable.store(newTable, std::memory_order_release);
        }
        rootTableResizeInProgress.clear(std::memory_order_release);
      }
    }
  }

public:
  ConcurrentLookupTable(size_t initialCapacity = 64)
  {
    assert((initialCapacity & (initialCapacity - 1)) == 0 && "Capacity must be power of 2");
    initialRoot = new detail::HashTableBucket<Value>(initialCapacity);

    count.store(0, std::memory_order_relaxed);
    rootTable.store(initialRoot, std::memory_order_release);
  }

  // Disable copy
  ConcurrentLookupTable(const ConcurrentLookupTable &) = delete;
  ConcurrentLookupTable &operator=(const ConcurrentLookupTable &) = delete;

  // Move Constructor
  ConcurrentLookupTable(ConcurrentLookupTable &&other) noexcept : initialRoot(nullptr)
  {
    rootTable.store(other.rootTable.load(), std::memory_order_relaxed);
    count.store(other.count.load(), std::memory_order_relaxed);
    initialRoot = other.initialRoot;

    other.rootTable.store(nullptr);
    other.initialRoot = nullptr;
    other.count.store(0);
  }

  ConcurrentLookupTable &operator=(ConcurrentLookupTable &&other) noexcept
  {
    if (this != &other)
    {
      this->~ConcurrentLookupTable();
      new (this) ConcurrentLookupTable(std::move(other));
    }
    return *this;
  }

  ~ConcurrentLookupTable()
  {
    detail::HashTableBucket<Value> *hash = rootTable.load(std::memory_order_acquire);

    while (hash != nullptr)
    {
      detail::HashTableBucket<Value> *prev = hash->prev;
      delete hash;
      hash = prev;
    }
  }

  // Optimized GET path
  bool get(size_t id, Value &v)
  {
    size_t hashedId = hashInteger(id);

    // Use acquire to ensure we see the initialized table and its data
    auto currentTable = rootTable.load(std::memory_order_acquire);
    assert(currentTable != nullptr);

    // Walk the table history (usually just 1 iteration)
    for (auto hash = currentTable; hash != nullptr; hash = hash->prev)
    {
      size_t index = hashedId & hash->mask;
      size_t probeLimit = hash->capacity;

      for (size_t i = 0; i < probeLimit; ++i)
      {
        // Load key with acquire to synchronize with the value write
        size_t probedKey = hash->keys[index].load(std::memory_order_acquire);

        if (probedKey == id)
        {
          v = hash->values[index];

          // Migration logic: If we found it in an old table, move it to the new one
          // to speed up future lookups.
          if (hash != currentTable)
          {
            // Try to insert into current. If successful, mark old as deleted (optional optimization)
            // Note: We duplicate the value to the head table.
            // The old value stays in history but won't be found first.
            insertInTable(currentTable, id, v);
          }
          return true;
        }

        if (probedKey == detail::HashTableBucket<Value>::INVALID_KEY)
        {
          // If we hit an empty slot in the CURRENT table, it doesn't exist.
          // If we are in an older table, we must continue searching previous tables.
          if (hash == currentTable)
            return false;
          break; // Break inner loop, go to hash->prev
        }

        index = (index + 1) & hash->mask;
      }
    }

    return false;
  }

  bool insert(size_t id, const Value &val)
  {
    // Pre-increment count to trigger resize early
    size_t newCount = 1 + count.fetch_add(1, std::memory_order_relaxed);

    // This loop handles the rare case where a resize happens *during* our insertion attempt
    while (true)
    {
      resizeTableIfNeeded(newCount);

      auto currentTable = rootTable.load(std::memory_order_acquire);

      if (insertInTable(currentTable, id, val))
      {
        return true;
      }
    }
  }
};

} // namespace detail

template <typename T> class ThreadLocalStorage
{
  detail::ConcurrentLookupTable<T> lookupTable;

  static constexpr size_t nextPowerOfTwo(uint32_t n)
  {
    if (n == 0)
      return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
  }

public:
  ThreadLocalStorage() : lookupTable(nextPowerOfTwo(std::max(64u, 2 * os::Thread::getHardwareConcurrency())))
  {
  }

  ThreadLocalStorage(ThreadLocalStorage &&other) noexcept : lookupTable(std::move(other.lookupTable))
  {
  }

  ThreadLocalStorage &operator=(ThreadLocalStorage &&other) noexcept
  {
    if (this != &other)
      lookupTable = std::move(other.lookupTable);
    return *this;
  }

  // Deleted copy to match move semantics logic clearly
  ThreadLocalStorage(const ThreadLocalStorage &) = delete;
  ThreadLocalStorage &operator=(const ThreadLocalStorage &) = delete;

  void set(T val)
  {
    lookupTable.insert(os::Thread::getCurrentThreadId(), val);
  }

  bool get(T &val)
  {
    return lookupTable.get(os::Thread::getCurrentThreadId(), val);
  }
};

} // namespace lib