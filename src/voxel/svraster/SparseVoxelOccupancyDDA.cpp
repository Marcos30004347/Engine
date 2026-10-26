#include "voxel/svraster/SparseVoxelOccupancyDDA.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace voxel::svraster
{
namespace
{

struct TraceInterval
{
  double entry = 0.0;
  double exit = 0.0;
};

int64_t floorDivide(const int64_t value, const int64_t divisor)
{
  const int64_t quotient = value / divisor;
  const int64_t remainder = value % divisor;
  return quotient - (remainder < 0 ? 1 : 0);
}

bool validRay(const SparseVoxelOccupancyRay &ray)
{
  bool nonzeroDirection = false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(ray.origin[axis]) || !std::isfinite(ray.direction[axis]))
      return false;
    nonzeroDirection |= ray.direction[axis] != 0.0f;
  }
  return nonzeroDirection && !std::isnan(ray.maximumDistance) && ray.maximumDistance >= 0.0;
}

bool intersectVolume(
    const SparseVoxelOccupancyVolumeGPU &state,
    const SparseVoxelOccupancyRay &ray,
    TraceInterval &interval)
{
  interval.entry = 0.0;
  interval.exit = ray.maximumDistance;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double cellSize = state.cellSize[axis];
    const double minimum = static_cast<double>(state.originCell[axis]) * cellSize;
    const double maximum =
        (static_cast<double>(state.originCell[axis]) + state.logicalResolution[axis]) * cellSize;
    const double origin = ray.origin[axis];
    const double direction = ray.direction[axis];
    if (direction == 0.0)
    {
      // The represented volume is half-open. A parallel ray exactly on its
      // maximum face has no interior segment.
      if (origin < minimum || origin >= maximum)
        return false;
      continue;
    }
    double first = (minimum - origin) / direction;
    double last = (maximum - origin) / direction;
    if (first > last)
      std::swap(first, last);
    interval.entry = std::max(interval.entry, first);
    interval.exit = std::min(interval.exit, last);
    if (interval.exit < interval.entry)
      return false;
  }
  return interval.exit >= 0.0;
}

bool baseCellAt(
    const SparseVoxelOccupancyVolumeGPU &state,
    const SparseVoxelOccupancyRay &ray,
    const double distance,
    std::array<int32_t, 3> &globalCell)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    double position =
        static_cast<double>(ray.origin[axis]) +
        static_cast<double>(ray.direction[axis]) * distance;
    if (ray.direction[axis] > 0.0f)
      position = std::nextafter(position, std::numeric_limits<double>::infinity());
    else if (ray.direction[axis] < 0.0f)
      position = std::nextafter(position, -std::numeric_limits<double>::infinity());
    const double cell = std::floor(position / state.cellSize[axis]);
    if (cell < std::numeric_limits<int32_t>::min() ||
        cell > std::numeric_limits<int32_t>::max())
      return false;
    globalCell[axis] = static_cast<int32_t>(cell);
    const int64_t logical =
        static_cast<int64_t>(globalCell[axis]) - state.originCell[axis];
    if (logical < 0 || logical >= state.logicalResolution[axis])
      return false;
  }
  return true;
}

bool mipContains(
    const SparseVoxelOccupancyVolume &volume,
    const uint32_t level,
    const std::array<int32_t, 3> &globalCell)
{
  if (level == 0u)
  {
    const SparseVoxelOccupancyVolumeGPU &state = volume.state();
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t logical =
          static_cast<int64_t>(globalCell[axis]) - state.originCell[axis];
      if (logical < 0 || logical >= state.logicalResolution[axis])
        return false;
    }
    return true;
  }
  if (level > volume.mipLevels().size())
    return false;
  const SparseVoxelOccupancyMipLevelGPU &state =
      volume.mipLevels()[level - 1u].state;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t logical =
        static_cast<int64_t>(globalCell[axis]) - state.originCell[axis];
    if (logical < 0 || logical >= state.activeResolution[axis])
      return false;
  }
  return true;
}

std::array<int32_t, 3> mipCellForBaseCell(
    const std::array<int32_t, 3> &baseCell,
    const uint32_t scale)
{
  return {
    static_cast<int32_t>(floorDivide(baseCell[0], scale)),
    static_cast<int32_t>(floorDivide(baseCell[1], scale)),
    static_cast<int32_t>(floorDivide(baseCell[2], scale)),
  };
}

double cellExitDistance(
    const SparseVoxelOccupancyVolumeGPU &baseState,
    const SparseVoxelOccupancyRay &ray,
    const std::array<int32_t, 3> &globalMipCell,
    const uint32_t scale)
{
  double exit = std::numeric_limits<double>::infinity();
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double direction = ray.direction[axis];
    if (direction == 0.0)
      continue;
    const double baseBoundary =
        direction > 0.0
            ? (static_cast<double>(globalMipCell[axis]) + 1.0) * scale
            : static_cast<double>(globalMipCell[axis]) * scale;
    const double worldBoundary = baseBoundary * baseState.cellSize[axis];
    const double candidate =
        (worldBoundary - static_cast<double>(ray.origin[axis])) / direction;
    exit = std::min(exit, candidate);
  }
  return exit;
}

double advancePastExit(const double current, const double cellExit)
{
  const double boundary = std::max(current, cellExit);
  double next =
      std::nextafter(boundary, std::numeric_limits<double>::infinity());
  if (!(next > current))
    next = std::nextafter(current, std::numeric_limits<double>::infinity());
  return next;
}

uint32_t automaticQueryLimit(
    const SparseVoxelOccupancyVolume &volume,
    const bool hierarchical)
{
  const SparseVoxelOccupancyVolumeGPU &state = volume.state();
  uint64_t baseTransitions = 4u;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    baseTransitions += state.logicalResolution[axis];
  if (hierarchical)
    baseTransitions *= volume.mipLevels().size() + 1u;
  return static_cast<uint32_t>(
      std::min<uint64_t>(
          std::max<uint64_t>(baseTransitions, 1u),
          std::numeric_limits<uint32_t>::max()));
}

SparseVoxelOccupancyTraceResult initialResult(
    const SparseVoxelOccupancyVolume &volume,
    const SparseVoxelOccupancyRay &ray,
    TraceInterval &interval)
{
  SparseVoxelOccupancyTraceResult result{};
  if (volume.state().valid == 0u || !validRay(ray))
    return result;
  if (!intersectVolume(volume.state(), ray, interval))
  {
    result.status = SparseVoxelOccupancyTraceStatus::Miss;
    return result;
  }
  return result;
}

} // namespace

SparseVoxelOccupancyTraceResult traceSparseVoxelOccupancyMip0(
    const SparseVoxelOccupancyVolume &volume,
    const SparseVoxelOccupancyRay &ray,
    uint32_t maximumQueries)
{
  TraceInterval interval{};
  SparseVoxelOccupancyTraceResult result =
      initialResult(volume, ray, interval);
  if (result.status != SparseVoxelOccupancyTraceStatus::Invalid ||
      volume.state().valid == 0u || !validRay(ray))
    return result;
  if (maximumQueries == 0u)
    maximumQueries = automaticQueryLimit(volume, false);

  double current = interval.entry;
  while (current <= interval.exit)
  {
    if (result.queryCount >= maximumQueries)
    {
      result.status = SparseVoxelOccupancyTraceStatus::QueryLimit;
      result.distance = current;
      return result;
    }
    std::array<int32_t, 3> baseCell{};
    if (!baseCellAt(volume.state(), ray, current, baseCell))
      break;
    ++result.queryCount;
    ++result.baseCellSteps;
    if (volume.occupiedAtGlobalCell(0u, baseCell))
    {
      result.status = SparseVoxelOccupancyTraceStatus::Hit;
      result.globalCell = baseCell;
      result.distance = current;
      return result;
    }
    current = advancePastExit(
        current,
        cellExitDistance(volume.state(), ray, baseCell, 1u));
  }
  result.status = SparseVoxelOccupancyTraceStatus::Miss;
  result.distance = interval.exit;
  return result;
}

SparseVoxelOccupancyTraceResult traceSparseVoxelOccupancyHierarchy(
    const SparseVoxelOccupancyVolume &volume,
    const SparseVoxelOccupancyRay &ray,
    uint32_t maximumQueries)
{
  TraceInterval interval{};
  SparseVoxelOccupancyTraceResult result =
      initialResult(volume, ray, interval);
  if (result.status != SparseVoxelOccupancyTraceStatus::Invalid ||
      volume.state().valid == 0u || !validRay(ray))
    return result;
  if (maximumQueries == 0u)
    maximumQueries = automaticQueryLimit(volume, true);

  double current = interval.entry;
  const uint32_t levelCount =
      static_cast<uint32_t>(volume.mipLevels().size());
  while (current <= interval.exit)
  {
    std::array<int32_t, 3> baseCell{};
    if (!baseCellAt(volume.state(), ray, current, baseCell))
      break;

    bool advanced = false;
    for (uint32_t descending = levelCount + 1u; descending > 0u; --descending)
    {
      const uint32_t level = descending - 1u;
      const uint32_t scale =
          level == 0u ? 1u : volume.mipLevels()[level - 1u].state.scale;
      const std::array<int32_t, 3> mipCell =
          mipCellForBaseCell(baseCell, scale);
      if (!mipContains(volume, level, mipCell))
        continue;
      if (result.queryCount >= maximumQueries)
      {
        result.status = SparseVoxelOccupancyTraceStatus::QueryLimit;
        result.distance = current;
        return result;
      }
      ++result.queryCount;
      result.highestLevel = std::max(result.highestLevel, level);
      if (level == 0u)
        ++result.baseCellSteps;
      if (volume.occupiedAtGlobalCell(level, mipCell))
      {
        if (level == 0u)
        {
          result.status = SparseVoxelOccupancyTraceStatus::Hit;
          result.globalCell = baseCell;
          result.distance = current;
          return result;
        }
        continue;
      }

      const double next = advancePastExit(
          current,
          cellExitDistance(volume.state(), ray, mipCell, scale));
      if (level > 0u)
        result.skippedDistance += next - current;
      current = next;
      advanced = true;
      break;
    }
    if (!advanced)
      break;
  }
  result.status = SparseVoxelOccupancyTraceStatus::Miss;
  result.distance = interval.exit;
  return result;
}

} // namespace voxel::svraster
