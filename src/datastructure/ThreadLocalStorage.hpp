#pragma once

#include <assert.h>
#include <atomic>
#include <thread>

#include "algorithm/random.hpp"
#include "os/Thread.hpp"
#include "os/print.hpp"

namespace lib
{

namespace detail
{
template <typename V> struct Entry
{
  std::atomic<size_t> key;
  V value;
  using Destructor = void (*)(V &);
  Destructor destructor;

  Entry() : key(0), destructor(nullptr)
  {
  }

  Entry(Entry &&other) noexcept : key(other.key.load(std::memory_order_relaxed)), value(std::move(other.value)), destructor(other.destructor)
  {
  }

  Entry &operator=(Entry &&other) noexcept
  {
    if (this != &other)
    {
      key.store(other.key.load(std::memory_order_relaxed), std::memory_order_relaxed);
      value = std::move(other.value);
      destructor = other.destructor;
    }
    return *this;
  }

  Entry(const Entry &other) : key(other.key.load(std::memory_order_relaxed)), value(other.value), destructor(other.destructor)
  {
  }

  Entry &operator=(const Entry &other)
  {
    if (this != &other)
    {
      key.store(other.key.load(std::memory_order_relaxed), std::memory_order_relaxed);
      value = other.value;
      destructor = other.destructor;
    }
    return *this;
  }
};

template <typename V> struct HashTableBucket
{
  const static size_t INVALID_KEY = SIZE_MAX;

  size_t capacity;
  Entry<V> *entries;
  HashTableBucket<V> *prev;

  HashTableBucket(size_t capacity, Entry<V> *initial) : capacity(capacity), entries(initial), prev(nullptr)
  {
    for (size_t i = 0; i != capacity; ++i)
    {
      entries[i].key.store(INVALID_KEY, std::memory_order_relaxed);
    }
  }

  HashTableBucket(size_t capacity) : capacity(capacity), entries(new Entry<V>[capacity]), prev(nullptr)
  {
    for (size_t i = 0; i != capacity; ++i)
    {
      entries[i].key.store(INVALID_KEY, std::memory_order_relaxed);
    }
  }

  ~HashTableBucket()
  {
    if (entries != nullptr)
    {
      delete[] entries;
    }
  }
};

template <typename Value> class ConcurrentLookupTable
{
private:
  std::atomic<detail::HashTableBucket<Value> *> rootTable;
  std::atomic_flag rootTableResizeInProgress;
  std::atomic<size_t> count;
  detail::HashTableBucket<Value> root;

  struct ThreadCache
  {
     size_t threadId;
    detail::HashTableBucket<Value> *lastTable;
    size_t lastIndex;
    char padding[64 - sizeof(size_t) * 2 - sizeof(void *)];
  };

  static thread_local ThreadCache cache;

  bool insertInTable(detail::HashTableBucket<Value> *hash, size_t key, Value value, void (*destructor)(Value &) = nullptr)
  {
    size_t hashedId = hashInteger(key);
    size_t index = hashedId & (hash->capacity - 1u);

    size_t probeLimit = hash->capacity;

    for (size_t i = 0; i < probeLimit; ++i)
    {
      size_t expected = detail::HashTableBucket<Value>::INVALID_KEY;

      if (hash->entries[index].key.compare_exchange_strong(expected, key, std::memory_order_release, std::memory_order_relaxed))
      {
        hash->entries[index].destructor = destructor;
        hash->entries[index].value = value;
        return true;
      }

      index = (index + 1) & (hash->capacity - 1u);
    }

    return false;
  }

  bool insertOrUpdateInTable(detail::HashTableBucket<Value> *hash, size_t key, Value value)
  {
    size_t hashedId = hashInteger(key);
    size_t index = hashedId & (hash->capacity - 1u);

    size_t probeLimit = hash->capacity;

    for (size_t i = 0; i < probeLimit; ++i)
    {
      size_t probedKey = hash->entries[index].key.load(std::memory_order_acquire);

      if (probedKey == key)
      {
        hash->entries[index].value = value;
        return true;
      }

      if (probedKey == detail::HashTableBucket<Value>::INVALID_KEY)
      {
        size_t expected = detail::HashTableBucket<Value>::INVALID_KEY;
        if (hash->entries[index].key.compare_exchange_strong(expected, key, std::memory_order_release, std::memory_order_relaxed))
        {
          hash->entries[index].value = value;
          return true;
        }

        --i;
        continue;
      }

      index = (index + 1) & (hash->capacity - 1u);
    }

    return false;
  }

  void resizeTableIfNeeded(size_t newCount)
  {
    detail::HashTableBucket<Value> *currentTable = rootTable.load(std::memory_order_acquire);

    if (newCount >= ((currentTable->capacity * 3) >> 2) && !rootTableResizeInProgress.test_and_set(std::memory_order_acquire))
    {
      currentTable = rootTable.load(std::memory_order_acquire);

      if (newCount >= ((currentTable->capacity * 3) >> 2))
      {
        size_t newCapacity = currentTable->capacity << 1;

        while (newCount >= ((newCapacity * 3) >> 2))
        {
          newCapacity <<= 1;
        }

        detail::HashTableBucket<Value> *newTable = new detail::HashTableBucket<Value>(newCapacity);
        newTable->prev = currentTable;
        rootTable.store(newTable, std::memory_order_release);
      }

      rootTableResizeInProgress.clear(std::memory_order_release);
    }
  }

public:
  ConcurrentLookupTable(size_t initialCapacity = 64) : root(initialCapacity)
  {
    assert((initialCapacity & (initialCapacity - 1)) == 0);

    rootTableResizeInProgress.clear(std::memory_order_release);
    count.store(0, std::memory_order_relaxed);
    rootTable.store(&root, std::memory_order_relaxed);
  }

  ConcurrentLookupTable(ConcurrentLookupTable &&other) noexcept : root(std::move(other.root))
  {
    root = std::move(other.root);

    other.root.entries = nullptr;
    other.root.prev = nullptr;
    other.root.capacity = 0;

    rootTableResizeInProgress.clear(std::memory_order_relaxed);
    count.store(other.count.load(std::memory_order_relaxed), std::memory_order_relaxed);

    rootTable.store(&root, std::memory_order_relaxed);

    other.rootTable.store(nullptr, std::memory_order_relaxed);
    other.count.store(0, std::memory_order_relaxed);
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
    detail::HashTableBucket<Value> *hash = rootTable.load(std::memory_order_relaxed);

    if (hash == nullptr)
    {
      return;
    }

    for (size_t i = 0; i < hash->capacity; i++)
    {
      if (hash->entries[i].key.load(std::memory_order_relaxed) != detail::HashTableBucket<Value>::INVALID_KEY && hash->entries[i].destructor != nullptr)
      {
        hash->entries[i].destructor(hash->entries[i].value);
        hash->entries[i].key.store(detail::HashTableBucket<Value>::INVALID_KEY, std::memory_order_relaxed);
      }
    }

    while (hash != nullptr)
    {
      detail::HashTableBucket<Value> *prev = hash->prev;

      if (hash != &root)
      {
        delete hash;
      }

      hash = prev;
    }

    rootTable.store(nullptr, std::memory_order_relaxed);
  }

  bool get(size_t id, Value &v)
  {
    size_t hashedId = hashInteger(id);
    auto currentTable = rootTable.load(std::memory_order_acquire);

    assert(currentTable != nullptr);

    if (cache.threadId == id && cache.lastTable == currentTable)
    {
      size_t cachedKey = currentTable->entries[cache.lastIndex].key.load(std::memory_order_acquire);
      if (cachedKey == id)
      {
        v = currentTable->entries[cache.lastIndex].value;
        return true;
      }
    }

    // Search through tables
    for (auto hash = currentTable; hash != nullptr; hash = hash->prev)
    {
      size_t index = hashedId & (hash->capacity - 1);
      size_t probeLimit = hash->capacity;

      for (size_t i = 0; i < probeLimit; ++i)
      {
        size_t probedKey = hash->entries[index].key.load(std::memory_order_acquire);

        if (probedKey == detail::HashTableBucket<Value>::INVALID_KEY)
        {
          break;
        }

        if (probedKey == id)
        {
          v = hash->entries[index].value;

          // Migrate to current table if in old table
          if (hash != currentTable)
          {
            bool inserted = insertInTable(currentTable, id, v);
            if (inserted)
            {
              hash->entries[index].key.store(detail::HashTableBucket<Value>::INVALID_KEY, std::memory_order_release);
            }
          }

          // Update cache
          cache.threadId = id;
          cache.lastTable = currentTable;
          cache.lastIndex = (hash == currentTable) ? index : (hashedId & (currentTable->capacity - 1));

          return true;
        }

        index = (index + 1) & (hash->capacity - 1);
      }
    }

    return false;
  }

  bool insert(size_t id, Value val, void (*destructor)(Value &) = nullptr)
  {
    size_t newCount = 1 + count.fetch_add(1, std::memory_order_relaxed);

    while (true)
    {
      resizeTableIfNeeded(newCount);

      auto currentTable = rootTable.load(std::memory_order_acquire);

      // Use 75% load factor for insertion threshold
      if (newCount < ((currentTable->capacity * 3) >> 2))
      {
        if (insertInTable(currentTable, id, val, destructor))
        {
          return true;
        }
      }
    }

    return true;
  }
};

template <typename Value> thread_local typename ConcurrentLookupTable<Value>::ThreadCache ConcurrentLookupTable<Value>::cache = {0, nullptr, 0};

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
  ThreadLocalStorage() : lookupTable(nextPowerOfTwo(2 * os::Thread::getHardwareConcurrency()))
  {
  }

  ThreadLocalStorage(ThreadLocalStorage &&other) noexcept : lookupTable(std::move(other.lookupTable))
  {
  }

  ThreadLocalStorage(const ThreadLocalStorage &other) : lookupTable(other.lookupTable)
  {
  }

  ThreadLocalStorage &operator=(const ThreadLocalStorage &other)
  {
    if (this != &other)
    {
      lookupTable = other.lookupTable;
    }
    return *this;
  }

  ThreadLocalStorage &operator=(ThreadLocalStorage &&other)
  {
    if (this != &other)
    {
      lookupTable = std::move(other.lookupTable);
      other.lookupTable = detail::ConcurrentLookupTable<T>();
    }

    return *this;
  }

  void set(T val)
  {
    bool inserted = lookupTable.insert(os::Thread::getCurrentThreadId(), val);
    assert(inserted);
  }

  bool get(T &val)
  {
    return lookupTable.get(os::Thread::getCurrentThreadId(), val);
  }
};

} // namespace lib