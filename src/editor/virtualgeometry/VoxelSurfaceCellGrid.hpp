#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace virtualgeometry
{

// Cell index by lattice position: a dense array over the cells' bounding box
// (one guard cell wide) when that is small - a terrain chunk or a house -
// and a hash map otherwise. The meshing and baking of a 64x64-column chunk
// spent most of its time in hash lookups over ~150k cells.
class VoxelSurfaceCellGrid
{
public:
  static constexpr uint32_t None = UINT32_MAX;

  template <typename Position>
  void build(size_t count, Position position)
  {
    lo_ = {INT32_MAX, INT32_MAX, INT32_MAX};
    std::array<int32_t, 3> hi{INT32_MIN, INT32_MIN, INT32_MIN};
    for (size_t i = 0; i < count; ++i)
      for (int a = 0; a < 3; ++a)
      {
        lo_[a] = std::min(lo_[a], position(i)[a]);
        hi[a] = std::max(hi[a], position(i)[a]);
      }
    uint64_t volume = 1u;
    for (int a = 0; a < 3; ++a)
    {
      lo_[a] = count ? lo_[a] - 1 : 0;
      size_[a] = count ? uint32_t(int64_t(hi[a]) - lo_[a] + 2) : 1u;
      volume *= size_[a];
    }
    dense_.clear();
    hashed_.clear();
    useDense_ = volume <= kMaxDense && volume <= 64u * std::max<uint64_t>(count, 1u);
    if (useDense_)
      dense_.assign(volume, None);
    else
      hashed_.reserve(count * 2u);
    for (size_t i = 0; i < count; ++i)
    {
      const auto p = position(i);
      if (useDense_)
      {
        uint32_t &slot = dense_[denseIndex(p)];
        if (slot == None)
          slot = uint32_t(i);
      }
      else
        hashed_.emplace(hashKey(p), uint32_t(i));
    }
  }

  uint32_t find(const std::array<int32_t, 3> &p) const
  {
    if (useDense_)
    {
      for (int a = 0; a < 3; ++a)
        if (p[a] < lo_[a] || uint32_t(p[a] - lo_[a]) >= size_[a])
          return None;
      return dense_[denseIndex(p)];
    }
    const auto it = hashed_.find(hashKey(p));
    return it == hashed_.end() ? None : it->second;
  }

private:
  static constexpr uint64_t kMaxDense = 64ull << 20; // 256 MiB of indices.

  size_t denseIndex(const std::array<int32_t, 3> &p) const
  {
    return (size_t(p[1] - lo_[1]) * size_[2] + size_t(p[2] - lo_[2])) * size_[0] + size_t(p[0] - lo_[0]);
  }
  static uint64_t hashKey(const std::array<int32_t, 3> &p)
  {
    return (uint64_t(uint32_t(p[0] + (1 << 20)) & 0x1fffffu) << 42) | (uint64_t(uint32_t(p[1] + (1 << 20)) & 0x1fffffu) << 21) |
           uint64_t(uint32_t(p[2] + (1 << 20)) & 0x1fffffu);
  }

  bool useDense_ = false;
  std::array<int32_t, 3> lo_{};
  std::array<uint32_t, 3> size_{};
  std::vector<uint32_t> dense_;
  std::unordered_map<uint64_t, uint32_t> hashed_;
};

} // namespace virtualgeometry
