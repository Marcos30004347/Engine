#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace procedural::world {

// Items by axis-aligned box on a uniform grid, for "is anything near this
// point" queries that used to scan a whole city per candidate. An item's box
// must already include whatever margin its own test adds (a road's half
// width, a door's clearance); a query box is the point plus the caller's
// radius. Every item whose box overlaps it is visited once; the caller
// still runs its exact test.
class SpatialBins {
public:
  explicit SpatialBins(double cellMetres = 8.0) : cell_(cellMetres) {}

  uint32_t insert(double minX, double minZ, double maxX, double maxZ) {
    const uint32_t id = uint32_t(stamp_.size());
    stamp_.push_back(0u);
    // A box that is not finite (an empty ring's bounds) or spans a huge
    // area is kept aside and offered to every query instead of filling
    // millions of cells.
    if (!(std::isfinite(minX) && std::isfinite(minZ) && std::isfinite(maxX) && std::isfinite(maxZ)) ||
        maxX < minX || maxZ < minZ || (maxX - minX) / cell_ > 256.0 || (maxZ - minZ) / cell_ > 256.0) {
      everywhere_.push_back(id);
      return id;
    }
    const auto [x0, z0] = key(minX, minZ);
    const auto [x1, z1] = key(maxX, maxZ);
    for (int64_t x = x0; x <= x1; ++x)
      for (int64_t z = z0; z <= z1; ++z) cells_[pack(x, z)].push_back(id);
    return id;
  }

  // Calls `hit(id)` for each candidate until it returns true; returns
  // whether any did.
  template <class Hit> bool any(double minX, double minZ, double maxX, double maxZ, Hit &&hit) const {
    if (++epoch_ == 0u) { std::fill(stamp_.begin(), stamp_.end(), 0u); epoch_ = 1u; }
    for (const uint32_t id : everywhere_) {
      stamp_[id] = epoch_;
      if (hit(id)) return true;
    }
    if (!(std::isfinite(minX) && std::isfinite(minZ) && std::isfinite(maxX) && std::isfinite(maxZ))) return false;
    const auto [x0, z0] = key(minX, minZ);
    const auto [x1, z1] = key(maxX, maxZ);
    if (double(x1 - x0 + 1) * double(z1 - z0 + 1) > double(cells_.size())) {
      // A query wider than the occupied grid visits each occupied cell once.
      for (const auto &[packed, ids] : cells_) {
        const int64_t x = int32_t(uint32_t(packed >> 32)), z = int32_t(uint32_t(packed));
        if (x < x0 || x > x1 || z < z0 || z > z1) continue;
        for (const uint32_t id : ids) {
          if (stamp_[id] == epoch_) continue;
          stamp_[id] = epoch_;
          if (hit(id)) return true;
        }
      }
      return false;
    }
    for (int64_t x = x0; x <= x1; ++x)
      for (int64_t z = z0; z <= z1; ++z) {
        const auto found = cells_.find(pack(x, z));
        if (found == cells_.end()) continue;
        for (const uint32_t id : found->second) {
          if (stamp_[id] == epoch_) continue;
          stamp_[id] = epoch_;
          if (hit(id)) return true;
        }
      }
    return false;
  }
  template <class Hit> bool any(double x, double z, double radius, Hit &&hit) const {
    return any(x - radius, z - radius, x + radius, z + radius, std::forward<Hit>(hit));
  }
  size_t size() const { return stamp_.size(); }

private:
  std::pair<int64_t, int64_t> key(double x, double z) const {
    return {int64_t(std::floor(x / cell_)), int64_t(std::floor(z / cell_))};
  }
  static uint64_t pack(int64_t x, int64_t z) { return (uint64_t(uint32_t(int32_t(x))) << 32) | uint32_t(int32_t(z)); }
  double cell_;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
  std::vector<uint32_t> everywhere_;
  mutable std::vector<uint32_t> stamp_;
  mutable uint32_t epoch_ = 0u;
};

} // namespace procedural::world
