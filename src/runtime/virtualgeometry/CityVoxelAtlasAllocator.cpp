#include "virtualgeometry/CityVoxelAtlasAllocator.hpp"

#include <algorithm>

namespace virtualgeometry {

void CityVoxelRangeAllocator::reset(uint64_t capacity) {
  free_.clear();
  capacity_ = capacity;
  used_ = 0;
  if (capacity) free_.emplace(0, capacity);
}

uint64_t CityVoxelRangeAllocator::allocate(uint64_t count) {
  if (!count) return UINT64_MAX;
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->second < count) continue;
    const uint64_t offset = it->first, remaining = it->second - count;
    free_.erase(it);
    if (remaining) free_.emplace(offset + count, remaining);
    used_ += count;
    return offset;
  }
  return UINT64_MAX;
}

void CityVoxelRangeAllocator::release(uint64_t offset, uint64_t count) {
  if (!count) return;
  used_ -= std::min(used_, count);
  auto next = free_.lower_bound(offset);
  // Merge with the following and the preceding free range when they touch.
  if (next != free_.end() && offset + count == next->first) {
    count += next->second;
    next = free_.erase(next);
  }
  if (next != free_.begin()) {
    auto previous = std::prev(next);
    if (previous->first + previous->second == offset) {
      previous->second += count;
      return;
    }
  }
  free_.emplace(offset, count);
}

uint64_t CityVoxelRangeAllocator::largestFree() const {
  uint64_t largest = 0;
  for (const auto &[offset, count] : free_) largest = std::max(largest, count);
  return largest;
}

void CityVoxelAtlasAllocator::reset(const Capacity &capacity, uint32_t releaseDelayFrames) {
  freeGeometrySlots_.clear();
  freeAppearanceSlots_.clear();
  // Hand out low slots first so live tables stay dense at the front.
  for (uint32_t i = capacity.geometries; i-- > 0;) freeGeometrySlots_.push_back(i);
  for (uint32_t i = capacity.appearances; i-- > 0;) freeAppearanceSlots_.push_back(i);
  bricks_.reset(capacity.bricks);
  words_.reset(capacity.appearanceWords);
  pending_.clear();
  delay_ = releaseDelayFrames;
  liveGeometries_ = liveAppearances_ = 0;
}

bool CityVoxelAtlasAllocator::allocate(std::vector<uint32_t> &freeSlots,
                                       CityVoxelRangeAllocator &ranges, uint64_t count,
                                       Range &out) {
  if (freeSlots.empty()) return false;
  const uint64_t offset = ranges.allocate(count);
  if (offset == UINT64_MAX) return false;
  out = {freeSlots.back(), offset, count};
  freeSlots.pop_back();
  return true;
}

bool CityVoxelAtlasAllocator::allocateGeometry(uint64_t bricks, Range &out) {
  if (!allocate(freeGeometrySlots_, bricks_, bricks, out)) return false;
  ++liveGeometries_;
  return true;
}

bool CityVoxelAtlasAllocator::allocateAppearance(uint64_t words, Range &out) {
  if (!allocate(freeAppearanceSlots_, words_, words, out)) return false;
  ++liveAppearances_;
  return true;
}

void CityVoxelAtlasAllocator::releaseGeometry(const Range &range, uint64_t frame) {
  pending_.push_back({range, frame, true});
  --liveGeometries_;
}

void CityVoxelAtlasAllocator::releaseAppearance(const Range &range, uint64_t frame) {
  pending_.push_back({range, frame, false});
  --liveAppearances_;
}

void CityVoxelAtlasAllocator::retire(uint64_t frame, std::vector<Range> *geometries,
                                     std::vector<Range> *appearances) {
  auto keep = pending_.begin();
  for (auto &entry : pending_) {
    if (entry.frame + delay_ > frame) { *keep++ = entry; continue; }
    if (entry.geometry) {
      bricks_.release(entry.range.offset, entry.range.count);
      freeGeometrySlots_.push_back(entry.range.slot);
      if (geometries) geometries->push_back(entry.range);
    } else {
      words_.release(entry.range.offset, entry.range.count);
      freeAppearanceSlots_.push_back(entry.range.slot);
      if (appearances) appearances->push_back(entry.range);
    }
  }
  pending_.erase(keep, pending_.end());
}

} // namespace virtualgeometry
