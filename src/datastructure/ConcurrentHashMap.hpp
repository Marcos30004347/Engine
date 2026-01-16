#pragma once

#include "ConcurrentSkipListMap.hpp"
#include <functional>
#include <utility>

namespace lib
{

template <
    typename K,
    typename V,
    size_t MAX_LEVEL = 16,
    typename Hasher = std::hash<K>>
class ConcurrentHashMap
{
private:
  using StoredValue   = std::pair<K, V>;
  using InternalMap   = ConcurrentSkipListMap<size_t, StoredValue, MAX_LEVEL>;
  using InternalIter  = typename InternalMap::Iterator;

  InternalMap map;
  Hasher hasher;

  size_t hashKey(const K& key) const
  {
    return hasher(key);
  }

public:
  ConcurrentHashMap() = default;
  ~ConcurrentHashMap() = default;

  // =======================
  // Iterator
  // =======================
  class Iterator
  {
    template <typename, typename, size_t, typename>
    friend class ConcurrentHashMap;

  private:
    InternalIter it;

    explicit Iterator(InternalIter iter)
      : it(std::move(iter))
    {}

  public:
    Iterator() = default;
    Iterator(const Iterator&) = default;
    Iterator& operator=(const Iterator&) = default;

    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) noexcept = default;

    ~Iterator() = default;

    // Accessors
    const K& key() const
    {
      return it.value().first;
    }

    V& value()
    {
      return it.value().second;
    }

    const V& value() const
    {
      return it.value().second;
    }

    // STL compatibility
    std::pair<const K&, V&> operator*()
    {
      auto& kv = it.value();
      return { kv.first, kv.second };
    }

    V* operator->()
    {
      return &it.value().second;
    }

    const V* operator->() const
    {
      return &it.value().second;
    }

    Iterator& operator++()
    {
      ++it;
      return *this;
    }

    Iterator operator++(int)
    {
      Iterator tmp(*this);
      ++it;
      return tmp;
    }

    bool operator==(const Iterator& other) const
    {
      return it == other.it;
    }

    bool operator!=(const Iterator& other) const
    {
      return it != other.it;
    }
  };

  // =======================
  // Modifiers
  // =======================
  Iterator insert(const K& key, const V& value)
  {
    size_t hash = hashKey(key);
    auto iter = map.insert(hash, { key, value });
    return Iterator(std::move(iter));
  }

  bool remove(const K& key)
  {
    size_t hash = hashKey(key);
    auto it = map.find(hash);

    if (it == map.end())
      return false;

    // hash collision safety
    if (it.value().first != key)
      return false;

    return map.remove(hash);
  }

  // =======================
  // Lookup
  // =======================
  Iterator find(const K& key)
  {
    size_t hash = hashKey(key);
    auto it = map.find(hash);

    if (it == map.end())
      return end();

    // hash collision safety
    if (it.value().first != key)
      return end();

    return Iterator(std::move(it));
  }

  bool contains(const K& key)
  {
    return find(key) != end();
  }

  // =======================
  // Element access
  // =======================
  Iterator operator[](const K& key)
  {
    while (true)
    {
      auto it = find(key);
      if (it != end())
        return it;

      auto inserted = insert(key, V{});
      if (inserted != end())
        return inserted;
    }
  }

  // =======================
  // Iteration
  // =======================
  Iterator begin()
  {
    return Iterator(map.begin());
  }

  Iterator end()
  {
    return Iterator(map.end());
  }

  // =======================
  // Capacity
  // =======================
  uint64_t size() const
  {
    return map.size();
  }

  bool isEmpty() const
  {
    return map.isEmpty();
  }

  void clear()
  {
    map.clear();
  }
};

} // namespace lib
