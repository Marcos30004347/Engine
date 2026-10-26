#pragma once

#include <algorithm>
#include <random>

namespace lib
{

template <typename T> inline void shuffleArray(std::vector<T> &arr, size_t hash)
{
  std::seed_seq seed{static_cast<uint32_t>(hash >> 32), static_cast<uint32_t>(hash & 0xFFFFFFFF)};
  std::mt19937 rng(seed);
  for (int i = arr.size() - 1; i > 0; --i)
  {
    std::uniform_int_distribution<> dis(0, i);
    int j = dis(rng);
    std::swap(arr[i], arr[j]);
  }
}

inline size_t random(size_t hash)
{
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<size_t> dis(0, std::numeric_limits<size_t>::max());
  return dis(gen);
}

inline size_t hashInteger(size_t h)
{
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}
} // namespace lib