#pragma once

#include "ConcurrentSkipListMap.hpp"
#include <functional>

namespace lib
{

template <
    typename K,
    typename V,
    size_t MAX_LEVEL = 16,
    typename Hasher = std::hash<K>,
    typename Allocator = memory::allocator::SystemAllocator<ConcurrentSkipListMapNode<size_t, V, MAX_LEVEL>>>
class ConcurrentHashMap
{
private:
  using InternalMap = ConcurrentSkipListMap<size_t, V, MAX_LEVEL, Allocator>;
  using InternalIterator = typename InternalMap::Iterator;

  InternalMap map;
  Hasher hasher;

  size_t hashKey(const K &key) const
  {
    return hasher(key);
  }

public:
  ConcurrentHashMap() : map(), hasher()
  {
  }

  ~ConcurrentHashMap() = default;

  class Iterator
  {
    template <typename A, typename B, size_t C, typename H, typename D> friend class ConcurrentHashMap;

  private:
    InternalIterator internal_iter;
    K original_key;

    Iterator(InternalIterator iter, const K &key) : internal_iter(std::move(iter)), original_key(key)
    {
    }

  public:
    // Copy constructor
    Iterator(const Iterator &other) : internal_iter(other.internal_iter), original_key(other.original_key)
    {
    }

    // Copy assignment operator
    Iterator &operator=(const Iterator &other)
    {
      if (this != &other)
      {
        internal_iter = other.internal_iter;
        original_key = other.original_key;
      }
      return *this;
    }

    // Move constructor
    Iterator(Iterator &&other) noexcept : internal_iter(std::move(other.internal_iter)), original_key(std::move(other.original_key))
    {
    }

    // Move assignment operator
    Iterator &operator=(Iterator &&other) noexcept
    {
      if (this != &other)
      {
        internal_iter = std::move(other.internal_iter);
        original_key = std::move(other.original_key);
      }
      return *this;
    }

    Iterator &operator=(const V &val)
    {
      internal_iter = val;
      return *this;
    }

    ~Iterator() = default;

    const K &key() const
    {
      return original_key;
    }

    V &value()
    {
      return internal_iter.value();
    }

    const V &value() const
    {
      return internal_iter.value();
    }

    operator V &()
    {
      return internal_iter.value();
    }

    operator const V &() const
    {
      return internal_iter.value();
    }

    Iterator &operator++()
    {
      ++internal_iter;
      return *this;
    }

    Iterator operator++(int)
    {
      Iterator tmp = *this;
      ++internal_iter;
      return tmp;
    }

    std::pair<const K &, V &> operator*()
    {
      return std::pair<const K &, V &>(original_key, internal_iter.value());
    }

    bool operator==(const Iterator &other) const
    {
      return internal_iter == other.internal_iter;
    }

    bool operator!=(const Iterator &other) const
    {
      return internal_iter != other.internal_iter;
    }

    V *operator->()
    {
      return internal_iter.operator->();
    }

    const V *operator->() const
    {
      return internal_iter.operator->();
    }
  };

  Iterator insert(const K &key, const V &value)
  {
    size_t hash = hashKey(key);
    auto internal_result = map.insert(hash, value);
    return Iterator(std::move(internal_result), key);
  }

  bool remove(const K &key)
  {
    size_t hash = hashKey(key);
    return map.remove(hash);
  }

  Iterator find(const K &key)
  {
    size_t hash = hashKey(key);
    auto internal_result = map.find(hash);
    return Iterator(std::move(internal_result), key);
  }

  int getSize() const
  {
    return map.size();
  }

  bool isEmpty() const
  {
    return map.isEmpty();
  }

  Iterator begin()
  {
    auto internal_result = map.begin();
    return Iterator(std::move(internal_result), K());
  }

  Iterator end()
  {
    auto internal_result = map.end();
    return Iterator(std::move(internal_result), K());
  }

  Iterator operator[](const K &key)
  {
    size_t hash = hashKey(key);
    auto internal_result = map[hash];
    return Iterator(std::move(internal_result), key);
  }

  void clear() {
    map.clear();
  }

  uint64_t size() {
    return map.size();
  }

  bool contains(const K& key) {
    return find(key) != end();
  }
};

} // namespace lib