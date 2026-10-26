#include "voxel/svraster/SparseVoxelTree64Reference.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace voxel::svraster
{
namespace
{
struct Interval
{
  double enter = 0.0;
  double exit = 0.0;
};

bool validRay(const SparseVoxelTree64Ray &ray)
{
  if (!std::isfinite(ray.maximumDistance) && ray.maximumDistance != std::numeric_limits<double>::infinity())
    return false;
  if (ray.maximumDistance < 0.0)
    return false;
  bool hasDirection = false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(ray.origin[axis]) || !std::isfinite(ray.direction[axis]))
      return false;
    hasDirection = hasDirection || ray.direction[axis] != 0.0;
  }
  return hasDirection;
}

bool intersectHalfOpenBox(const SparseVoxelTree64Ray &ray, const std::array<uint32_t, 3> &origin, const uint32_t size, Interval &interval)
{
  double enter = 0.0;
  double exit = ray.maximumDistance;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double minimum = origin[axis];
    const double maximum = origin[axis] + size;
    const double direction = ray.direction[axis];
    if (direction == 0.0)
    {
      if (ray.origin[axis] < minimum || ray.origin[axis] >= maximum)
        return false;
      continue;
    }
    const double first = (minimum - ray.origin[axis]) / direction;
    const double second = (maximum - ray.origin[axis]) / direction;
    enter = std::max(enter, std::min(first, second));
    exit = std::min(exit, std::max(first, second));
    if (!(exit > enter))
      return false;
  }
  interval = {enter, exit};
  return exit > enter;
}

size_t cellIndex(const uint32_t side, const uint32_t x, const uint32_t y, const uint32_t z)
{
  return static_cast<size_t>(x) + static_cast<size_t>(side) * (static_cast<size_t>(y) + static_cast<size_t>(side) * z);
}

struct TreeTask
{
  uint32_t nodeIndex = 0u;
  std::array<uint32_t, 3> origin{};
  uint32_t size = 0u;
  Interval interval{};
  bool cell = false;
};

struct TreeTaskFarther
{
  bool operator()(const TreeTask &left, const TreeTask &right) const
  {
    if (left.interval.enter != right.interval.enter)
      return left.interval.enter > right.interval.enter;
    if (left.cell != right.cell)
      return left.cell < right.cell;
    if (left.origin[2] != right.origin[2])
      return left.origin[2] > right.origin[2];
    if (left.origin[1] != right.origin[1])
      return left.origin[1] > right.origin[1];
    return left.origin[0] > right.origin[0];
  }
};

bool pushChildTask(
    std::priority_queue<TreeTask, std::vector<TreeTask>, TreeTaskFarther> &tasks,
    const SparseVoxelTree64Ray &ray,
    const TreeTask &parent,
    const uint32_t childIndex,
    const uint32_t nodeIndex,
    const bool cell)
{
  const uint32_t childSize = parent.size / kSparseVoxelTree64Branching;
  const uint32_t childX = childIndex & 3u;
  const uint32_t childY = (childIndex >> 2u) & 3u;
  const uint32_t childZ = (childIndex >> 4u) & 3u;
  const std::array<uint32_t, 3> childOrigin{
    parent.origin[0] + childX * childSize,
    parent.origin[1] + childY * childSize,
    parent.origin[2] + childZ * childSize,
  };
  Interval interval{};
  if (!intersectHalfOpenBox(ray, childOrigin, childSize, interval))
    return false;
  interval.enter = std::max(interval.enter, parent.interval.enter);
  interval.exit = std::min(interval.exit, parent.interval.exit);
  if (!(interval.exit > interval.enter))
    return false;
  tasks.push({
    nodeIndex,
    childOrigin,
    childSize,
    interval,
    cell,
  });
  return true;
}
} // namespace

SparseVoxelTree64TraceResult traceSparseVoxelTree64(const SparseVoxelTree64Chunk &chunk, const SparseVoxelTree64Ray &ray, const uint32_t maximumQueries)
{
  SparseVoxelTree64TraceResult result{};
  if (!validRay(ray) ||
      !sparseVoxelTree64SupportedModelSide(chunk.side) ||
      chunk.nodes.empty())
    return result;
  Interval rootInterval{};
  if (!intersectHalfOpenBox(ray, {0u, 0u, 0u}, chunk.side, rootInterval))
  {
    result.status = SparseVoxelTree64TraceStatus::Miss;
    return result;
  }

  std::priority_queue<TreeTask, std::vector<TreeTask>, TreeTaskFarther> tasks;
  tasks.push({0u, {0u, 0u, 0u}, chunk.side, rootInterval, false});
  while (!tasks.empty())
  {
    if (result.queryCount >= maximumQueries)
    {
      result.status = SparseVoxelTree64TraceStatus::QueryLimit;
      return result;
    }
    ++result.queryCount;
    const TreeTask task = tasks.top();
    tasks.pop();
    if (task.cell)
    {
      result.status = SparseVoxelTree64TraceStatus::Hit;
      result.cell = {
        static_cast<int32_t>(task.origin[0]),
        static_cast<int32_t>(task.origin[1]),
        static_cast<int32_t>(task.origin[2]),
      };
      result.distance = task.interval.enter;
      return result;
    }
    if (task.nodeIndex >= chunk.nodes.size())
      return result;
    ++result.nodeVisits;
    const auto &node = chunk.nodes[task.nodeIndex];
    const uint64_t mask = sparseVoxelTree64PopulationMask(node);
    if (mask == 0u)
      continue;
    const bool terminal = sparseVoxelTree64NodeTerminal(node);
    const uint32_t childBase = sparseVoxelTree64NodeChildBase(node);
    for (uint32_t child = 0u; child < kSparseVoxelTree64ChildCount; ++child)
    {
      if ((mask & (uint64_t{1u} << child)) == 0u)
        continue;
      const uint32_t childNode = terminal ? 0u : childBase + sparseVoxelTree64PopulationCountBefore(node, child);
      pushChildTask(tasks, ray, task, child, childNode, terminal);
    }
  }
  result.status = SparseVoxelTree64TraceStatus::Miss;
  return result;
}

SparseVoxelTree64TraceResult traceSparseVoxelTree64CellsDDA(const uint32_t side, const std::vector<uint8_t> &occupiedCells, const SparseVoxelTree64Ray &ray, const uint32_t maximumQueries)
{
  SparseVoxelTree64TraceResult result{};
  // This is a dense semantic oracle for a single model, not the bounded
  // scene-directory traversal. Generated terrain pages may legitimately use
  // model sides above the directory's 64-cell limit (for example a shallow
  // 8x128x8 page is represented by a side-256 model). Rejecting those models
  // made the oracle return Invalid while the Tree64 reference correctly
  // returned Hit/Miss, hiding large-page GPU build regressions.
  if (!validRay(ray) || !sparseVoxelTree64SupportedModelSide(side) ||
      occupiedCells.size() != uint64_t{side} * side * side)
    return result;
  Interval volumeInterval{};
  if (!intersectHalfOpenBox(ray, {0u, 0u, 0u}, side, volumeInterval))
  {
    result.status = SparseVoxelTree64TraceStatus::Miss;
    return result;
  }

  double current = volumeInterval.enter;
  const double sample = std::nextafter(current, volumeInterval.exit);
  std::array<int32_t, 3> cell{};
  std::array<int32_t, 3> step{};
  std::array<double, 3> nextBoundary{};
  std::array<double, 3> delta{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    double coordinate = ray.origin[axis] + sample * ray.direction[axis];
    if (ray.direction[axis] != 0.0)
      coordinate = std::nextafter(coordinate, ray.direction[axis] > 0.0 ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity());
    cell[axis] = std::clamp(static_cast<int32_t>(std::floor(coordinate)), 0, static_cast<int32_t>(side) - 1);
    if (ray.direction[axis] > 0.0)
    {
      step[axis] = 1;
      nextBoundary[axis] = (static_cast<double>(cell[axis] + 1) - ray.origin[axis]) / ray.direction[axis];
      delta[axis] = 1.0 / ray.direction[axis];
    }
    else if (ray.direction[axis] < 0.0)
    {
      step[axis] = -1;
      nextBoundary[axis] = (static_cast<double>(cell[axis]) - ray.origin[axis]) / ray.direction[axis];
      delta[axis] = -1.0 / ray.direction[axis];
    }
    else
    {
      step[axis] = 0;
      nextBoundary[axis] = std::numeric_limits<double>::infinity();
      delta[axis] = std::numeric_limits<double>::infinity();
    }
    while (nextBoundary[axis] <= current)
      nextBoundary[axis] += delta[axis];
  }

  while (cell[0] >= 0 && cell[1] >= 0 && cell[2] >= 0 && cell[0] < static_cast<int32_t>(side) && cell[1] < static_cast<int32_t>(side) && cell[2] < static_cast<int32_t>(side) && current < volumeInterval.exit)
  {
    if (result.queryCount >= maximumQueries)
    {
      result.status = SparseVoxelTree64TraceStatus::QueryLimit;
      return result;
    }
    ++result.queryCount;
    if (occupiedCells[cellIndex(side, static_cast<uint32_t>(cell[0]), static_cast<uint32_t>(cell[1]), static_cast<uint32_t>(cell[2]))] != 0u)
    {
      result.status = SparseVoxelTree64TraceStatus::Hit;
      result.cell = cell;
      result.distance = current;
      return result;
    }
    const double next = std::min(nextBoundary[0], std::min(nextBoundary[1], nextBoundary[2]));
    if (!(next < volumeInterval.exit))
      break;
    current = next;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      if (nextBoundary[axis] == next)
      {
        cell[axis] += step[axis];
        nextBoundary[axis] += delta[axis];
      }
  }
  result.status = SparseVoxelTree64TraceStatus::Miss;
  return result;
}
} // namespace voxel::svraster
