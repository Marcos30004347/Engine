#include "voxel/svraster/CityVoxelExactAppearanceLookup.hpp"

#include <algorithm>
#include <limits>

namespace voxel::svraster {
namespace {
uint32_t word(const std::vector<uint8_t> &bytes, uint64_t at) {
  return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1u]) << 8u) |
         (uint32_t(bytes[at + 2u]) << 16u) | (uint32_t(bytes[at + 3u]) << 24u);
}
bool fail(std::string *error, const char *message) {
  if (error) *error = message;
  return false;
}
std::array<int32_t, 3> brickKey(const std::vector<uint8_t> &bytes, uint32_t index) {
  const uint64_t at = 28ull + uint64_t(index) * 76ull;
  return {int32_t(word(bytes, at)), int32_t(word(bytes, at + 4u)),
          int32_t(word(bytes, at + 8u))};
}
} // namespace

bool CityVoxelExactAppearanceLookup::bind(
    const CityVoxelCompiledGeometry &geometry,
    const CityVoxelCompiledAppearance &appearance,
    std::string *error) {
  if (error) error->clear();
  geometry_ = nullptr;
  appearance_ = nullptr;
  brickBaseRanks_.clear();
  paletteCount_ = overrideCount_ = 0u;
  if (geometry.id != appearance.geometryId ||
      geometry.addressHash != appearance.geometryAddressHash ||
      geometry.occupiedCount != appearance.occupiedCount ||
      geometry.bytes.size() < 28u)
    return fail(error, "appearance does not match geometry address stream");
  const auto &g = geometry.bytes;
  const uint32_t count = word(g, 20u), bricks = word(g, 24u);
  if (word(g, 0u) != 0x31475643u || word(g, 4u) != geometry.pitchMicrometres ||
      count != geometry.occupiedCount || bricks == 0u ||
      g.size() != 28ull + uint64_t(bricks) * 76ull)
    return fail(error, "invalid city geometry stream");
  brickBaseRanks_.reserve(bricks);
  uint64_t prefix = 0;
  std::array<int32_t, 3> previous{};
  for (uint32_t b = 0; b < bricks; ++b) {
    const auto key = brickKey(g, b);
    if ((b && key <= previous) || key[0] < 0 || key[1] < 0 || key[2] < 0)
      return fail(error, "invalid city brick order");
    previous = key;
    brickBaseRanks_.push_back(static_cast<uint32_t>(prefix));
    const uint64_t at = 28ull + uint64_t(b) * 76ull + 12ull;
    for (uint32_t i = 0; i < 16u; ++i)
      prefix += __builtin_popcount(word(g, at + uint64_t(i) * 4ull));
  }
  if (prefix != count) return fail(error, "city brick occupancy count mismatch");
  const auto &a = appearance.bytes;
  switch (appearance.codec) {
    case CityVoxelCodec::Raw:
      if (a.size() != uint64_t(count) * 8ull)
        return fail(error, "invalid raw appearance size");
      break;
    case CityVoxelCodec::Palette8:
    case CityVoxelCodec::Palette16:
      if (a.size() < 4u) return fail(error, "truncated palette appearance");
      paletteCount_ = word(a, 0u);
      if (paletteCount_ == 0u ||
          paletteCount_ > (appearance.codec == CityVoxelCodec::Palette8 ? 256u : 65536u) ||
          a.size() != 4ull + uint64_t(paletteCount_) * 8ull +
                      uint64_t(count) * (appearance.codec == CityVoxelCodec::Palette8 ? 1ull : 2ull))
        return fail(error, "invalid palette appearance size");
      break;
    case CityVoxelCodec::SparseOverride:
      if (a.size() < 12u) return fail(error, "truncated sparse appearance");
      overrideCount_ = word(a, 8u);
      if (overrideCount_ > count || a.size() != 12ull + uint64_t(overrideCount_) * 12ull)
        return fail(error, "invalid sparse appearance size");
      for (uint32_t i = 0; i < overrideCount_; ++i)
        if (word(a, 12ull + uint64_t(i) * 12ull) >= count ||
            (i && word(a, 12ull + uint64_t(i) * 12ull) <=
                  word(a, uint64_t(i) * 12ull)))
          return fail(error, "invalid sparse override order");
      break;
    default:
      return fail(error, "unsupported city appearance codec");
  }
  geometry_ = &geometry;
  appearance_ = &appearance;
  return true;
}

bool CityVoxelExactAppearanceLookup::lookupCell(
    const std::array<int32_t, 3> &cell,
    uint32_t &rgba, uint32_t &materialProfile) const {
  if (!geometry_ || !appearance_) return false;
  for (uint32_t axis = 0; axis < 3u; ++axis)
    if (cell[axis] < 0 || cell[axis] >= geometry_->extent[axis]) return false;
  const std::array<int32_t, 3> target{cell[0] / 8, cell[1] / 8, cell[2] / 8};
  const auto &g = geometry_->bytes;
  const uint32_t bricks = static_cast<uint32_t>(brickBaseRanks_.size());
  uint32_t low = 0u, high = bricks;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2u;
    if (brickKey(g, middle) < target) low = middle + 1u;
    else high = middle;
  }
  if (low == bricks || brickKey(g, low) != target) return false;
  const uint32_t bit = uint32_t(cell[0] & 7) + 8u * uint32_t(cell[1] & 7) +
                       64u * uint32_t(cell[2] & 7);
  const uint64_t occupancy = 28ull + uint64_t(low) * 76ull + 12ull;
  const uint32_t current = word(g, occupancy + uint64_t(bit / 32u) * 4ull);
  if ((current & (1u << (bit % 32u))) == 0u) return false;
  uint32_t rank = brickBaseRanks_[low];
  for (uint32_t w = 0u; w < bit / 32u; ++w)
    rank += __builtin_popcount(word(g, occupancy + uint64_t(w) * 4ull));
  rank += __builtin_popcount(current & ((1u << (bit % 32u)) - 1u));
  const auto &a = appearance_->bytes;
  uint64_t valueAt = 0;
  switch (appearance_->codec) {
    case CityVoxelCodec::Raw:
      valueAt = uint64_t(rank) * 8ull;
      break;
    case CityVoxelCodec::Palette8:
    case CityVoxelCodec::Palette16: {
      const uint64_t indices = 4ull + uint64_t(paletteCount_) * 8ull;
      const uint32_t palette = appearance_->codec == CityVoxelCodec::Palette8
          ? a[indices + rank]
          : uint32_t(a[indices + uint64_t(rank) * 2ull]) |
            (uint32_t(a[indices + uint64_t(rank) * 2ull + 1ull]) << 8u);
      if (palette >= paletteCount_) return false;
      valueAt = 4ull + uint64_t(palette) * 8ull;
      break;
    }
    case CityVoxelCodec::SparseOverride: {
      uint32_t left = 0u, right = overrideCount_;
      while (left < right) {
        const uint32_t middle = left + (right - left) / 2u;
        if (word(a, 12ull + uint64_t(middle) * 12ull) < rank) left = middle + 1u;
        else right = middle;
      }
      valueAt = left < overrideCount_ &&
                word(a, 12ull + uint64_t(left) * 12ull) == rank
          ? 16ull + uint64_t(left) * 12ull : 0ull;
      break;
    }
    default:
      return false;
  }
  rgba = word(a, valueAt);
  materialProfile = word(a, valueAt + 4u);
  return true;
}

bool CityVoxelExactAppearanceLookup::lookupHit(
    const CityVoxelVGClusterSource &source,
    const std::array<float, 3> &localPosition,
    const std::array<float, 3> &localNormal,
    const std::array<float, 2> &uv,
    uint32_t &rgba, uint32_t &materialProfile) const {
  if (!geometry_) return false;
  std::array<int32_t, 3> cell{};
  return cityVoxelSourceCellFromHit(source, localPosition, localNormal, uv,
                                    geometry_->pitchMicrometres, cell) &&
         lookupCell(cell, rgba, materialProfile);
}

} // namespace voxel::svraster
