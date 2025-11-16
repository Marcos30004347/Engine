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
  std::atomic<size_t> filled;

  V value;
  using Destructor = void (*)(V &);
  Destructor destructor;

  Entry() : key(0), filled(0), destructor(nullptr)
  {
  }

  Entry(Entry &&other) noexcept
  {
    key.store(other.key.load(std::memory_order_relaxed), std::memory_order_relaxed);
    filled.store(other.filled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    value = std::move(other.value);
    destructor = other.destructor;
  }

  // Move assignment
  Entry &operator=(Entry &&other) noexcept
  {
    if (this != &other)
    {
      key.store(other.key.load(std::memory_order_relaxed), std::memory_order_relaxed);
      filled.store(other.filled.load(std::memory_order_relaxed), std::memory_order_relaxed);
      value = std::move(other.value);
      destructor = other.destructor;
    }
    return *this;
  }

  // Copy constructor
  Entry(const Entry &other)
  {
    key.store(other.key.load(std::memory_order_relaxed), std::memory_order_relaxed);
    filled.store(other.filled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    value = other.value;
    destructor = other.destructor;
  }

  // Copy assignment
  Entry &operator=(const Entry &other)
  {
    if (this != &other)
    {
      key.store(other.key.load(std::memory_order_relaxed), std::memory_order_relaxed);
      filled.store(other.filled.load(std::memory_order_relaxed), std::memory_order_relaxed);
      value = other.value;
      destructor = other.destructor;
    }
    return *this;
  }
};

template <typename V> struct HashTableBucket
{
  const static size_t INVALID_KEY = -1;

  size_t capacity;

  Entry<V> *entries;
  HashTableBucket<V> *prev;
  /*

    HashTableBucket() : capacity(), prev(nullptr)
    {
      entries = nullptr;
    }
  */

  HashTableBucket(size_t capacity, Entry<V> *initial) : capacity(capacity), prev(nullptr)
  {
    entries = initial;

    for (size_t i = 0; i != capacity; ++i)
    {
      entries[i].key.store(INVALID_KEY, std::memory_order_relaxed);
      entries[i].filled.store(0, std::memory_order_relaxed);
    }
  }

  HashTableBucket(size_t capacity) : capacity(capacity), prev(nullptr)
  {
    entries = new Entry<V>[capacity];
    for (size_t i = 0; i != capacity; ++i)
    {
      entries[i].key.store(INVALID_KEY, std::memory_order_relaxed);
      entries[i].filled.store(0, std::memory_order_relaxed);
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

  bool insertInTable(detail::HashTableBucket<Value> *hash, size_t key, Value value, void (*destructor)(Value &) = nullptr)
  {
    size_t hashedId = hashInteger(key);
    size_t index = hashedId;

    size_t i = 0;

    while (true)
    {
      index &= (hash->capacity - 1u);

      size_t expected = detail::HashTableBucket<Value>::INVALID_KEY;

      if (hash->entries[index].key.compare_exchange_strong(expected, key, std::memory_order_seq_cst, std::memory_order_relaxed))
      {
        hash->entries[index].destructor = destructor;
        hash->entries[index].value = value;
        hash->entries[index].filled = 1;
        return true;
      }

      ++index;

      if (++i == hash->capacity)
      {
        break;
      }
    }

    return false;
  }

  bool insertOrUpdateInTable(detail::HashTableBucket<Value> *hash, size_t key, Value value)
  {
    size_t hashedId = hashInteger(key);
    size_t index = hashedId;

    size_t i = 0;

    while (true)
    {
      index &= (hash->capacity - 1u);

      size_t expected = detail::HashTableBucket<Value>::INVALID_KEY;

      if (hash->entries[index].key.load() == key)
      {
        // os::threadSafePrintf("updated %u, as %u, capacity = %u at %p in %u\n", key, hashedId, hash->capacity, hash, index);
        hash->entries[index].value = value;
        return true;
      }

      if (hash->entries[index].key.compare_exchange_strong(expected, key, std::memory_order_seq_cst, std::memory_order_relaxed))
      {
        hash->entries[index].value = value;
        hash->entries[index].filled = 1;
        // os::threadSafePrintf("inserted %u, as %u, capacity = %u at %p in %u\n", key, hashedId, hash->capacity, hash, index);
        return true;
      }

      ++index;

      if (++i == hash->capacity)
      {
        break;
      }
    }

    return false;
  }

  template <typename G> static inline void swapRelaxed(std::atomic<G> &left, std::atomic<G> &right)
  {
    G temp = left.load(std::memory_order_relaxed);
    left.store(right.load(std::memory_order_relaxed), std::memory_order_relaxed);
    right.store(temp, std::memory_order_relaxed);
  }

  void resizeTableIfNeeded(size_t newCount)
  {
    detail::HashTableBucket<Value> *currentTable = rootTable.load(std::memory_order_acquire);

    if (newCount >= (currentTable->capacity >> 1) && !rootTableResizeInProgress.test_and_set(std::memory_order_acquire))
    {
      currentTable = rootTable.load(std::memory_order_acquire);

      if (newCount >= (currentTable->capacity >> 1))
      {
        size_t newCapacity = currentTable->capacity << 1;

        while (newCount >= (newCapacity >> 1))
        {
          newCapacity <<= 1;
        }

        detail::HashTableBucket<Value> *newTable = new detail::HashTableBucket<Value>(newCapacity);

        newTable->prev = currentTable;

        rootTable.store(newTable, std::memory_order_release);
        rootTableResizeInProgress.clear(std::memory_order_release);
      }
      else
      {
        rootTableResizeInProgress.clear(std::memory_order_release);
      }
    }
  }

public:
  ConcurrentLookupTable(size_t initialCapacity = 64) : root(initialCapacity)
  {
    assert(initialCapacity > 1);

    rootTableResizeInProgress.clear(std::memory_order_release);
    count.store(0, std::memory_order_relaxed);
    rootTable.store(&root, std::memory_order_relaxed);
  }

  ConcurrentLookupTable(ConcurrentLookupTable &&other) noexcept : root(std::move(other.root))
  {
    root = std::move(other.root);

    other.root.entries = nullptr;
    other.root.prev = 0;
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

    for (detail::HashTableBucket<Value> *curr = hash->prev; curr; curr = curr->prev)
    {
      for (size_t i = 0; i < curr->capacity; i++)
      {
        if (curr->entries[i].key.load() != detail::HashTableBucket<Value>::INVALID_KEY)
        {
          bool exists = get(curr->entries[i].key.load(), curr->entries[i].value);
          assert(exists);
        }
      }
    }

    for (size_t i = 0; i < hash->capacity; i++)
    {
      if (hash->entries[i].key.load() != detail::HashTableBucket<Value>::INVALID_KEY && hash->entries[i].destructor != nullptr)
      {
        if (hash->entries[i].destructor)
        {
          hash->entries[i].destructor(hash->entries[i].value);
        }
        hash->entries[i].key.store(detail::HashTableBucket<Value>::INVALID_KEY);
      }
    }

    while (hash != nullptr)
    {
      detail::HashTableBucket<Value> *prev = hash->prev;

      if (prev != nullptr)
      {
        for (size_t i = 0; i != hash->capacity; ++i)
        {
          hash->entries[i].~Entry<Value>();
        }

        if (hash != &root)
        {
          delete hash;
        }
      }

      hash = prev;
    }

    hash = nullptr;
    rootTable.store(nullptr);
    // delete root.entries;
  }

  bool get(size_t id, Value &v)
  {
    size_t hashedId = hashInteger(id);

    auto currentTable = rootTable.load(std::memory_order_acquire);

    assert(currentTable != nullptr);
    size_t iters = 0;

    for (auto hash = currentTable; hash != nullptr; hash = hash->prev)
    {
      size_t index = hashedId;
      size_t i = 0;

      while (true)
      {
        iters++;
        index &= (hash->capacity - 1);

        size_t probedKey = hash->entries[index].key.load(std::memory_order_relaxed);
        // os::threadSafePrintf("getting %u, as %u, capacity = %u at %p in %u, key = %u\n", id, hashedId, hash->capacity, hash, index, probedKey);

        if (probedKey == detail::HashTableBucket<Value>::INVALID_KEY)
        {
          break;
        }

        if (probedKey == id)
        {
          if (hash->entries[index].filled.load() == 0)
          {
            return false;
          }

          v = hash->entries[index].value;

          if (hash != currentTable)
          {
            bool inserted = insertInTable(currentTable, id, v);
            assert(inserted);
            hash->entries[index].key = detail::HashTableBucket<Value>::INVALID_KEY;
          }

          return true;
        }

        ++index;

        if (++i == hash->capacity)
        {
          break;
        }
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
      if (newCount < (currentTable->capacity >> 1) + (currentTable->capacity >> 2))
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

} // namespace detail

template <typename T> class ThreadLocalStorage
{
  detail::ConcurrentLookupTable<T> lookupTable;

  static size_t nextPowerOfTwo(uint32_t n)
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

    bool v = lookupTable.get(os::Thread::getCurrentThreadId(), val);
    return v;
  }
};

} // namespace lib
