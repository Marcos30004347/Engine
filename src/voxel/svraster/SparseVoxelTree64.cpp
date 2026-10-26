#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

uint32_t populationCount(uint64_t value)
{
  uint32_t result = 0u;
  while (value != 0u)
  {
    value &= value - 1u;
    ++result;
  }
  return result;
}

uint64_t cellCount(const uint32_t side)
{
  return uint64_t{side} * side * side;
}

size_t cellIndex(const uint32_t side, const uint32_t x, const uint32_t y, const uint32_t z)
{
  return static_cast<size_t>(x) + static_cast<size_t>(side) * (static_cast<size_t>(y) + static_cast<size_t>(side) * z);
}

bool regionOccupied(const std::vector<uint8_t> &cells, const uint32_t side, const std::array<uint32_t, 3> &origin, const uint32_t size)
{
  for (uint32_t z = origin[2]; z < origin[2] + size; ++z)
    for (uint32_t y = origin[1]; y < origin[1] + size; ++y)
      for (uint32_t x = origin[0]; x < origin[0] + size; ++x)
        if (cells[cellIndex(side, x, y, z)] != 0u)
          return true;
  return false;
}

void setNodeMask(SparseVoxelTree64NodeGPU &node, const uint64_t mask)
{
  node.populationMaskLo = static_cast<uint32_t>(mask);
  node.populationMaskHi = static_cast<uint32_t>(mask >> 32u);
}

void buildNode(const std::vector<uint8_t> &cells, SparseVoxelTree64Chunk &chunk, const uint32_t nodeIndex, const std::array<uint32_t, 3> &origin, const uint32_t size)
{
  if (size == kSparseVoxelTree64Branching)
  {
    uint64_t mask = 0u;
    for (uint32_t z = 0u; z < 4u; ++z)
      for (uint32_t y = 0u; y < 4u; ++y)
        for (uint32_t x = 0u; x < 4u; ++x)
          if (cells[cellIndex(chunk.side, origin[0] + x, origin[1] + y, origin[2] + z)] != 0u)
            mask |= uint64_t{1u} << sparseVoxelTree64ChildIndex(x, y, z);
    chunk.nodes[nodeIndex].childBaseAndFlags = kSparseVoxelTree64TerminalFlag;
    setNodeMask(chunk.nodes[nodeIndex], mask);
    return;
  }

  const uint32_t childSize = size / kSparseVoxelTree64Branching;
  uint64_t mask = 0u;
  std::array<std::array<uint32_t, 3>, kSparseVoxelTree64ChildCount> childOrigins{};
  for (uint32_t z = 0u; z < 4u; ++z)
    for (uint32_t y = 0u; y < 4u; ++y)
      for (uint32_t x = 0u; x < 4u; ++x)
      {
        const uint32_t child = sparseVoxelTree64ChildIndex(x, y, z);
        childOrigins[child] = {
          origin[0] + x * childSize,
          origin[1] + y * childSize,
          origin[2] + z * childSize,
        };
        if (regionOccupied(cells, chunk.side, childOrigins[child], childSize))
          mask |= uint64_t{1u} << child;
      }
  setNodeMask(chunk.nodes[nodeIndex], mask);
  const uint32_t childCount = populationCount(mask);
  if (childCount == 0u)
    return;
  const uint32_t childBase = static_cast<uint32_t>(chunk.nodes.size());
  chunk.nodes.resize(chunk.nodes.size() + childCount);
  chunk.nodes[nodeIndex].childBaseAndFlags = childBase << 1u;
  for (uint32_t child = 0u; child < kSparseVoxelTree64ChildCount; ++child)
  {
    if ((mask & (uint64_t{1u} << child)) == 0u)
      continue;
    const uint32_t compactChild = populationCount(mask & ((uint64_t{1u} << child) - 1u));
    buildNode(cells, chunk, childBase + compactChild, childOrigins[child], childSize);
  }
}

bool validateNode(const SparseVoxelTree64Chunk &chunk, const uint32_t nodeIndex, const uint32_t size, const bool root, std::vector<uint8_t> &visited, std::string *error)
{
  if (nodeIndex >= chunk.nodes.size())
    return fail(error, "Tree64 child index is out of range");
  if (visited[nodeIndex] != 0u)
    return fail(error, "Tree64 node is reachable more than once");
  visited[nodeIndex] = 1u;
  const auto &node = chunk.nodes[nodeIndex];
  const uint64_t mask = sparseVoxelTree64PopulationMask(node);
  if (sparseVoxelTree64NodeTerminal(node))
  {
    if (sparseVoxelTree64NodeChildBase(node) != 0u)
      return fail(error, "Tree64 terminal node has a child pointer");
    if (!root && mask == 0u)
      return fail(error, "Tree64 contains an empty referenced terminal");
    return true;
  }
  if (size == kSparseVoxelTree64Branching)
    return fail(error, "Tree64 terminal level is missing its flag");
  const uint32_t childCount = populationCount(mask);
  const uint32_t childBase = sparseVoxelTree64NodeChildBase(node);
  if (childCount == 0u)
  {
    if (!root)
      return fail(error, "Tree64 contains an empty referenced internal node");
    if (childBase != 0u)
      return fail(error, "Empty Tree64 root has a child pointer");
    return true;
  }
  if (childBase == 0u || uint64_t{childBase} + childCount > chunk.nodes.size())
    return fail(error, "Tree64 child block is out of range");
  const uint32_t childSize = size / kSparseVoxelTree64Branching;
  for (uint32_t child = 0u; child < childCount; ++child)
    if (!validateNode(chunk, childBase + child, childSize, false, visited, error))
      return false;
  return true;
}

struct SparseModelBox
{
  std::array<uint32_t, 3> minimum{};
  std::array<uint32_t, 3> maximum{};
};

bool buildSparseModelNode(
    const std::vector<SparseModelBox> &boxes,
    const std::vector<uint32_t> &boxIndices,
    SparseVoxelTree64Chunk &chunk,
    const uint32_t nodeIndex,
    const std::array<uint32_t, 3> &origin,
    const uint32_t size,
    std::string *error)
{
  if (size == kSparseVoxelTree64Branching)
  {
    uint64_t mask = 0u;
    for (const uint32_t boxIndex : boxIndices)
    {
      const auto &box = boxes[boxIndex];
      for (uint32_t z = std::max(origin[2], box.minimum[2]);
           z < std::min(origin[2] + size, box.maximum[2]); ++z)
        for (uint32_t y = std::max(origin[1], box.minimum[1]);
             y < std::min(origin[1] + size, box.maximum[1]); ++y)
          for (uint32_t x = std::max(origin[0], box.minimum[0]);
               x < std::min(origin[0] + size, box.maximum[0]); ++x)
            mask |= uint64_t{1u} << sparseVoxelTree64ChildIndex(
                x - origin[0], y - origin[1], z - origin[2]);
    }
    chunk.nodes[nodeIndex].childBaseAndFlags =
        kSparseVoxelTree64TerminalFlag;
    setNodeMask(chunk.nodes[nodeIndex], mask);
    return true;
  }

  const uint32_t childSize = size / kSparseVoxelTree64Branching;
  std::array<std::vector<uint32_t>, kSparseVoxelTree64ChildCount>
      childBoxes;
  std::array<bool, kSparseVoxelTree64ChildCount> fullChildren{};
  for (const uint32_t boxIndex : boxIndices)
  {
    const auto &box = boxes[boxIndex];
    std::array<uint32_t, 3> first{};
    std::array<uint32_t, 3> last{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      first[axis] = std::min(
          3u, (std::max(box.minimum[axis], origin[axis]) - origin[axis]) /
                  childSize);
      last[axis] = std::min(
          3u,
          (std::min(box.maximum[axis], origin[axis] + size) - 1u -
           origin[axis]) /
              childSize);
    }
    for (uint32_t z = first[2]; z <= last[2]; ++z)
      for (uint32_t y = first[1]; y <= last[1]; ++y)
        for (uint32_t x = first[0]; x <= last[0]; ++x)
        {
          const uint32_t child =
              sparseVoxelTree64ChildIndex(x, y, z);
          if (fullChildren[child])
            continue;
          const std::array<uint32_t, 3> childOrigin{
            origin[0] + x * childSize,
            origin[1] + y * childSize,
            origin[2] + z * childSize,
          };
          bool covers = true;
          for (uint32_t axis = 0u; axis < 3u; ++axis)
            covers &= box.minimum[axis] <= childOrigin[axis] &&
                box.maximum[axis] >= childOrigin[axis] + childSize;
          if (covers)
          {
            fullChildren[child] = true;
            childBoxes[child].clear();
          }
          else
          {
            childBoxes[child].push_back(boxIndex);
          }
        }
  }

  uint64_t mask = 0u;
  bool allOccupiedChildrenAreFull = true;
  for (uint32_t child = 0u; child < kSparseVoxelTree64ChildCount; ++child)
  {
    if (!fullChildren[child] && childBoxes[child].empty())
      continue;
    mask |= uint64_t{1u} << child;
    allOccupiedChildrenAreFull &= fullChildren[child];
  }
  setNodeMask(chunk.nodes[nodeIndex], mask);
  if (mask == 0u)
    return true;
  if (allOccupiedChildrenAreFull)
  {
    chunk.nodes[nodeIndex].childBaseAndFlags =
        kSparseVoxelTree64TerminalFlag;
    return true;
  }

  const uint32_t childCount = populationCount(mask);
  if (chunk.nodes.size() + childCount >
      (uint64_t{std::numeric_limits<uint32_t>::max()} >> 1u))
    return fail(error, "Tree64 model exceeds packed child addressing");
  const uint32_t childBase = static_cast<uint32_t>(chunk.nodes.size());
  chunk.nodes.resize(chunk.nodes.size() + childCount);
  chunk.nodes[nodeIndex].childBaseAndFlags = childBase << 1u;
  for (uint32_t child = 0u; child < kSparseVoxelTree64ChildCount; ++child)
  {
    if ((mask & (uint64_t{1u} << child)) == 0u)
      continue;
    const uint32_t compactChild =
        populationCount(mask & ((uint64_t{1u} << child) - 1u));
    const uint32_t target = childBase + compactChild;
    if (fullChildren[child])
    {
      chunk.nodes[target].childBaseAndFlags =
          kSparseVoxelTree64TerminalFlag;
      setNodeMask(chunk.nodes[target], ~uint64_t{0u});
      continue;
    }
    const uint32_t x = child & 3u;
    const uint32_t y = (child >> 2u) & 3u;
    const uint32_t z = (child >> 4u) & 3u;
    if (!buildSparseModelNode(
            boxes, childBoxes[child], chunk, target,
            {
              origin[0] + x * childSize,
              origin[1] + y * childSize,
              origin[2] + z * childSize,
            },
            childSize, error))
      return false;
  }
  return true;
}

bool validateTree(
    const SparseVoxelTree64Chunk &chunk,
    const bool modelSide,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (modelSide ? !sparseVoxelTree64SupportedModelSide(chunk.side)
                : !sparseVoxelTree64SupportedSide(chunk.side))
    return fail(error, "Tree64 chunk side is invalid");
  if (chunk.nodes.empty())
    return fail(error, "Tree64 chunk has no root");
  if (chunk.nodes.size() >
      (uint64_t{std::numeric_limits<uint32_t>::max()} >> 1u))
    return fail(error, "Tree64 chunk exceeds packed child addressing");
  if (!modelSide)
  {
    const uint32_t maximumNodes =
        sparseVoxelTree64MaximumNodeCount(chunk.side);
    if (maximumNodes == 0u || chunk.nodes.size() > maximumNodes)
      return fail(error, "Tree64 chunk exceeds its maximum node count");
  }
  std::vector<uint8_t> visited(chunk.nodes.size(), 0u);
  if (!validateNode(chunk, 0u, chunk.side, true, visited, error))
    return false;
  if (std::any_of(
          visited.begin(), visited.end(),
          [](const uint8_t value)
          {
            return value == 0u;
          }))
    return fail(error, "Tree64 chunk contains unreachable nodes");
  return true;
}
} // namespace

static_assert(sizeof(SparseVoxelTree64NodeGPU) == 12u, "Tree64 nodes must retain the packed 12-byte ABI");
static_assert(offsetof(SparseVoxelTree64NodeGPU, populationMaskLo) == 4u && offsetof(SparseVoxelTree64NodeGPU, populationMaskHi) == 8u, "Tree64 node fields must retain their reflected offsets");
static_assert(sizeof(SparseVoxelTree64ChunkGPU) == 32u, "Tree64 directory entries must retain the 32-byte ABI");
static_assert(offsetof(SparseVoxelTree64ChunkGPU, nodeSlot) == 12u && offsetof(SparseVoxelTree64ChunkGPU, flags) == 28u, "Tree64 directory fields must retain their reflected offsets");
static_assert(sizeof(SparseVoxelTree64StateGPU) == 112u, "Tree64 state must retain its seven-vec4 ABI");
static_assert(
    offsetof(SparseVoxelTree64StateGPU, originWorldChunk) == 16u && offsetof(SparseVoxelTree64StateGPU, wrapOffset) == 32u && offsetof(SparseVoxelTree64StateGPU, originCell) == 48u &&
        offsetof(SparseVoxelTree64StateGPU, logicalCellResolution) == 64u && offsetof(SparseVoxelTree64StateGPU, cellSize) == 80u && offsetof(SparseVoxelTree64StateGPU, worldExtent) == 96u &&
        offsetof(SparseVoxelTree64StateGPU, overflowStage) == 108u,
    "Tree64 state fields must retain their reflected offsets");

bool sparseVoxelTree64SupportedSide(const uint32_t side)
{
  if (side < kSparseVoxelTree64Branching || side > 64u)
    return false;
  return sparseVoxelTree64SupportedModelSide(side);
}

bool sparseVoxelTree64SupportedModelSide(const uint32_t side)
{
  if (side < kSparseVoxelTree64Branching || side > 65536u)
    return false;
  uint32_t value = side;
  while (value > 1u)
  {
    if ((value % kSparseVoxelTree64Branching) != 0u)
      return false;
    value /= kSparseVoxelTree64Branching;
  }
  return true;
}

uint32_t sparseVoxelTree64Depth(const uint32_t side)
{
  if (!sparseVoxelTree64SupportedSide(side))
    return 0u;
  return sparseVoxelTree64ModelDepth(side);
}

uint32_t sparseVoxelTree64ModelDepth(const uint32_t side)
{
  if (!sparseVoxelTree64SupportedModelSide(side))
    return 0u;
  uint32_t depth = 0u;
  for (uint32_t value = side; value > 1u; value /= kSparseVoxelTree64Branching)
    ++depth;
  return depth;
}

uint32_t sparseVoxelTree64MaximumNodeCount(const uint32_t side)
{
  const uint32_t depth = sparseVoxelTree64Depth(side);
  if (depth == 0u)
    return 0u;
  uint64_t levelNodes = 1u;
  uint64_t total = 0u;
  for (uint32_t level = 0u; level < depth; ++level)
  {
    total += levelNodes;
    levelNodes *= kSparseVoxelTree64ChildCount;
  }
  return total <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(total) : 0u;
}

uint32_t sparseVoxelTree64ChildIndex(const uint32_t x, const uint32_t y, const uint32_t z)
{
  return x + kSparseVoxelTree64Branching * y + kSparseVoxelTree64Branching * kSparseVoxelTree64Branching * z;
}

uint64_t sparseVoxelTree64PopulationMask(const SparseVoxelTree64NodeGPU &node)
{
  return uint64_t{node.populationMaskLo} | (uint64_t{node.populationMaskHi} << 32u);
}

uint32_t sparseVoxelTree64PopulationCountBefore(const SparseVoxelTree64NodeGPU &node, const uint32_t childIndex)
{
  if (childIndex == 0u)
    return 0u;
  const uint64_t prefix = childIndex >= 64u ? std::numeric_limits<uint64_t>::max() : (uint64_t{1u} << childIndex) - 1u;
  return populationCount(sparseVoxelTree64PopulationMask(node) & prefix);
}

bool sparseVoxelTree64NodeTerminal(const SparseVoxelTree64NodeGPU &node)
{
  return (node.childBaseAndFlags & kSparseVoxelTree64TerminalFlag) != 0u;
}

uint32_t sparseVoxelTree64NodeChildBase(const SparseVoxelTree64NodeGPU &node)
{
  return node.childBaseAndFlags >> 1u;
}

int32_t sparseVoxelTree64FloorDivide(const int32_t value, const uint32_t divisor)
{
  if (divisor == 0u)
    return 0;
  const int64_t wideValue = value;
  const int64_t wideDivisor = divisor;
  int64_t quotient = wideValue / wideDivisor;
  const int64_t remainder = wideValue % wideDivisor;
  if (remainder < 0)
    --quotient;
  return static_cast<int32_t>(quotient);
}

uint32_t sparseVoxelTree64PositiveModulo(const int32_t value, const uint32_t divisor)
{
  if (divisor == 0u)
    return 0u;
  const int64_t remainder = static_cast<int64_t>(value) % static_cast<int64_t>(divisor);
  return static_cast<uint32_t>(remainder < 0 ? remainder + divisor : remainder);
}

SparseVoxelTree64CellAddress sparseVoxelTree64CellAddress(const std::array<int32_t, 3> &globalCell, const uint32_t chunkSide)
{
  SparseVoxelTree64CellAddress result{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    result.worldChunk[axis] = sparseVoxelTree64FloorDivide(globalCell[axis], chunkSide);
    result.chunkCell[axis] = sparseVoxelTree64PositiveModulo(globalCell[axis], chunkSide);
  }
  return result;
}

bool sparseVoxelTree64DirectorySlot(
    const std::array<int32_t, 3> &worldChunk,
    const std::array<int32_t, 3> &originWorldChunk,
    const std::array<uint32_t, 3> &logicalResolution,
    const std::array<uint32_t, 3> &wrapOffset,
    std::array<uint32_t, 3> &physicalSlot)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (logicalResolution[axis] == 0u)
      return false;
    const int64_t logical = static_cast<int64_t>(worldChunk[axis]) - originWorldChunk[axis];
    if (logical < 0 || logical >= static_cast<int64_t>(logicalResolution[axis]))
      return false;
    physicalSlot[axis] = static_cast<uint32_t>((logical + wrapOffset[axis]) % logicalResolution[axis]);
  }
  return true;
}

bool buildSparseVoxelTree64(const uint32_t side, const std::vector<uint8_t> &occupiedCells, SparseVoxelTree64Chunk &chunk, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!sparseVoxelTree64SupportedSide(side))
    return fail(error, "Tree64 side must be 4, 16, or 64");
  if (occupiedCells.size() != cellCount(side))
    return fail(error, "Tree64 cell count does not match its side");
  SparseVoxelTree64Chunk next{};
  next.side = side;
  next.nodes.resize(1u);
  buildNode(occupiedCells, next, 0u, {0u, 0u, 0u}, side);
  if (!validateSparseVoxelTree64(next, error))
    return false;
  chunk = std::move(next);
  return true;
}

bool buildSparseVoxelTree64(const uint32_t side, const std::vector<SparseVoxelTree64AABB> &aabbs, SparseVoxelTree64Chunk &chunk, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!sparseVoxelTree64SupportedSide(side))
    return fail(error, "Tree64 side must be 4, 16, or 64");
  std::vector<uint8_t> occupied(static_cast<size_t>(cellCount(side)), 0u);
  for (const SparseVoxelTree64AABB &aabb : aabbs)
  {
    std::array<int32_t, 3> begin{};
    std::array<int32_t, 3> end{};
    bool empty = false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      if (!std::isfinite(aabb.minimum[axis]) || !std::isfinite(aabb.maximum[axis]) || aabb.maximum[axis] < aabb.minimum[axis])
        return fail(error, "Tree64 AABB is invalid");
      if (aabb.maximum[axis] == aabb.minimum[axis])
        empty = true;
      const double clippedMinimum = std::clamp(aabb.minimum[axis], 0.0, static_cast<double>(side));
      const double clippedMaximum = std::clamp(aabb.maximum[axis], 0.0, static_cast<double>(side));
      begin[axis] = static_cast<int32_t>(std::floor(clippedMinimum));
      end[axis] = static_cast<int32_t>(std::ceil(clippedMaximum));
      empty = empty || begin[axis] >= end[axis];
    }
    if (empty)
      continue;
    for (int32_t z = begin[2]; z < end[2]; ++z)
      for (int32_t y = begin[1]; y < end[1]; ++y)
        for (int32_t x = begin[0]; x < end[0]; ++x)
          occupied[cellIndex(side, static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z))] = 1u;
  }
  return buildSparseVoxelTree64(side, occupied, chunk, error);
}

bool buildConservativeSparseVoxelTree64(
    const uint32_t sourceSide,
    const std::vector<uint8_t> &sourceOccupiedCells,
    const uint32_t targetSide,
    SparseVoxelTree64Chunk &chunk,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!sparseVoxelTree64SupportedSide(sourceSide) ||
      !sparseVoxelTree64SupportedSide(targetSide))
    return fail(error, "Conservative Tree64 sides must be 4, 16, or 64");
  if (targetSide > sourceSide || sourceSide % targetSide != 0u)
    return fail(
        error,
        "Conservative Tree64 target side must divide the source side");
  if (sourceOccupiedCells.size() != cellCount(sourceSide))
    return fail(
        error,
        "Conservative Tree64 source cell count does not match its side");

  const uint32_t reduction = sourceSide / targetSide;
  std::vector<uint8_t> targetOccupiedCells(
      static_cast<size_t>(cellCount(targetSide)), 0u);
  for (uint32_t z = 0u; z < sourceSide; ++z)
    for (uint32_t y = 0u; y < sourceSide; ++y)
      for (uint32_t x = 0u; x < sourceSide; ++x)
      {
        if (sourceOccupiedCells[cellIndex(sourceSide, x, y, z)] == 0u)
          continue;
        targetOccupiedCells[cellIndex(
            targetSide, x / reduction, y / reduction, z / reduction)] = 1u;
      }
  return buildSparseVoxelTree64(
      targetSide, targetOccupiedCells, chunk, error);
}

bool buildSparseModelBoxes(const uint32_t side,
    const std::vector<SparseModelBox> &boxes,
    SparseVoxelTree64Chunk &chunk, std::string *error)
{
  SparseVoxelTree64Chunk next{};
  next.side = side;
  next.nodes.resize(1u);
  std::vector<uint32_t> indices(boxes.size());
  for (uint32_t index = 0u; index < indices.size(); ++index)
    indices[index] = index;
  if (!buildSparseModelNode(boxes, indices, next, 0u, {0u, 0u, 0u}, side, error) ||
      !validateSparseVoxelTree64Model(next, error))
    return false;
  chunk = std::move(next);
  return true;
}

bool buildSparseVoxelTree64Model(
    const uint32_t side,
    const std::vector<SparseVoxelTree64AABB> &aabbs,
    SparseVoxelTree64Chunk &chunk,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!sparseVoxelTree64SupportedModelSide(side))
    return fail(
        error,
        "Tree64 model side must be a power of four from 4 through 65536");
  std::vector<SparseModelBox> boxes;
  boxes.reserve(aabbs.size());
  for (const auto &aabb : aabbs)
  {
    SparseModelBox box{};
    bool empty = false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      if (!std::isfinite(aabb.minimum[axis]) ||
          !std::isfinite(aabb.maximum[axis]) ||
          aabb.maximum[axis] < aabb.minimum[axis])
        return fail(error, "Tree64 model AABB is invalid");
      const double clippedMinimum =
          std::clamp(aabb.minimum[axis], 0.0, static_cast<double>(side));
      const double clippedMaximum =
          std::clamp(aabb.maximum[axis], 0.0, static_cast<double>(side));
      box.minimum[axis] =
          static_cast<uint32_t>(std::floor(clippedMinimum));
      box.maximum[axis] =
          static_cast<uint32_t>(std::ceil(clippedMaximum));
      empty |= box.minimum[axis] >= box.maximum[axis];
    }
    if (!empty)
      boxes.push_back(box);
  }
  return buildSparseModelBoxes(side, boxes, chunk, error);
}

bool buildSparseVoxelTree64ModelCells(
    const uint32_t side,
    const std::vector<std::array<int32_t, 3>> &cells,
    SparseVoxelTree64Chunk &chunk,
    std::string *error)
{
  if (error) error->clear();
  if (!sparseVoxelTree64SupportedModelSide(side) || cells.empty())
    return fail(error, "Tree64 city model needs non-empty cells and a power-of-four side");
  std::vector<SparseModelBox> boxes;
  boxes.reserve(cells.size());
  for (const auto &cell : cells)
  {
    SparseModelBox box{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      if (cell[axis] < 0 || uint32_t(cell[axis]) >= side)
        return fail(error, "Tree64 city cell lies outside its model side");
      box.minimum[axis] = uint32_t(cell[axis]);
      box.maximum[axis] = box.minimum[axis] + 1u;
    }
    boxes.push_back(box);
  }
  return buildSparseModelBoxes(side, boxes, chunk, error);
}

bool validateSparseVoxelTree64(const SparseVoxelTree64Chunk &chunk, std::string *error)
{
  return validateTree(chunk, false, error);
}

bool validateSparseVoxelTree64Model(
    const SparseVoxelTree64Chunk &chunk,
    std::string *error)
{
  return validateTree(chunk, true, error);
}

bool sparseVoxelTree64Occupied(const SparseVoxelTree64Chunk &chunk, const std::array<uint32_t, 3> &cell)
{
  if (chunk.nodes.empty() || cell[0] >= chunk.side || cell[1] >= chunk.side || cell[2] >= chunk.side)
    return false;
  uint32_t nodeIndex = 0u;
  uint32_t size = chunk.side;
  std::array<uint32_t, 3> origin{};
  while (nodeIndex < chunk.nodes.size())
  {
    const auto &node = chunk.nodes[nodeIndex];
    const uint32_t childSize = size / kSparseVoxelTree64Branching;
    const uint32_t childX = (cell[0] - origin[0]) / childSize;
    const uint32_t childY = (cell[1] - origin[1]) / childSize;
    const uint32_t childZ = (cell[2] - origin[2]) / childSize;
    const uint32_t child = sparseVoxelTree64ChildIndex(childX, childY, childZ);
    const uint64_t mask = sparseVoxelTree64PopulationMask(node);
    if ((mask & (uint64_t{1u} << child)) == 0u)
      return false;
    if (sparseVoxelTree64NodeTerminal(node))
      return true;
    const uint32_t childBase = sparseVoxelTree64NodeChildBase(node);
    nodeIndex = childBase + sparseVoxelTree64PopulationCountBefore(node, child);
    origin = {
      origin[0] + childX * childSize,
      origin[1] + childY * childSize,
      origin[2] + childZ * childSize,
    };
    size = childSize;
  }
  return false;
}
} // namespace voxel::svraster
