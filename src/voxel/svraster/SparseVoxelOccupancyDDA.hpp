#pragma once

#include "voxel/svraster/SparseVoxelOccupancyVolume.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace voxel::svraster
{

struct SparseVoxelOccupancyRay
{
  std::array<float, 3> origin{};
  std::array<float, 3> direction{};
  double maximumDistance = std::numeric_limits<double>::infinity();
};

enum class SparseVoxelOccupancyTraceStatus : uint32_t
{
  Invalid = 0u,
  Miss = 1u,
  Hit = 2u,
  QueryLimit = 3u,
};

struct SparseVoxelOccupancyTraceResult
{
  SparseVoxelOccupancyTraceStatus status = SparseVoxelOccupancyTraceStatus::Invalid;
  std::array<int32_t, 3> globalCell{};
  double distance = 0.0;
  double skippedDistance = 0.0;
  uint32_t queryCount = 0u;
  uint32_t baseCellSteps = 0u;
  uint32_t highestLevel = 0u;

  bool hit() const
  {
    return status == SparseVoxelOccupancyTraceStatus::Hit;
  }
};

// maximumQueries == 0 selects a conservative bound derived from the volume
// dimensions. Ray direction need not be normalized; returned distances use the
// ray's parametric t.
SparseVoxelOccupancyTraceResult traceSparseVoxelOccupancyMip0(
    const SparseVoxelOccupancyVolume &volume,
    const SparseVoxelOccupancyRay &ray,
    uint32_t maximumQueries = 0u);

SparseVoxelOccupancyTraceResult traceSparseVoxelOccupancyHierarchy(
    const SparseVoxelOccupancyVolume &volume,
    const SparseVoxelOccupancyRay &ray,
    uint32_t maximumQueries = 0u);

} // namespace voxel::svraster
