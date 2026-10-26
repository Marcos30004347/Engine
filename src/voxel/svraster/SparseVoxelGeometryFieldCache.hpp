#pragma once

#include "voxel/material/VoxelMaterialFieldAtlas.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::material
{
class VoxelMaterialManager;
}

namespace voxel::svraster
{

struct SparseVoxelBrickHierarchy;

inline constexpr uint32_t kSparseVoxelGeometryFieldCacheValid = 1u << 31u;
inline constexpr uint32_t kSparseVoxelGeometryFieldCacheConservative =
    1u << 30u;
inline constexpr uint32_t kSparseVoxelGeometryFieldCacheSubdivisionShift =
    16u;
inline constexpr uint32_t kSparseVoxelGeometryFieldCacheSubdivisionMask =
    0x0fu;
inline constexpr uint32_t kSparseVoxelGeometryFieldCacheMaximumProbes = 8u;
inline constexpr uint32_t kSparseVoxelGeometryFieldCacheOccupancyWords = 16u;
inline constexpr uint32_t kMaximumSparseVoxelGeometryFieldCacheRecords =
    1u << 18u;

// Fixed-size per-parent materialization. Sixteen words cover the maximum
// 8^3 microcells. Both brick raster and Tree64 address this same table; no
// texture residency decision occurs during traversal.
struct alignas(16) SparseVoxelGeometryFieldCacheRecordGPU
{
  std::array<int32_t, 3> canonicalParent{};
  // material handle | subdivision << 16 | conservative << 30 | valid << 31
  uint32_t materialAndFlags = 0u;
  std::array<uint32_t, kSparseVoxelGeometryFieldCacheOccupancyWords>
      occupancy{};

  bool valid() const
  {
    return (materialAndFlags & kSparseVoxelGeometryFieldCacheValid) != 0u;
  }
  uint16_t materialHandle() const
  {
    return static_cast<uint16_t>(materialAndFlags & 0xffffu);
  }
  uint32_t subdivision() const
  {
    return (materialAndFlags >>
            kSparseVoxelGeometryFieldCacheSubdivisionShift) &
        kSparseVoxelGeometryFieldCacheSubdivisionMask;
  }
  bool conservative() const
  {
    return (materialAndFlags &
            kSparseVoxelGeometryFieldCacheConservative) != 0u;
  }
  bool operator==(const SparseVoxelGeometryFieldCacheRecordGPU &other) const
  {
    return canonicalParent == other.canonicalParent &&
        materialAndFlags == other.materialAndFlags &&
        occupancy == other.occupancy;
  }
};
static_assert(sizeof(SparseVoxelGeometryFieldCacheRecordGPU) == 80u);

struct SparseVoxelGeometryFieldCacheSnapshot
{
  std::vector<SparseVoxelGeometryFieldCacheRecordGPU> table;
  uint64_t contentRevision = 0u;
  uint64_t sourceAtlasRevision = 0u;
  uint32_t recordCount = 0u;
  uint32_t conservativeRecordCount = 0u;
};

uint32_t sparseVoxelGeometryFieldCacheHash(
    const std::array<int32_t, 3> &canonicalParent,
    material::VoxelMaterialHandle materialHandle);

const SparseVoxelGeometryFieldCacheRecordGPU *
findSparseVoxelGeometryFieldCacheRecord(
    const std::vector<SparseVoxelGeometryFieldCacheRecordGPU> &table,
    const std::array<int32_t, 3> &canonicalParent,
    material::VoxelMaterialHandle materialHandle,
    uint32_t *probes = nullptr);

// Builds only records for programs that read a material field. All loads are
// nearest mip-zero integer samples. If any required channel is absent, that
// parent is cached as fully occupied rather than evaluating authored defaults.
bool buildSparseVoxelGeometryFieldCache(
    const SparseVoxelBrickHierarchy &hierarchy,
    const material::VoxelMaterialManager &materials,
    const material::VoxelMaterialFieldAtlasSnapshot &atlas,
    SparseVoxelGeometryFieldCacheSnapshot &snapshot,
    std::string *error = nullptr);

// Missing entries are deliberately occupied. `found` distinguishes a cached
// solid/materialized answer from the conservative lookup fallback in tests and
// diagnostics; traversal only needs the returned occupancy.
bool sparseVoxelGeometryFieldCacheOccupied(
    const std::vector<SparseVoxelGeometryFieldCacheRecordGPU> &table,
    const std::array<int32_t, 3> &canonicalParent,
    material::VoxelMaterialHandle materialHandle,
    const std::array<uint32_t, 3> &microCell,
    bool *found = nullptr);

} // namespace voxel::svraster
