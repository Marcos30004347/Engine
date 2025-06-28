#pragma once

#include <assert.h>
#include <atomic>
#include <thread>

#include "Vector.hpp"

#include "lib/algorithm/random.hpp"
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

  Entry()
  {
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

  std::atomic<HashTableBucket<Value> *> rootTable;

  std::atomic_flag rootTableResizeInProgress;
  std::atomic<size_t> count;

  HashTableBucket<Value> root;

  bool insertInTable(HashTableBucket<Value> *hash, size_t key, Value value)
  {
    size_t hashedId = hashInteger(key);
    size_t index = hashedId;

    size_t i = 0;

    while (true)
    {
      index &= (hash->capacity - 1u);

      size_t expected = HashTableBucket<Value>::INVALID_KEY;

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
    HashTableBucket<Value> *currentTable = rootTable.load(std::memory_order_acquire);

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

        HashTableBucket<Value> *newTable = new HashTableBucket<Value>(newCapacity);

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

  ConcurrentLookupTable(ConcurrentLookupTable<Value> &&other) : root(other.root.capacity)
  {
    swapRelaxed(count, other.count);
    swapRelaxed(rootTable, other.rootTable);

    for (size_t i = 0; i < other.root.capacity; i++)
    {
      Entry<Value> tmp = root.entries[i];
      root.entries[i] = other.root.entries[i];
      other.root.entries[i] = tmp;
    }

    if (rootTable.load(std::memory_order_relaxed) == &other.root)
    {
      rootTable.store(&root, std::memory_order_relaxed);
    }
    else
    {
      HashTableBucket<Value> *hash;

      for (hash = rootTable.load(std::memory_order_relaxed); hash->prev != &other.root; hash = hash->prev)
      {
        continue;
      }

      hash->prev = &root;
    }

    if (other.rootTable.load(std::memory_order_relaxed) == &root)
    {
      other.rootTable.store(&other.root, std::memory_order_relaxed);
    }
    else
    {
      HashTableBucket<Value> *hash;

      for (hash = other.rootTable.load(std::memory_order_relaxed); hash->prev != &root; hash = hash->prev)
      {
        continue;
      }

      hash->prev = &other.root;
    }
  }

  ~ConcurrentLookupTable()
  {
    HashTableBucket<Value> *hash = rootTable.load(std::memory_order_relaxed);

    while (hash != nullptr)
    {
      HashTableBucket<Value> *prev = hash->prev;

      if (prev != nullptr)
      {
        for (size_t i = 0; i != hash->capacity; ++i)
        {
          hash->entries[i].~Entry<Value>();
        }

        hash->~HashTableBucket<Value>();
        if (hash != &root)
        {
          delete hash;
        }
      }

      hash = prev;
    }

    delete root.entries;
  }

  bool get(size_t id, Value &v)
  {
    size_t hashedId = hashInteger(id);

    auto currentTable = rootTable.load(std::memory_order_acquire);

    assert(currentTable != nullptr);

    for (auto hash = currentTable; hash != nullptr; hash = hash->prev)
    {
      size_t index = hashedId;
      size_t i = 0;
      while (true)
      {
        index &= (hash->capacity - 1);

        size_t probedKey = hash->entries[index].key.load(std::memory_order_relaxed);
        // os::threadSafePrintf("getting %u, as %u, capacity = %u at %p in %u, key = %u\n", id, hashedId, hash->capacity, hash, index, probedKey);

        if (probedKey == HashTableBucket<Value>::INVALID_KEY)
        {
          // os::threadSafePrintf("getting %u, as %u, capacity = %u at %p in %u failed break!\n", id, hashedId, hash->capacity, hash, index);
          break;
        }

        if (probedKey == id)
        {
          if (hash->entries[index].filled.load() == 0)
          {
            // os::threadSafePrintf("getting %u, as %u, capacity = %u at %p in %u failed still writting!\n", id, hashedId, hash->capacity, hash, index);

            // Other thread is still updating the value
            return false;
          }

          v = hash->entries[index].value;

          if (hash != currentTable)
          {
            // os::threadSafePrintf("getting %u, as %u, capacity = %u at %p in %u ins in new table!\n", id, hashedId, hash->capacity, hash, index);

            insertInTable(currentTable, id, v);
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

  bool insert(size_t id, Value val)
  {
    size_t newCount = 1 + count.fetch_add(1, std::memory_order_relaxed);

    while (true)
    {
      resizeTableIfNeeded(newCount);

      auto currentTable = rootTable.load(std::memory_order_acquire);
      // No mather if we're allocating a new table or not, if there is room in the current
      // table, we add the value to it.
      if (newCount < (currentTable->capacity >> 1) + (currentTable->capacity >> 2))
      {
        if (insertInTable(currentTable, id, val))
        {
          return true;
        }
      }
    }

    return true;
  }
  /*
    bool update(size_t id, Value v)
    {
      size_t hashedId = hashInteger(id);

      auto currentTable = rootTable.load(std::memory_order_acquire);

      assert(currentTable != nullptr);

      for (auto hash = currentTable; hash != nullptr; hash = hash->prev)
      {
        size_t index = hashedId;
        size_t i = 0;
        while (true)
        {
          index &= (hash->capacity - 1);
          size_t probedKey = hash->entries[index].key.load(std::memory_order_relaxed);

          if (probedKey == HashTableBucket<Value>::INVALID_KEY)
          {
            break;
          }

          if (probedKey == id)
          {

            hash->entries[index].value = v;
            hash->entries[index].filled = 1;

            currentTable = rootTable.load(std::memory_order_acquire);

            while (hash != currentTable)
            {
              // Recent tables are bigger than old ones with space for all elements
              // in older tables, so this call should always return true.
              assert(insertInTable(currentTable, id, v));

              hash = currentTable;
              currentTable = rootTable.load(std::memory_order_acquire);
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

  */
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

  void set(T val)
  {
#if NDEBUG
    T old;
    assert(lookupTable.get(os::Thread::getCurrentThreadId(), old) == false);
#endif
    assert(lookupTable.insert(os::Thread::getCurrentThreadId(), val));
  }

  /*
    void set(T val, T &old)
    {

      if (lookupTable.get(os::Thread::getCurrentThreadId(), old))
      {
        return assert(lookupTable.update(os::Thread::getCurrentThreadId(), val));
      }

      assert(lookupTable.insert(os::Thread::getCurrentThreadId(), val));
    }
  */

  bool get(T &val)
  {
    return lookupTable.get(os::Thread::getCurrentThreadId(), val);
  }
};

} // namespace lib