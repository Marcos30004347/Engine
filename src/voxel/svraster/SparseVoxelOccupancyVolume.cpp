#include "voxel/svraster/SparseVoxelOccupancyVolume.hpp"
#include "voxel/svraster/SparseVoxelFileRuntime.hpp"

#include <algorithm>
#include <cmath>
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

bool checkedMultiply(const uint64_t left, const uint64_t right, uint64_t &result)
{
  if (left != 0u && right > std::numeric_limits<uint64_t>::max() / left)
    return false;
  result = left * right;
  return true;
}

bool checkedAdd(const uint64_t left, const uint64_t right, uint64_t &result)
{
  if (right > std::numeric_limits<uint64_t>::max() - left)
    return false;
  result = left + right;
  return true;
}

uint64_t ceilDivide(const uint64_t value, const uint64_t divisor)
{
  return value / divisor + (value % divisor != 0u ? 1u : 0u);
}

int64_t floorDivide(const int64_t value, const int64_t divisor)
{
  const int64_t quotient = value / divisor;
  const int64_t remainder = value % divisor;
  return quotient - (remainder < 0 ? 1 : 0);
}

uint32_t positiveModulo(const int64_t value, const uint32_t modulus)
{
  const int64_t signedModulus = static_cast<int64_t>(modulus);
  const int64_t remainder = value % signedModulus;
  return static_cast<uint32_t>(remainder < 0 ? remainder + signedModulus : remainder);
}

uint32_t bitCount(uint32_t value)
{
  uint32_t count = 0u;
  while (value != 0u)
  {
    value &= value - 1u;
    ++count;
  }
  return count;
}

bool finiteAABB(const SparseVoxelAABB &bounds)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (!std::isfinite(bounds.minimum[axis]) || !std::isfinite(bounds.maximum[axis]) || bounds.minimum[axis] > bounds.maximum[axis])
      return false;
  return true;
}

std::array<float, 3> transformPoint(const std::array<float, 16> &matrix, const std::array<float, 3> &point)
{
  return {
    matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
    matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
    matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14],
  };
}

SparseVoxelAABB transformCubeBounds(
    const std::array<float, 16> &matrix,
    const std::array<float, 3> &center,
    const float halfLength)
{
  const std::array<float, 3> worldCenter =
      transformPoint(matrix, center);
  SparseVoxelAABB world{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    // For a cube, abs(linear transform) * localExtent is the exact
    // axis-aligned world extent. This replaces eight corner transforms.
    const float worldExtent =
        halfLength *
        (std::abs(matrix[axis]) +
         std::abs(matrix[4u + axis]) +
         std::abs(matrix[8u + axis]));
    world.minimum[axis] = worldCenter[axis] - worldExtent;
    world.maximum[axis] = worldCenter[axis] + worldExtent;
  }
  return world;
}

bool sameLayout(const SparseVoxelOccupancyLayout &layout, const SparseVoxelOccupancyVolumeGPU &state)
{
  return state.logicalResolution == layout.logicalResolution && state.wordResolution == layout.wordResolution && state.cellSize == layout.cellSize && state.worldExtent == layout.worldExtent;
}

bool cameraDecomposition(const SparseVoxelOccupancyLayout &layout, const std::array<float, 3> &camera, std::array<int32_t, 3> &cell, std::array<float, 3> &inner, std::array<int32_t, 3> &origin, std::string *error)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(camera[axis]))
      return fail(error, "Sparse voxel occupancy camera position is not finite");
    const double scaled = static_cast<double>(camera[axis]) / static_cast<double>(layout.cellSize[axis]);
    const double floored = std::floor(scaled);
    if (floored < static_cast<double>(std::numeric_limits<int32_t>::min()) || floored > static_cast<double>(std::numeric_limits<int32_t>::max()))
      return fail(error, "Sparse voxel occupancy camera cell exceeds signed 32-bit range");
    cell[axis] = static_cast<int32_t>(floored);
    inner[axis] = static_cast<float>(scaled - floored);
    if (inner[axis] < 0.0f)
      inner[axis] = 0.0f;
    if (inner[axis] >= 1.0f)
      inner[axis] = std::nextafter(1.0f, 0.0f);

    const int64_t signedOrigin = static_cast<int64_t>(cell[axis]) - static_cast<int64_t>(layout.logicalResolution[axis] / 2u);
    if (signedOrigin < std::numeric_limits<int32_t>::min() || signedOrigin > std::numeric_limits<int32_t>::max())
      return fail(error, "Sparse voxel occupancy origin cell exceeds signed 32-bit range");
    const int64_t signedLast = signedOrigin + static_cast<int64_t>(layout.logicalResolution[axis]) - 1;
    if (signedLast < std::numeric_limits<int32_t>::min() || signedLast > std::numeric_limits<int32_t>::max())
      return fail(error, "Sparse voxel occupancy represented cells exceed signed 32-bit range");
    origin[axis] = static_cast<int32_t>(signedOrigin);
  }
  return true;
}

bool newlyExposed1D(const uint32_t coordinate, const int32_t delta, const uint32_t extent)
{
  if (delta > 0)
    return coordinate >= extent - static_cast<uint32_t>(delta);
  if (delta < 0)
    return coordinate < static_cast<uint32_t>(-static_cast<int64_t>(delta));
  return false;
}

bool globalCellForWorld(const std::array<float, 3> &world, const std::array<float, 3> &cellSize, std::array<int64_t, 3> &globalCell)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(world[axis]))
      return false;
    const double scaled = static_cast<double>(world[axis]) / static_cast<double>(cellSize[axis]);
    const double floored = std::floor(scaled);
    if (floored < static_cast<double>(std::numeric_limits<int64_t>::min()) || floored > static_cast<double>(std::numeric_limits<int64_t>::max()))
      return false;
    globalCell[axis] = static_cast<int64_t>(floored);
  }
  return true;
}

bool mipContainsGlobalCell(const SparseVoxelOccupancyMipLevelGPU &state, const std::array<int32_t, 3> &globalCell)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t logical = static_cast<int64_t>(globalCell[axis]) - state.originCell[axis];
    if (logical < 0 || logical >= state.activeResolution[axis])
      return false;
  }
  return true;
}

bool baseContainsGlobalCell(const SparseVoxelOccupancyVolumeGPU &state, const std::array<int32_t, 3> &globalCell)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t logical = static_cast<int64_t>(globalCell[axis]) - state.originCell[axis];
    if (logical < 0 || logical >= state.logicalResolution[axis])
      return false;
  }
  return true;
}

bool occupiedBaseCell(const SparseVoxelOccupancyVolumeGPU &state, const std::vector<uint32_t> &words, const std::array<int32_t, 3> &globalCell)
{
  if (!baseContainsGlobalCell(state, globalCell))
    return false;
  const std::array<uint32_t, 3> logical{
    static_cast<uint32_t>(globalCell[0] - state.originCell[0]),
    static_cast<uint32_t>(globalCell[1] - state.originCell[1]),
    static_cast<uint32_t>(globalCell[2] - state.originCell[2]),
  };
  const SparseVoxelOccupancyAddress address = sparseVoxelOccupancyAddress(logical, state);
  return (words[address.wordIndex] & address.bitMask) != 0u;
}

bool occupiedMipCell(const SparseVoxelOccupancyMipLevelGPU &state, const std::vector<uint32_t> &words, const std::array<int32_t, 3> &globalCell)
{
  if (!mipContainsGlobalCell(state, globalCell))
    return false;
  const std::array<uint32_t, 3> logical{
    static_cast<uint32_t>(globalCell[0] - state.originCell[0]),
    static_cast<uint32_t>(globalCell[1] - state.originCell[1]),
    static_cast<uint32_t>(globalCell[2] - state.originCell[2]),
  };
  const SparseVoxelOccupancyAddress address = sparseVoxelOccupancyMipAddress(logical, state);
  return (words[address.wordIndex] & address.bitMask) != 0u;
}

void sortUniqueCells(std::vector<std::array<int32_t, 3>> &cells)
{
  std::sort(cells.begin(), cells.end());
  cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
}

} // namespace

bool makeSparseVoxelOccupancyLayout(const SparseVoxelOccupancySettings &settings, const SparseVoxelOccupancyLimits &limits, SparseVoxelOccupancyLayout &layout, std::string *error)
{
  if (error != nullptr)
    error->clear();
  layout = {};
  if (limits.maxTextureDimension3D == 0u || limits.maxStorageBufferBytes == 0u || settings.memoryCeilingBytes == 0u)
    return fail(error, "Sparse voxel occupancy limits must be nonzero");

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (settings.logicalResolution[axis] == 0u || settings.logicalResolution[axis] > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
      return fail(error, "Sparse voxel occupancy logical resolution is invalid");
    if (!std::isfinite(settings.worldExtent[axis]) || settings.worldExtent[axis] <= 0.0f)
      return fail(error, "Sparse voxel occupancy world extent is invalid");
  }

  const uint64_t wordsX64 = (static_cast<uint64_t>(settings.logicalResolution[0]) + 31u) / 32u;
  if (wordsX64 > limits.maxTextureDimension3D || settings.logicalResolution[1] > limits.maxTextureDimension3D || settings.logicalResolution[2] > limits.maxTextureDimension3D)
    return fail(error, "Sparse voxel occupancy texture dimensions exceed the device limit");

  uint64_t wordsXY = 0u;
  uint64_t wordCount = 0u;
  uint64_t residentBytes = 0u;
  uint64_t persistentBytes = 0u;
  if (!checkedMultiply(wordsX64, settings.logicalResolution[1], wordsXY) || !checkedMultiply(wordsXY, settings.logicalResolution[2], wordCount) || !checkedMultiply(wordCount, sizeof(uint32_t), residentBytes) ||
      !checkedMultiply(residentBytes, 2u, persistentBytes))
    return fail(error, "Sparse voxel occupancy allocation size overflow");
  if (residentBytes > limits.maxStorageBufferBytes)
    return fail(error, "Sparse voxel occupancy atomic mirror exceeds the storage-buffer limit");
  if (persistentBytes > settings.memoryCeilingBytes)
    return fail(error, "Sparse voxel occupancy persistent storage exceeds its memory ceiling");
  if (wordCount > std::numeric_limits<uint32_t>::max())
    return fail(error, "Sparse voxel occupancy word indices exceed 32-bit range");

  layout.logicalResolution = settings.logicalResolution;
  layout.wordResolution = {
    static_cast<uint32_t>(wordsX64),
    settings.logicalResolution[1],
    settings.logicalResolution[2],
  };
  layout.worldExtent = settings.worldExtent;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    layout.cellSize[axis] = settings.worldExtent[axis] / static_cast<float>(settings.logicalResolution[axis]);
    if (!std::isfinite(layout.cellSize[axis]) || layout.cellSize[axis] <= 0.0f)
      return fail(error, "Sparse voxel occupancy cell size is not representable");
  }
  layout.wordCount = wordCount;
  layout.residentTextureBytes = residentBytes;
  layout.persistentBytes = persistentBytes;
  return true;
}

bool makeSparseVoxelOccupancyPyramidLayout(
    const SparseVoxelOccupancySettings &settings,
    const SparseVoxelOccupancyLimits &limits,
    const SparseVoxelOccupancyLayout &baseLayout,
    SparseVoxelOccupancyPyramidLayout &layout,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  layout = {};
  const uint32_t maximumResolution = std::max({
    baseLayout.logicalResolution[0],
    baseLayout.logicalResolution[1],
    baseLayout.logicalResolution[2],
  });
  if (maximumResolution <= 1u)
    return true;

  uint64_t scale = 2u;
  uint32_t level = 1u;
  uint64_t wordOffset = 0u;
  while (true)
  {
    SparseVoxelOccupancyMipLayout mip{};
    mip.gpu.level = level;
    mip.gpu.scale = static_cast<uint32_t>(scale);
    mip.gpu.wordOffset = static_cast<uint32_t>(wordOffset);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const uint64_t padded = static_cast<uint64_t>(baseLayout.logicalResolution[axis]) + scale - 1u;
      const uint64_t resolution = ceilDivide(padded, scale);
      if (resolution == 0u || resolution > std::numeric_limits<uint32_t>::max())
        return fail(error, "Sparse voxel occupancy mip resolution exceeds 32-bit range");
      mip.gpu.logicalResolution[axis] = static_cast<uint32_t>(resolution);
    }
    mip.gpu.wordsX = static_cast<uint32_t>(ceilDivide(mip.gpu.logicalResolution[0], 32u));
    mip.gpu.wordResolution = {
      mip.gpu.wordsX,
      mip.gpu.logicalResolution[1],
      mip.gpu.logicalResolution[2],
    };

    uint64_t wordsXY = 0u;
    uint64_t wordCount = 0u;
    if (!checkedMultiply(mip.gpu.wordResolution[0], mip.gpu.wordResolution[1], wordsXY) || !checkedMultiply(wordsXY, mip.gpu.wordResolution[2], wordCount) || wordCount > std::numeric_limits<uint32_t>::max())
      return fail(error, "Sparse voxel occupancy mip word count overflow");
    uint64_t nextWordOffset = 0u;
    if (!checkedAdd(wordOffset, wordCount, nextWordOffset) || nextWordOffset > std::numeric_limits<uint32_t>::max())
      return fail(error, "Sparse voxel occupancy packed mip offsets exceed 32-bit range");
    mip.gpu.wordCount = static_cast<uint32_t>(wordCount);
    if (!checkedMultiply(wordCount, sizeof(uint32_t), mip.residentBytes))
      return fail(error, "Sparse voxel occupancy mip allocation size overflow");
    layout.levels.push_back(mip);
    wordOffset = nextWordOffset;

    if (scale >= maximumResolution)
      break;
    if (scale > std::numeric_limits<uint32_t>::max() / 2u)
      return fail(error, "Sparse voxel occupancy mip scale overflow");
    scale *= 2u;
    ++level;
  }

  layout.wordCount = wordOffset;
  if (!checkedMultiply(layout.wordCount, sizeof(uint32_t), layout.residentBytes) || !checkedMultiply(layout.residentBytes, 2u, layout.persistentBytes))
    return fail(error, "Sparse voxel occupancy pyramid allocation size overflow");
  uint64_t totalPersistentBytes = 0u;
  if (layout.residentBytes > limits.maxStorageBufferBytes || !checkedAdd(baseLayout.persistentBytes, layout.persistentBytes, totalPersistentBytes) || totalPersistentBytes > settings.memoryCeilingBytes)
    return fail(error, "Sparse voxel occupancy pyramid exceeds its buffer or memory ceiling");
  return true;
}

SparseVoxelOccupancyAddress sparseVoxelOccupancyAddress(const std::array<uint32_t, 3> &logicalCell, const SparseVoxelOccupancyVolumeGPU &state)
{
  SparseVoxelOccupancyAddress address{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (state.logicalResolution[axis] == 0u || logicalCell[axis] >= state.logicalResolution[axis])
      return {};
    address.physicalCell[axis] = static_cast<uint32_t>((static_cast<uint64_t>(logicalCell[axis]) + state.wrapOffset[axis]) % state.logicalResolution[axis]);
  }
  const uint32_t wordX = address.physicalCell[0] >> 5u;
  address.wordIndex = wordX + state.wordResolution[0] * (address.physicalCell[1] + state.wordResolution[1] * address.physicalCell[2]);
  address.bitMask = 1u << (address.physicalCell[0] & 31u);
  return address;
}

std::array<uint32_t, 3> sparseVoxelOccupancyLogicalCellFromPhysical(const std::array<uint32_t, 3> &physicalCell, const SparseVoxelOccupancyVolumeGPU &state)
{
  std::array<uint32_t, 3> logical{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    logical[axis] = positiveModulo(static_cast<int64_t>(physicalCell[axis]) - static_cast<int64_t>(state.wrapOffset[axis]), state.logicalResolution[axis]);
  return logical;
}

bool sparseVoxelOccupancyDirtyLogicalCell(const std::array<uint32_t, 3> &logicalCell, const SparseVoxelOccupancyVolumeGPU &state)
{
  if (state.fullRebuild != 0u)
    return true;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (newlyExposed1D(logicalCell[axis], state.scrollDelta[axis], state.logicalResolution[axis]))
      return true;
  return false;
}

std::vector<SparseVoxelAABB> sparseVoxelOccupancyQueryBoxes(const SparseVoxelOccupancyVolumeGPU &state)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (state.logicalResolution[axis] == 0u || !std::isfinite(state.cellSize[axis]) || state.cellSize[axis] <= 0.0f)
      return {};

  const auto makeBox = [&](const uint32_t slabAxis, const uint32_t slabFirst, const uint32_t slabEnd)
  {
    SparseVoxelAABB box{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const uint32_t logicalFirst = axis == slabAxis ? slabFirst : 0u;
      const uint32_t logicalEnd = axis == slabAxis ? slabEnd : state.logicalResolution[axis];
      const double cellSize = state.cellSize[axis];
      box.minimum[axis] = static_cast<float>((static_cast<double>(state.originCell[axis]) + logicalFirst) * cellSize);
      box.maximum[axis] = static_cast<float>((static_cast<double>(state.originCell[axis]) + logicalEnd) * cellSize);
    }
    return box;
  };

  if (state.fullRebuild != 0u)
    return {makeBox(0u, 0u, state.logicalResolution[0])};

  std::vector<SparseVoxelAABB> boxes;
  boxes.reserve(3u);
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t delta = state.scrollDelta[axis];
    if (delta == 0)
      continue;
    const uint64_t width64 = static_cast<uint64_t>(delta > 0 ? delta : -delta);
    const uint32_t width = static_cast<uint32_t>(std::min<uint64_t>(width64, state.logicalResolution[axis]));
    const uint32_t first = delta > 0 ? state.logicalResolution[axis] - width : 0u;
    const uint32_t end = delta > 0 ? state.logicalResolution[axis] : width;
    boxes.push_back(makeBox(axis, first, end));
  }
  return boxes;
}

std::vector<SparseVoxelAABB> sparseVoxelOccupancyQueryBoxes(
    const SparseVoxelOccupancyVolumeGPU &state,
    const std::vector<SparseVoxelAABB> &invalidatedWorldBounds)
{
  std::vector<SparseVoxelAABB> boxes = sparseVoxelOccupancyQueryBoxes(state);
  SparseVoxelOccupancyVolumeGPU fullState = state;
  fullState.fullRebuild = 1u;
  const std::vector<SparseVoxelAABB> fullBoxes = sparseVoxelOccupancyQueryBoxes(fullState);
  if (fullBoxes.empty())
    return {};

  for (const SparseVoxelAABB &bounds : invalidatedWorldBounds)
  {
    if (!finiteAABB(bounds))
      return fullBoxes;
    SparseVoxelAABB clipped{};
    bool intersects = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const double minimum = std::nextafter(
          static_cast<double>(bounds.minimum[axis]),
          -std::numeric_limits<double>::infinity());
      const double maximum = std::nextafter(
          static_cast<double>(bounds.maximum[axis]),
          std::numeric_limits<double>::infinity());
      const int64_t volumeFirst = state.originCell[axis];
      const int64_t volumeLast =
          volumeFirst + state.logicalResolution[axis] - 1u;
      const double firstCell =
          std::floor(minimum / state.cellSize[axis]);
      const double lastCell =
          std::floor(maximum / state.cellSize[axis]);
      const bool axisIntersects =
          firstCell <= static_cast<double>(volumeLast) &&
          lastCell >= static_cast<double>(volumeFirst);
      intersects &= axisIntersects;
      const int64_t clippedFirst = firstCell <= volumeFirst
          ? volumeFirst
          : (firstCell >= volumeLast
                 ? volumeLast
                 : static_cast<int64_t>(firstCell));
      const int64_t clippedLast = lastCell >= volumeLast
          ? volumeLast
          : (lastCell <= volumeFirst
                 ? volumeFirst
                 : static_cast<int64_t>(lastCell));
      // Upload cell-aligned bounds. The GPU's strict half-open box test then
      // classifies exactly the same conservatively covered cells as the
      // scalar nextafter/floor rule, including zero-volume invalidations.
      clipped.minimum[axis] =
          static_cast<float>(clippedFirst) * state.cellSize[axis];
      clipped.maximum[axis] =
          static_cast<float>(clippedLast + 1) *
          state.cellSize[axis];
    }
    if (intersects)
      boxes.push_back(clipped);
  }
  if (boxes.size() <= 3u)
    return boxes;

  SparseVoxelAABB merged = boxes.front();
  for (uint32_t boxIndex = 1u; boxIndex < boxes.size(); ++boxIndex)
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      merged.minimum[axis] = std::min(merged.minimum[axis], boxes[boxIndex].minimum[axis]);
      merged.maximum[axis] = std::max(merged.maximum[axis], boxes[boxIndex].maximum[axis]);
    }
  return {merged};
}

uint32_t sparseVoxelOccupancyTailMask(const uint32_t logicalResolutionX)
{
  const uint32_t tailBits = logicalResolutionX & 31u;
  return tailBits == 0u ? std::numeric_limits<uint32_t>::max() : (uint32_t{1} << tailBits) - 1u;
}

SparseVoxelOccupancyAddress sparseVoxelOccupancyMipAddress(const std::array<uint32_t, 3> &logicalCell, const SparseVoxelOccupancyMipLevelGPU &state)
{
  SparseVoxelOccupancyAddress address{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (state.logicalResolution[axis] == 0u || logicalCell[axis] >= state.logicalResolution[axis])
      return {};
    address.physicalCell[axis] = static_cast<uint32_t>((static_cast<uint64_t>(logicalCell[axis]) + state.wrapOffset[axis]) % state.logicalResolution[axis]);
  }
  const uint32_t wordX = address.physicalCell[0] >> 5u;
  address.wordIndex = wordX + state.wordResolution[0] * (address.physicalCell[1] + state.wordResolution[1] * address.physicalCell[2]);
  address.bitMask = 1u << (address.physicalCell[0] & 31u);
  return address;
}

std::array<uint32_t, 3> sparseVoxelOccupancyMipLogicalCellFromPhysical(const std::array<uint32_t, 3> &physicalCell, const SparseVoxelOccupancyMipLevelGPU &state)
{
  std::array<uint32_t, 3> logical{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    logical[axis] = positiveModulo(static_cast<int64_t>(physicalCell[axis]) - static_cast<int64_t>(state.wrapOffset[axis]), state.logicalResolution[axis]);
  return logical;
}

bool prepareSparseVoxelOccupancyUpdate(
    const SparseVoxelOccupancyLayout &layout,
    const SparseVoxelOccupancyVolumeGPU *committedState,
    const std::array<float, 3> &cameraWorldPosition,
    const uint64_t sceneRevision,
    SparseVoxelOccupancyUpdate &update,
    std::string *error)
{
  return prepareSparseVoxelOccupancyUpdate(layout, committedState, cameraWorldPosition, sceneRevision, {}, update, error);
}

bool prepareSparseVoxelOccupancyUpdate(
    const SparseVoxelOccupancyLayout &layout,
    const SparseVoxelOccupancyVolumeGPU *committedState,
    const std::array<float, 3> &cameraWorldPosition,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
    SparseVoxelOccupancyUpdate &update,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  update = {};
  if (layout.wordCount == 0u || sceneRevision > std::numeric_limits<uint32_t>::max())
    return fail(error, "Sparse voxel occupancy layout or scene revision is invalid");

  SparseVoxelOccupancyVolumeGPU pending{};
  pending.logicalResolution = layout.logicalResolution;
  pending.wordsX = layout.wordResolution[0];
  pending.wordResolution = layout.wordResolution;
  pending.cellSize = layout.cellSize;
  pending.worldExtent = layout.worldExtent;
  pending.sceneRevision = static_cast<uint32_t>(sceneRevision);
  if (!cameraDecomposition(layout, cameraWorldPosition, pending.cameraCell, pending.cameraInnerCell, pending.originCell, error))
    return false;

  const bool hasCommitted = committedState != nullptr && committedState->valid != 0u && sameLayout(layout, *committedState);
  for (const SparseVoxelAABB &bounds : invalidatedWorldBounds)
    if (!finiteAABB(bounds))
      return fail(error, "Sparse voxel occupancy invalidation bounds are invalid");

  // Camera motion within the current base cell changes only the diagnostic
  // sub-cell coordinate. Preserve the complete toroidal mapping and avoid
  // scanning every physical bit to rediscover an empty dirty set.
  if (hasCommitted &&
      committedState->sceneRevision == pending.sceneRevision &&
      invalidatedWorldBounds.empty() &&
      committedState->originCell == pending.originCell)
  {
    update.pendingState = *committedState;
    update.pendingState.cameraCell = pending.cameraCell;
    update.pendingState.cameraInnerCell = pending.cameraInnerCell;
    update.pendingState.scrollDelta = {0, 0, 0};
    update.pendingState.dirtyWordCount = 0u;
    update.pendingState.fullRebuild = 0u;
    return true;
  }

  // A changed scene revision is incremental only when the caller supplies the
  // complete old/new world-space bounds affected by that revision.
  const bool fullRebuildForScene = hasCommitted && committedState->sceneRevision != pending.sceneRevision && invalidatedWorldBounds.empty();
  bool fullRebuild = !hasCommitted || fullRebuildForScene;
  if (hasCommitted)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t delta = static_cast<int64_t>(pending.originCell[axis]) - static_cast<int64_t>(committedState->originCell[axis]);
      if (delta < std::numeric_limits<int32_t>::min() || delta > std::numeric_limits<int32_t>::max() || std::abs(delta) >= static_cast<int64_t>(layout.logicalResolution[axis]))
      {
        fullRebuild = true;
        pending.scrollDelta[axis] = 0;
      }
      else
      {
        pending.scrollDelta[axis] = static_cast<int32_t>(delta);
      }
      pending.wrapOffset[axis] = positiveModulo(static_cast<int64_t>(committedState->wrapOffset[axis]) + delta, layout.logicalResolution[axis]);
    }
    pending.contentGeneration = committedState->contentGeneration;
  }
  pending.fullRebuild = fullRebuild ? 1u : 0u;
  if (!hasCommitted)
  {
    pending.wrapOffset = {0u, 0u, 0u};
    pending.scrollDelta = {0, 0, 0};
  }

  std::vector<uint32_t> dirtyMasks(static_cast<size_t>(layout.wordCount), 0u);
  if (pending.fullRebuild != 0u)
  {
    for (uint32_t wordIndex = 0u;
         wordIndex < static_cast<uint32_t>(layout.wordCount);
         ++wordIndex)
    {
      const uint32_t wordX =
          wordIndex % layout.wordResolution[0];
      dirtyMasks[wordIndex] =
          wordX + 1u == layout.wordResolution[0]
              ? sparseVoxelOccupancyTailMask(
                    layout.logicalResolution[0])
              : std::numeric_limits<uint32_t>::max();
    }
  }
  else if (pending.scrollDelta != std::array<int32_t, 3>{0, 0, 0})
  {
    for (uint32_t wordIndex = 0u;
         wordIndex < static_cast<uint32_t>(layout.wordCount);
         ++wordIndex)
    {
      const uint32_t wordX =
          wordIndex % layout.wordResolution[0];
      const uint32_t yz =
          wordIndex / layout.wordResolution[0];
      const uint32_t physicalY =
          yz % layout.wordResolution[1];
      const uint32_t physicalZ =
          yz / layout.wordResolution[1];
      uint32_t dirtyMask = 0u;
      for (uint32_t bit = 0u; bit < 32u; ++bit)
      {
        const uint32_t physicalX = wordX * 32u + bit;
        if (physicalX >= layout.logicalResolution[0])
          break;
        const auto logical =
            sparseVoxelOccupancyLogicalCellFromPhysical(
                {physicalX, physicalY, physicalZ}, pending);
        if (sparseVoxelOccupancyDirtyLogicalCell(logical, pending))
          dirtyMask |= uint32_t{1} << bit;
      }
      dirtyMasks[wordIndex] = dirtyMask;
    }
  }

  if (pending.fullRebuild == 0u)
    for (const SparseVoxelAABB &bounds : invalidatedWorldBounds)
    {
      std::array<int64_t, 3> firstGlobal{};
      std::array<int64_t, 3> lastGlobal{};
      bool intersects = true;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const double minimum = std::nextafter(static_cast<double>(bounds.minimum[axis]), -std::numeric_limits<double>::infinity());
        const double maximum = std::nextafter(static_cast<double>(bounds.maximum[axis]), std::numeric_limits<double>::infinity());
        const int64_t volumeFirst = pending.originCell[axis];
        const int64_t volumeLast = volumeFirst + layout.logicalResolution[axis] - 1u;
        const double firstCell = std::floor(minimum / layout.cellSize[axis]);
        const double lastCell = std::floor(maximum / layout.cellSize[axis]);
        if (firstCell > static_cast<double>(volumeLast) || lastCell < static_cast<double>(volumeFirst))
        {
          intersects = false;
          break;
        }
        firstGlobal[axis] = firstCell <= static_cast<double>(volumeFirst) ? volumeFirst : static_cast<int64_t>(firstCell);
        lastGlobal[axis] = lastCell >= static_cast<double>(volumeLast) ? volumeLast : static_cast<int64_t>(lastCell);
      }
      if (!intersects)
        continue;

      for (int64_t z = firstGlobal[2]; z <= lastGlobal[2]; ++z)
        for (int64_t y = firstGlobal[1]; y <= lastGlobal[1]; ++y)
          for (int64_t x = firstGlobal[0]; x <= lastGlobal[0]; ++x)
          {
            const std::array<uint32_t, 3> logical{
              static_cast<uint32_t>(x - pending.originCell[0]),
              static_cast<uint32_t>(y - pending.originCell[1]),
              static_cast<uint32_t>(z - pending.originCell[2]),
            };
            const SparseVoxelOccupancyAddress address = sparseVoxelOccupancyAddress(logical, pending);
            dirtyMasks[address.wordIndex] |= address.bitMask;
          }
    }

  update.dirtyWords.reserve(static_cast<size_t>(layout.wordCount));
  for (uint32_t wordIndex = 0u; wordIndex < dirtyMasks.size(); ++wordIndex)
  {
    const uint32_t dirtyMask = dirtyMasks[wordIndex];
    if (dirtyMask != 0u)
      update.dirtyWords.push_back({wordIndex, dirtyMask});
  }
  pending.dirtyWordCount = static_cast<uint32_t>(update.dirtyWords.size());
  update.pendingState = pending;
  return true;
}

std::vector<SparseVoxelAABB> collectSparseVoxelOccupancyLeafBounds(const SparseVoxelSceneManager &sceneManager)
{
  std::vector<SparseVoxelAABB> bounds;
  const SparseVoxelScene &scene = sceneManager.scene();
  for (const SparseVoxelRuntimeInstance &runtimeInstance : sceneManager.runtimeInstances())
  {
    if (runtimeInstance.gpu.blasIndex >= scene.models.size())
      continue;
    const SparseVoxelBLAS &model = scene.models[runtimeInstance.gpu.blasIndex];
    for (uint32_t nodeIndex = 0u; nodeIndex < model.hierarchyNodes.size(); ++nodeIndex)
    {
      if (model.hierarchyNodes[nodeIndex].childCount != 0u)
        continue;
      const SparseVoxelCenterLengthGPU &source = model.centersLengths[nodeIndex];
      const float halfLength = source.length * 0.5f;
      const SparseVoxelAABB world = transformCubeBounds(
          runtimeInstance.gpu.modelToWorld,
          source.center,
          halfLength);
      if (finiteAABB(world))
        bounds.push_back(world);
    }
  }
  return bounds;
}

std::vector<SparseVoxelAABB> collectSparseVoxelOccupancyLeafBounds(
    const SparseVoxelSceneManager &sceneManager,
    const SparseVoxelFileRuntime &fileRuntime)
{
  std::vector<SparseVoxelAABB> bounds;
  const SparseVoxelPageResidency &residency = fileRuntime.residency();
  const auto &nodes = residency.physicalNodes();
  const auto &metadata = residency.physicalMetadata();
  const auto &centersLengths = residency.physicalCentersLengths();
  const auto &fileMetadata = fileRuntime.metadata();
  if (nodes.size() != metadata.size() || nodes.size() != centersLengths.size())
    return bounds;

  std::vector<std::vector<const SparseVoxelRuntimeInstance *>> modelInstances(
      fileMetadata.models.size());
  for (const SparseVoxelRuntimeInstance &instance :
       sceneManager.runtimeInstances())
    if (instance.gpu.blasIndex < modelInstances.size())
      modelInstances[instance.gpu.blasIndex].push_back(&instance);

  bounds.reserve(residency.residentNodeCount());
  for (uint32_t physical = 0u; physical < nodes.size(); ++physical)
  {
    const SparseVoxelResidentNodeMetadataGPU &resident = metadata[physical];
    if (resident.logicalNodeIndex == kInvalidSparseVoxelIndex ||
        resident.owningPageIndex >= fileMetadata.pages.size() ||
        nodes[physical].childCount != 0u)
      continue;
    const uint32_t modelIndex =
        fileMetadata.pages[resident.owningPageIndex].modelIndex;
    if (modelIndex >= modelInstances.size())
      continue;
    const SparseVoxelCenterLengthGPU &source = centersLengths[physical];
    if (!std::isfinite(source.length) || source.length <= 0.0f)
      continue;
    const float halfLength = source.length * 0.5f;
    for (const SparseVoxelRuntimeInstance *instance :
         modelInstances[modelIndex])
    {
      const SparseVoxelAABB world = transformCubeBounds(
          instance->gpu.modelToWorld,
          source.center,
          halfLength);
      if (finiteAABB(world))
        bounds.push_back(world);
    }
  }
  return bounds;
}

bool SparseVoxelOccupancyVolume::configure(const SparseVoxelOccupancySettings &settings, const SparseVoxelOccupancyLimits &limits, std::string *error)
{
  SparseVoxelOccupancyLayout candidate{};
  if (!makeSparseVoxelOccupancyLayout(settings, limits, candidate, error))
    return false;
  SparseVoxelOccupancyPyramidLayout candidatePyramid{};
  if (!makeSparseVoxelOccupancyPyramidLayout(settings, limits, candidate, candidatePyramid, error))
    return false;
  std::vector<SparseVoxelOccupancyMipLevel> candidateMipLevels;
  candidateMipLevels.reserve(candidatePyramid.levels.size());
  for (const SparseVoxelOccupancyMipLayout &mipLayout : candidatePyramid.levels)
  {
    SparseVoxelOccupancyMipLevel level{};
    level.state = mipLayout.gpu;
    if (settings.maintainCPUReference)
      level.words.assign(level.state.wordCount, 0u);
    candidateMipLevels.push_back(std::move(level));
  }
  layout_ = candidate;
  pyramidLayout_ = std::move(candidatePyramid);
  state_ = {};
  if (settings.maintainCPUReference)
    words_.assign(static_cast<size_t>(layout_.wordCount), 0u);
  else
    words_.clear();
  lastDirtyWords_.clear();
  mipLevels_ = std::move(candidateMipLevels);
  maintainCPUReference_ = settings.maintainCPUReference;
  configured_ = true;
  return true;
}

bool SparseVoxelOccupancyVolume::update(const std::array<float, 3> &cameraWorldPosition, const uint64_t sceneRevision, const std::vector<SparseVoxelAABB> &sourceLeafBounds, std::string *error)
{
  return update(cameraWorldPosition, sceneRevision, sourceLeafBounds, {}, error);
}

bool SparseVoxelOccupancyVolume::updateGPU(
    const std::array<float, 3> &cameraWorldPosition,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
    std::string *error)
{
  if (!configured_)
    return fail(error, "Sparse voxel occupancy volume is not configured");
  if (error != nullptr)
    error->clear();
  if (sceneRevision > std::numeric_limits<uint32_t>::max())
    return fail(error, "Sparse voxel occupancy scene revision is invalid");
  for (const SparseVoxelAABB &bounds : invalidatedWorldBounds)
    if (!finiteAABB(bounds))
      return fail(error, "Sparse voxel occupancy invalidation bounds are invalid");

  SparseVoxelOccupancyVolumeGPU pending{};
  pending.logicalResolution = layout_.logicalResolution;
  pending.wordsX = layout_.wordResolution[0];
  pending.wordResolution = layout_.wordResolution;
  pending.cellSize = layout_.cellSize;
  pending.worldExtent = layout_.worldExtent;
  pending.sceneRevision = static_cast<uint32_t>(sceneRevision);
  if (!cameraDecomposition(
          layout_,
          cameraWorldPosition,
          pending.cameraCell,
          pending.cameraInnerCell,
          pending.originCell,
          error))
    return false;

  const bool hasCommitted =
      state_.valid != 0u && sameLayout(layout_, state_);
  const bool sameOrigin =
      hasCommitted && state_.originCell == pending.originCell;
  const bool sameScene =
      hasCommitted && state_.sceneRevision == pending.sceneRevision;
  const bool contentDirty =
      !hasCommitted || !sameOrigin || !sameScene ||
      !invalidatedWorldBounds.empty();
  if (!contentDirty)
  {
    state_.cameraCell = pending.cameraCell;
    state_.cameraInnerCell = pending.cameraInnerCell;
    state_.scrollDelta = {0, 0, 0};
    state_.dirtyWordCount = 0u;
    state_.fullRebuild = 0u;
    lastDirtyWords_.clear();
    for (SparseVoxelOccupancyMipLevel &level : mipLevels_)
      level.lastDirtyWords.clear();
    return true;
  }

  bool fullRebuild = !hasCommitted ||
      (hasCommitted && !sameScene && invalidatedWorldBounds.empty());
  if (hasCommitted)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t delta =
          static_cast<int64_t>(pending.originCell[axis]) -
          static_cast<int64_t>(state_.originCell[axis]);
      if (delta < std::numeric_limits<int32_t>::min() ||
          delta > std::numeric_limits<int32_t>::max() ||
          std::abs(delta) >=
              static_cast<int64_t>(layout_.logicalResolution[axis]))
      {
        fullRebuild = true;
        pending.scrollDelta[axis] = 0;
      }
      else
      {
        pending.scrollDelta[axis] = static_cast<int32_t>(delta);
      }
      pending.wrapOffset[axis] = positiveModulo(
          static_cast<int64_t>(state_.wrapOffset[axis]) + delta,
          layout_.logicalResolution[axis]);
    }
    if (state_.contentGeneration ==
        std::numeric_limits<uint32_t>::max())
      return fail(
          error,
          "Sparse voxel occupancy content generation overflow");
    pending.contentGeneration = state_.contentGeneration + 1u;
  }
  else
  {
    pending.wrapOffset = {0u, 0u, 0u};
    pending.scrollDelta = {0, 0, 0};
    pending.contentGeneration = 1u;
  }
  pending.fullRebuild = fullRebuild ? 1u : 0u;
  pending.dirtyWordCount = 1u;
  pending.valid = 1u;

  for (uint32_t mipIndex = 0u;
       mipIndex < mipLevels_.size();
       ++mipIndex)
  {
    SparseVoxelOccupancyMipLevelGPU descriptor =
        pyramidLayout_.levels[mipIndex].gpu;
    const SparseVoxelOccupancyMipLevelGPU previous =
        mipLevels_[mipIndex].state;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t firstBase = pending.originCell[axis];
      const int64_t lastBase =
          firstBase +
          static_cast<int64_t>(pending.logicalResolution[axis]) - 1;
      const int64_t firstParent =
          floorDivide(firstBase, descriptor.scale);
      const int64_t lastParent =
          floorDivide(lastBase, descriptor.scale);
      if (firstParent < std::numeric_limits<int32_t>::min() ||
          firstParent > std::numeric_limits<int32_t>::max())
        return fail(
            error,
            "Sparse voxel occupancy mip origin exceeds signed 32-bit range");
      descriptor.originCell[axis] =
          static_cast<int32_t>(firstParent);
      descriptor.activeResolution[axis] =
          static_cast<uint32_t>(lastParent - firstParent + 1);
      if (descriptor.activeResolution[axis] >
          descriptor.logicalResolution[axis])
        return fail(
            error,
            "Sparse voxel occupancy active mip range exceeds its allocation");
      if (!fullRebuild && hasCommitted)
      {
        const int64_t delta =
            firstParent - previous.originCell[axis];
        descriptor.wrapOffset[axis] = positiveModulo(
            static_cast<int64_t>(previous.wrapOffset[axis]) + delta,
            descriptor.logicalResolution[axis]);
      }
      else
      {
        descriptor.wrapOffset[axis] = 0u;
      }
    }
    mipLevels_[mipIndex].state = descriptor;
    mipLevels_[mipIndex].lastDirtyWords.clear();
  }

  state_ = pending;
  lastDirtyWords_.clear();
  return true;
}

bool SparseVoxelOccupancyVolume::update(
    const std::array<float, 3> &cameraWorldPosition,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelAABB> &sourceLeafBounds,
    const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
    std::string *error)
{
  if (!configured_)
    return fail(error, "Sparse voxel occupancy volume is not configured");
  if (!maintainCPUReference_)
    return fail(
        error,
        "Sparse voxel occupancy scalar update requires a CPU reference");
  SparseVoxelOccupancyUpdate updatePlan{};
  const SparseVoxelOccupancyVolumeGPU *committed = state_.valid != 0u ? &state_ : nullptr;
  if (!prepareSparseVoxelOccupancyUpdate(layout_, committed, cameraWorldPosition, sceneRevision, invalidatedWorldBounds, updatePlan, error))
    return false;

  // The common static/sub-cell case is metadata-only. In particular, do not
  // copy the complete base and mip CPU mirrors or revisit every source leaf.
  if (state_.valid != 0u &&
      state_.sceneRevision == updatePlan.pendingState.sceneRevision &&
      invalidatedWorldBounds.empty() &&
      updatePlan.dirtyWords.empty())
  {
    state_ = updatePlan.pendingState;
    lastDirtyWords_.clear();
    for (SparseVoxelOccupancyMipLevel &level : mipLevels_)
      level.lastDirtyWords.clear();
    return true;
  }

  if (maintainCPUReference_)
    for (const SparseVoxelAABB &bounds : sourceLeafBounds)
      if (!finiteAABB(bounds))
        return fail(error, "Sparse voxel occupancy source bounds are invalid");

  std::vector<uint32_t> candidateWords;
  std::vector<uint32_t> dirtyMasks;
  if (maintainCPUReference_)
  {
    candidateWords = words_;
    dirtyMasks.assign(static_cast<size_t>(layout_.wordCount), 0u);
    for (const SparseVoxelOccupancyDirtyWordGPU dirty :
         updatePlan.dirtyWords)
    {
      candidateWords[dirty.wordIndex] &= ~dirty.bitMask;
      dirtyMasks[dirty.wordIndex] = dirty.bitMask;
    }
  }

  if (maintainCPUReference_)
    for (const SparseVoxelAABB &bounds : sourceLeafBounds)
  {
    std::array<int64_t, 3> firstGlobal{};
    std::array<int64_t, 3> lastGlobal{};
    bool intersects = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      // Bias outwards by one representable value. This intentionally favors a
      // conservative occupied result at transformed coarse-cell boundaries.
      const double minimum = std::nextafter(static_cast<double>(bounds.minimum[axis]), -std::numeric_limits<double>::infinity());
      const double maximum = std::nextafter(static_cast<double>(bounds.maximum[axis]), std::numeric_limits<double>::infinity());
      const int64_t volumeFirst = updatePlan.pendingState.originCell[axis];
      const int64_t volumeLast = volumeFirst + layout_.logicalResolution[axis] - 1u;
      const double firstCell = std::floor(minimum / layout_.cellSize[axis]);
      const double lastCell = std::floor(maximum / layout_.cellSize[axis]);
      if (firstCell > static_cast<double>(volumeLast) || lastCell < static_cast<double>(volumeFirst))
      {
        intersects = false;
        continue;
      }
      firstGlobal[axis] = firstCell <= static_cast<double>(volumeFirst) ? volumeFirst : static_cast<int64_t>(firstCell);
      lastGlobal[axis] = lastCell >= static_cast<double>(volumeLast) ? volumeLast : static_cast<int64_t>(lastCell);
      intersects &= firstGlobal[axis] <= lastGlobal[axis];
    }
    if (!intersects)
      continue;

    for (int64_t z = firstGlobal[2]; z <= lastGlobal[2]; ++z)
      for (int64_t y = firstGlobal[1]; y <= lastGlobal[1]; ++y)
        for (int64_t x = firstGlobal[0]; x <= lastGlobal[0]; ++x)
        {
          const std::array<uint32_t, 3> logical{
            static_cast<uint32_t>(x - updatePlan.pendingState.originCell[0]),
            static_cast<uint32_t>(y - updatePlan.pendingState.originCell[1]),
            static_cast<uint32_t>(z - updatePlan.pendingState.originCell[2]),
          };
          const SparseVoxelOccupancyAddress address = sparseVoxelOccupancyAddress(logical, updatePlan.pendingState);
          if ((dirtyMasks[address.wordIndex] & address.bitMask) == 0u)
            continue;
          candidateWords[address.wordIndex] |= address.bitMask;
        }
  }

  SparseVoxelOccupancyVolumeGPU candidateState = updatePlan.pendingState;
  candidateState.valid = 1u;
  candidateState.overflowStage = static_cast<uint32_t>(SparseVoxelOverflowStage::None);
  if (!updatePlan.dirtyWords.empty())
  {
    if (state_.valid != 0u && state_.contentGeneration == std::numeric_limits<uint32_t>::max())
      return fail(error, "Sparse voxel occupancy content generation overflow");
    candidateState.contentGeneration = state_.valid != 0u ? state_.contentGeneration + 1u : 1u;
  }

  std::vector<SparseVoxelOccupancyMipLevel> candidateMipLevels = mipLevels_;
  if (!updatePlan.dirtyWords.empty())
  {
    uint64_t dirtyBaseCellCount = 0u;
    for (const SparseVoxelOccupancyDirtyWordGPU dirty :
         updatePlan.dirtyWords)
      dirtyBaseCellCount += bitCount(dirty.bitMask);
    const uint64_t baseCellCount =
        uint64_t{candidateState.logicalResolution[0]} *
        candidateState.logicalResolution[1] *
        candidateState.logicalResolution[2];
    const bool dirtyEntireHierarchy =
        dirtyBaseCellCount == baseCellCount;
    std::vector<std::array<int32_t, 3>> touchedChildCells;
    if (!dirtyEntireHierarchy)
    {
      touchedChildCells.reserve(updatePlan.dirtyWords.size() * 8u);
      for (const SparseVoxelOccupancyDirtyWordGPU dirty :
           updatePlan.dirtyWords)
      {
        const uint32_t wordX =
            dirty.wordIndex % candidateState.wordResolution[0];
        const uint32_t yz =
            dirty.wordIndex / candidateState.wordResolution[0];
        const uint32_t physicalY =
            yz % candidateState.wordResolution[1];
        const uint32_t physicalZ =
            yz / candidateState.wordResolution[1];
        uint32_t bits = dirty.bitMask;
        for (uint32_t bit = 0u; bit < 32u; ++bit)
        {
          const uint32_t bitMask = uint32_t{1} << bit;
          if ((bits & bitMask) == 0u)
            continue;
          bits &= ~bitMask;
          const uint32_t physicalX = wordX * 32u + bit;
          const std::array<uint32_t, 3> physical{
            physicalX, physicalY, physicalZ};
          const auto pendingLogical =
              sparseVoxelOccupancyLogicalCellFromPhysical(
                  physical, candidateState);
          touchedChildCells.push_back({
            static_cast<int32_t>(
                static_cast<int64_t>(candidateState.originCell[0]) +
                pendingLogical[0]),
            static_cast<int32_t>(
                static_cast<int64_t>(candidateState.originCell[1]) +
                pendingLogical[1]),
            static_cast<int32_t>(
                static_cast<int64_t>(candidateState.originCell[2]) +
                pendingLogical[2]),
          });
          if (state_.valid != 0u)
          {
            const auto committedLogical =
                sparseVoxelOccupancyLogicalCellFromPhysical(
                    physical, state_);
            touchedChildCells.push_back({
              static_cast<int32_t>(
                  static_cast<int64_t>(state_.originCell[0]) +
                  committedLogical[0]),
              static_cast<int32_t>(
                  static_cast<int64_t>(state_.originCell[1]) +
                  committedLogical[1]),
              static_cast<int32_t>(
                  static_cast<int64_t>(state_.originCell[2]) +
                  committedLogical[2]),
            });
          }
        }
      }
      sortUniqueCells(touchedChildCells);
    }

    for (uint32_t mipIndex = 0u; mipIndex < candidateMipLevels.size(); ++mipIndex)
    {
      SparseVoxelOccupancyMipLevel &candidateLevel = candidateMipLevels[mipIndex];
      const SparseVoxelOccupancyMipLevel &committedLevel = mipLevels_[mipIndex];
      SparseVoxelOccupancyMipLevelGPU pendingLevel = pyramidLayout_.levels[mipIndex].gpu;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const int64_t firstBase = candidateState.originCell[axis];
        const int64_t lastBase = firstBase + static_cast<int64_t>(candidateState.logicalResolution[axis]) - 1;
        const int64_t firstParent = floorDivide(firstBase, pendingLevel.scale);
        const int64_t lastParent = floorDivide(lastBase, pendingLevel.scale);
        if (firstParent < std::numeric_limits<int32_t>::min() || firstParent > std::numeric_limits<int32_t>::max())
          return fail(error, "Sparse voxel occupancy mip origin exceeds signed 32-bit range");
        pendingLevel.originCell[axis] = static_cast<int32_t>(firstParent);
        pendingLevel.activeResolution[axis] = static_cast<uint32_t>(lastParent - firstParent + 1);
        if (pendingLevel.activeResolution[axis] > pendingLevel.logicalResolution[axis])
          return fail(error, "Sparse voxel occupancy active mip range exceeds its allocation");

        if (candidateState.fullRebuild == 0u && state_.valid != 0u)
        {
          const int64_t delta = firstParent - committedLevel.state.originCell[axis];
          pendingLevel.wrapOffset[axis] = positiveModulo(static_cast<int64_t>(committedLevel.state.wrapOffset[axis]) + delta, pendingLevel.logicalResolution[axis]);
        }
        else
        {
          pendingLevel.wrapOffset[axis] = 0u;
        }
      }
      candidateLevel.state = pendingLevel;

      std::vector<std::array<int32_t, 3>> touchedParentCells;
      if (!dirtyEntireHierarchy &&
          candidateState.fullRebuild == 0u)
      {
        touchedParentCells.reserve(touchedChildCells.size());
        for (const auto &child : touchedChildCells)
          touchedParentCells.push_back({
            static_cast<int32_t>(floorDivide(child[0], 2)),
            static_cast<int32_t>(floorDivide(child[1], 2)),
            static_cast<int32_t>(floorDivide(child[2], 2)),
          });
        sortUniqueCells(touchedParentCells);
      }

      std::vector<uint32_t> dirtyMasks(candidateLevel.state.wordCount, 0u);
      if (candidateState.fullRebuild != 0u ||
          dirtyEntireHierarchy)
      {
        for (uint32_t wordIndex = 0u; wordIndex < candidateLevel.state.wordCount; ++wordIndex)
        {
          const uint32_t wordX = wordIndex % candidateLevel.state.wordResolution[0];
          dirtyMasks[wordIndex] = wordX + 1u == candidateLevel.state.wordResolution[0] ? sparseVoxelOccupancyTailMask(candidateLevel.state.logicalResolution[0]) : std::numeric_limits<uint32_t>::max();
        }
      }
      else
      {
        for (const auto &parent : touchedParentCells)
        {
          if (mipContainsGlobalCell(committedLevel.state, parent))
          {
            const std::array<uint32_t, 3> logical{
              static_cast<uint32_t>(parent[0] - committedLevel.state.originCell[0]),
              static_cast<uint32_t>(parent[1] - committedLevel.state.originCell[1]),
              static_cast<uint32_t>(parent[2] - committedLevel.state.originCell[2]),
            };
            const auto address = sparseVoxelOccupancyMipAddress(logical, committedLevel.state);
            dirtyMasks[address.wordIndex] |= address.bitMask;
          }
          if (mipContainsGlobalCell(candidateLevel.state, parent))
          {
            const std::array<uint32_t, 3> logical{
              static_cast<uint32_t>(parent[0] - candidateLevel.state.originCell[0]),
              static_cast<uint32_t>(parent[1] - candidateLevel.state.originCell[1]),
              static_cast<uint32_t>(parent[2] - candidateLevel.state.originCell[2]),
            };
            const auto address = sparseVoxelOccupancyMipAddress(logical, candidateLevel.state);
            dirtyMasks[address.wordIndex] |= address.bitMask;
          }
        }
      }

      candidateLevel.lastDirtyWords.clear();
      for (uint32_t wordIndex = 0u; wordIndex < dirtyMasks.size(); ++wordIndex)
        if (dirtyMasks[wordIndex] != 0u)
        {
          candidateLevel.lastDirtyWords.push_back({wordIndex, dirtyMasks[wordIndex]});
          if (maintainCPUReference_)
            candidateLevel.words[wordIndex] &= ~dirtyMasks[wordIndex];
        }

      if (maintainCPUReference_)
      {
        const auto childOccupied =
            [&](const std::array<int32_t, 3> &child)
        {
          return mipIndex == 0u
              ? occupiedBaseCell(candidateState, candidateWords, child)
              : occupiedMipCell(
                    candidateMipLevels[mipIndex - 1u].state,
                    candidateMipLevels[mipIndex - 1u].words,
                    child);
        };
        const auto recomputeParent =
            [&](const std::array<int32_t, 3> &parent)
        {
          if (!mipContainsGlobalCell(candidateLevel.state, parent))
            return;
          bool occupied = false;
          for (uint32_t childIndex = 0u; childIndex < 8u; ++childIndex)
          {
            const std::array<int32_t, 3> child{
              static_cast<int32_t>(
                  static_cast<int64_t>(parent[0]) * 2 +
                  ((childIndex & 1u) != 0u ? 1 : 0)),
              static_cast<int32_t>(
                  static_cast<int64_t>(parent[1]) * 2 +
                  ((childIndex & 2u) != 0u ? 1 : 0)),
              static_cast<int32_t>(
                  static_cast<int64_t>(parent[2]) * 2 +
                  ((childIndex & 4u) != 0u ? 1 : 0)),
            };
            occupied |= childOccupied(child);
          }
          if (!occupied)
            return;
          const std::array<uint32_t, 3> logical{
            static_cast<uint32_t>(
                parent[0] - candidateLevel.state.originCell[0]),
            static_cast<uint32_t>(
                parent[1] - candidateLevel.state.originCell[1]),
            static_cast<uint32_t>(
                parent[2] - candidateLevel.state.originCell[2]),
          };
          const auto address = sparseVoxelOccupancyMipAddress(
              logical, candidateLevel.state);
          candidateLevel.words[address.wordIndex] |= address.bitMask;
        };

        if (candidateState.fullRebuild != 0u ||
            dirtyEntireHierarchy)
        {
          for (uint32_t z = 0u;
               z < candidateLevel.state.activeResolution[2]; ++z)
            for (uint32_t y = 0u;
                 y < candidateLevel.state.activeResolution[1]; ++y)
              for (uint32_t x = 0u;
                   x < candidateLevel.state.activeResolution[0]; ++x)
                recomputeParent({
                  static_cast<int32_t>(
                      candidateLevel.state.originCell[0] + x),
                  static_cast<int32_t>(
                      candidateLevel.state.originCell[1] + y),
                  static_cast<int32_t>(
                      candidateLevel.state.originCell[2] + z),
                });
        }
        else
        {
          for (const auto &parent : touchedParentCells)
            recomputeParent(parent);
        }
      }
      touchedChildCells = std::move(touchedParentCells);
    }
  }
  else
  {
    for (SparseVoxelOccupancyMipLevel &level : candidateMipLevels)
      level.lastDirtyWords.clear();
  }

  if (maintainCPUReference_)
    words_ = std::move(candidateWords);
  state_ = candidateState;
  lastDirtyWords_ = std::move(updatePlan.dirtyWords);
  mipLevels_ = std::move(candidateMipLevels);
  return true;
}

bool SparseVoxelOccupancyVolume::occupiedAtWorldPosition(const std::array<float, 3> &worldPosition) const
{
  if (state_.valid == 0u)
    return false;
  std::array<int64_t, 3> globalCell{};
  if (!globalCellForWorld(worldPosition, state_.cellSize, globalCell))
    return false;
  std::array<uint32_t, 3> logical{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t coordinate = globalCell[axis] - state_.originCell[axis];
    if (coordinate < 0 || coordinate >= state_.logicalResolution[axis])
      return false;
    logical[axis] = static_cast<uint32_t>(coordinate);
  }
  const SparseVoxelOccupancyAddress address = sparseVoxelOccupancyAddress(logical, state_);
  return (words_[address.wordIndex] & address.bitMask) != 0u;
}

bool SparseVoxelOccupancyVolume::occupiedAtGlobalCell(const uint32_t level, const std::array<int32_t, 3> &globalCell) const
{
  if (state_.valid == 0u)
    return false;
  if (level == 0u)
    return occupiedBaseCell(state_, words_, globalCell);
  if (level > mipLevels_.size())
    return false;
  const SparseVoxelOccupancyMipLevel &mip = mipLevels_[level - 1u];
  return occupiedMipCell(mip.state, mip.words, globalCell);
}

} // namespace voxel::svraster
