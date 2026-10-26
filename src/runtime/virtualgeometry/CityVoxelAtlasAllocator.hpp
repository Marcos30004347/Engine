#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace virtualgeometry {

// First-fit range allocator over [0, capacity) for a fixed GPU buffer. Freed
// ranges coalesce with their neighbours. Units are whatever the caller
// indexes (bricks, words); nothing here touches the GPU.
class CityVoxelRangeAllocator {
public:
  explicit CityVoxelRangeAllocator(uint64_t capacity = 0) { reset(capacity); }
  void reset(uint64_t capacity);
  // UINT64_MAX when no free range is large enough.
  uint64_t allocate(uint64_t count);
  void release(uint64_t offset, uint64_t count);
  uint64_t capacity() const { return capacity_; }
  uint64_t used() const { return used_; }
  uint64_t largestFree() const;

private:
  std::map<uint64_t, uint64_t> free_; // offset -> count
  uint64_t capacity_ = 0, used_ = 0;
};

// Slot and range bookkeeping for the streaming city appearance atlas. The GPU
// may still read a slot for a few frames after the CPU drops it, so releases
// take effect only once `retire(frame)` passes the frame they were made in
// plus the delay.
class CityVoxelAtlasAllocator {
public:
  struct Capacity {
    uint32_t geometries = 0, appearances = 0;
    uint64_t bricks = 0, appearanceWords = 0;
  };
  struct Range { uint32_t slot = UINT32_MAX; uint64_t offset = 0, count = 0; };

  void reset(const Capacity &capacity, uint32_t releaseDelayFrames);
  bool allocateGeometry(uint64_t bricks, Range &out);
  bool allocateAppearance(uint64_t words, Range &out);
  void releaseGeometry(const Range &range, uint64_t frame);
  void releaseAppearance(const Range &range, uint64_t frame);
  // Returns the ranges whose release has retired, so the caller can clear
  // their table entries before the slots are reused.
  void retire(uint64_t frame, std::vector<Range> *geometries = nullptr,
              std::vector<Range> *appearances = nullptr);
  uint32_t liveGeometries() const { return liveGeometries_; }
  uint32_t liveAppearances() const { return liveAppearances_; }
  const CityVoxelRangeAllocator &bricks() const { return bricks_; }
  const CityVoxelRangeAllocator &words() const { return words_; }

private:
  struct Pending { Range range; uint64_t frame = 0; bool geometry = false; };
  bool allocate(std::vector<uint32_t> &freeSlots, CityVoxelRangeAllocator &ranges,
                uint64_t count, Range &out);
  std::vector<uint32_t> freeGeometrySlots_, freeAppearanceSlots_;
  CityVoxelRangeAllocator bricks_, words_;
  std::vector<Pending> pending_;
  uint32_t delay_ = 0, liveGeometries_ = 0, liveAppearances_ = 0;
};

} // namespace virtualgeometry
