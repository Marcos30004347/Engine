#include "virtualgeometry/CityVoxelAtlasAllocator.hpp"

#undef NDEBUG
#include <cassert>
#include <iostream>
#include <random>
#include <set>

using namespace virtualgeometry;

int main() {
  {
    CityVoxelRangeAllocator ranges(100);
    const uint64_t a = ranges.allocate(30), b = ranges.allocate(30), c = ranges.allocate(30);
    assert(a == 0 && b == 30 && c == 60 && ranges.used() == 90);
    assert(ranges.allocate(20) == UINT64_MAX);      // Only 10 left.
    ranges.release(b, 30);
    assert(ranges.largestFree() == 30);
    ranges.release(a, 30);                           // Merges with b's hole.
    assert(ranges.largestFree() == 60);
    ranges.release(c, 30);                           // Merges both sides and the tail.
    assert(ranges.largestFree() == 100 && ranges.used() == 0);
  }
  {
    CityVoxelAtlasAllocator atlas;
    atlas.reset({2, 2, 64, 64}, 2);
    CityVoxelAtlasAllocator::Range g0, g1, g2;
    assert(atlas.allocateGeometry(10, g0) && atlas.allocateGeometry(10, g1));
    assert(g0.slot == 0 && g1.slot == 1);           // Low slots first.
    assert(!atlas.allocateGeometry(10, g2));         // Out of slots.
    atlas.releaseGeometry(g0, 100);
    atlas.retire(101);
    assert(!atlas.allocateGeometry(10, g2));         // Still in flight.
    std::vector<CityVoxelAtlasAllocator::Range> retired;
    atlas.retire(102, &retired);
    assert(retired.size() == 1 && retired[0].slot == 0);
    assert(atlas.allocateGeometry(10, g2) && g2.slot == 0 && g2.offset == 0);
    CityVoxelAtlasAllocator::Range a;
    assert(!atlas.allocateAppearance(65, a));        // Larger than the word pool.
    assert(atlas.allocateAppearance(64, a) && atlas.words().used() == 64);
  }
  {
    // Churn: random allocate/release never overlaps live ranges and always
    // returns the pool to empty.
    CityVoxelAtlasAllocator atlas;
    atlas.reset({512, 512, 1u << 16, 1u << 16}, 3);
    std::mt19937 random(7);
    std::vector<CityVoxelAtlasAllocator::Range> live;
    for (uint64_t frame = 0; frame < 4000; ++frame) {
      atlas.retire(frame);
      if (live.size() < 400 && random() % 3) {
        CityVoxelAtlasAllocator::Range r;
        if (atlas.allocateGeometry(1 + random() % 300, r)) live.push_back(r);
      } else if (!live.empty()) {
        const size_t i = random() % live.size();
        atlas.releaseGeometry(live[i], frame);
        live[i] = live.back(); live.pop_back();
      }
      std::set<uint32_t> slots;
      std::vector<std::pair<uint64_t, uint64_t>> spans;
      for (const auto &r : live) { assert(slots.insert(r.slot).second); spans.push_back({r.offset, r.count}); }
      std::sort(spans.begin(), spans.end());
      for (size_t i = 1; i < spans.size(); ++i)
        assert(spans[i - 1].first + spans[i - 1].second <= spans[i].first);
    }
    for (const auto &r : live) atlas.releaseGeometry(r, 4000);
    atlas.retire(4003);
    assert(atlas.liveGeometries() == 0 && atlas.bricks().used() == 0 &&
           atlas.bricks().largestFree() == (1u << 16));
  }
  std::cout << "City voxel atlas allocator passed\n";
}
