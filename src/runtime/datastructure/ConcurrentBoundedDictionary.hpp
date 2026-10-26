#pragma once
#include "algorithm/crc32.hpp"
#include "os/print.hpp"
#include <atomic>
#include <cassert>
#include <functional> // for std::hash
#include <string>
#include <type_traits>
#include <utility>

namespace lib
{
namespace detail
{
template <typename K, typename V> struct EntryNode
{
  std::atomic<size_t> keyHash;
  std::atomic<size_t> filled;
  K key;
  V value;
  EntryNode() : keyHash(0), filled(0)
  {
  }
};
} // namespace detail

template <typename Key, typename Value, typename Hash = std::hash<Key>> class ConcurrentBoundedDictionary
{
private:
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

  using Entry = detail::EntryNode<Key, Value>;
  const size_t capacity;
  Entry *entries; // direct array of entries
  Hash hasher;    // Hash function object

  size_t computeHash(const Key &key) const
  {
    // Use the hash function object
    return hasher(key);
  }

public:
  static Value emptyValue;

  ConcurrentBoundedDictionary(size_t n, const Hash &hash_func = Hash{}) : capacity(nextPowerOfTwo(n)), hasher(hash_func)
  {
    assert(n > 1);
    entries = new Entry[capacity];
  }

  ~ConcurrentBoundedDictionary()
  {
    delete[] entries;
  }

  bool insert(const Key &key, const Value &val)
  {
    size_t hashValue = computeHash(key);
    size_t index = hashValue;
    size_t i = 0;
    while (i < capacity)
    {
      index &= (capacity - 1);
      Entry &slot = entries[index];
      size_t expected = 0;
      if (slot.filled.compare_exchange_strong(expected, 1, std::memory_order_acq_rel, std::memory_order_relaxed))
      {
        slot.keyHash.store(hashValue, std::memory_order_relaxed);
        slot.key = key;
        slot.value = std::move(val);
        return true;
      }
      if (slot.filled.load(std::memory_order_acquire) == 1 && slot.keyHash.load(std::memory_order_relaxed) == hashValue && slot.key == key)
      {
        return false;
      }
      index++;
      i++;
    }
    return false; // table full
  }

  Value &get(const Key &key)
  {
    size_t hashValue = computeHash(key);
    size_t index = hashValue;
    size_t i = 0;
    while (i < capacity)
    {
      index &= (capacity - 1);
      Entry &slot = entries[index];
      if (slot.filled.load(std::memory_order_acquire) == 0)
        return emptyValue;
      if (slot.keyHash.load(std::memory_order_relaxed) == hashValue && slot.key == key && slot.filled.load(std::memory_order_acquire))
      {
        return slot.value;
      }
      index++;
      i++;
    }
    return emptyValue;
  }

  bool contains(const Key &key) const
  {
    size_t hashValue = computeHash(key);
    size_t index = hashValue;
    size_t i = 0;
    while (i < capacity)
    {
      index &= (capacity - 1);
      const Entry &slot = entries[index];
      if (slot.filled.load(std::memory_order_acquire) == 0)
        return false;
      if (slot.keyHash.load(std::memory_order_relaxed) == hashValue && slot.key == key && slot.filled.load(std::memory_order_acquire))
      {
        return true;
      }
      index++;
      i++;
    }
    return false;
  }
};

template <typename Key, typename Value, typename Hash> Value ConcurrentBoundedDictionary<Key, Value, Hash>::emptyValue{};

} // namespace lib