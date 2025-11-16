#pragma once
#include <array>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace lib
{

template <typename K, typename V, size_t NumShards = 64> class ConcurrentHashMap
{
  static_assert((NumShards & (NumShards - 1)) == 0, "NumShards must be a power of two");

private:
  struct Shard
  {
    mutable std::mutex mtx;
    std::unordered_map<K, V> map;
  };

  std::array<Shard, NumShards> shards;

  static size_t shardIndex(const K &key)
  {
    size_t h = std::hash<K>{}(key);
    return h & (NumShards - 1);
  }

  Shard &shardFor(const K &key)
  {
    return shards[shardIndex(key)];
  }

  //   const Shard &shardFor(const K &key) const
  //   {
  //     return shards[shardIndex(key)];
  //   }

  size_t getShardIndex(const K &key) const
  {
    return std::hash<K>{}(key) % NumShards;
  }

public:
  ConcurrentHashMap() = default;
  ~ConcurrentHashMap() = default;

  ConcurrentHashMap(const ConcurrentHashMap &) = delete;
  ConcurrentHashMap &operator=(const ConcurrentHashMap &) = delete;

  void insert(const K &key, const V &value)
  {
    auto &s = shards[getShardIndex(key)];

    std::lock_guard<std::mutex> lock(s.mtx);
    s.map[key] = value;
  }

  void insert(K &&key, V &&value)
  {
    auto &s = shards[getShardIndex(key)];
    std::lock_guard<std::mutex> lock(s.mtx);
    s.map[key] = std::move(value);//.emplace(std::move(key), std::move(value));
  }

  bool find(const K &key, V &out) const
  {
    auto &s = shards[getShardIndex(key)];
    std::lock_guard<std::mutex> lock(s.mtx);
    auto it = s.map.find(key);
    if (it == s.map.end())
      return false;
    out = it->second;
    return true;
  }

  bool contains(const K &key) const
  {
    auto &s = shards[getShardIndex(key)];
    std::lock_guard<std::mutex> lock(s.mtx);
    return s.map.find(key) != s.map.end();
  }

  bool erase(const K &key)
  {
    auto &s = shardFor(key);
    std::lock_guard<std::mutex> lock(s.mtx);
    return s.map.erase(key) > 0;
  }

  size_t size() const
  {
    size_t total = 0;
    for (auto &s : shards)
    {
      std::lock_guard<std::mutex> lock(s.mtx);
      total += s.map.size();
    }
    return total;
  }

  void clear()
  {
    for (auto &s : shards)
    {
      std::lock_guard<std::mutex> lock(s.mtx);
      s.map.clear();
    }
  }
};

} // namespace lib
