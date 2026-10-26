#pragma once

#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace voxel::svraster
{
struct SparseVoxelTree64Ray
{
  std::array<double, 3> origin{};
  std::array<double, 3> direction{};
  double maximumDistance = std::numeric_limits<double>::infinity();
};

enum class SparseVoxelTree64TraceStatus : uint32_t
{
  Invalid = 0u,
  Miss = 1u,
  Hit = 2u,
  QueryLimit = 3u,
};

struct SparseVoxelTree64TraceResult
{
  SparseVoxelTree64TraceStatus status = SparseVoxelTree64TraceStatus::Invalid;
  std::array<int32_t, 3> cell{};
  double distance = 0.0;
  uint32_t queryCount = 0u;
  uint32_t nodeVisits = 0u;

  bool hit() const
  {
    return status == SparseVoxelTree64TraceStatus::Hit;
  }
};

SparseVoxelTree64TraceResult traceSparseVoxelTree64(const SparseVoxelTree64Chunk &chunk, const SparseVoxelTree64Ray &ray, uint32_t maximumQueries = std::numeric_limits<uint32_t>::max());

SparseVoxelTree64TraceResult traceSparseVoxelTree64CellsDDA(uint32_t side, const std::vector<uint8_t> &occupiedCells, const SparseVoxelTree64Ray &ray, uint32_t maximumQueries = std::numeric_limits<uint32_t>::max());
} // namespace voxel::svraster
