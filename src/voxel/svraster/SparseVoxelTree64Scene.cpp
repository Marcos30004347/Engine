#include "voxel/svraster/SparseVoxelTree64Scene.hpp"

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

bool validBounds(const SparseVoxelAABB &bounds)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (!std::isfinite(bounds.minimum[axis]) || !std::isfinite(bounds.maximum[axis]) || bounds.minimum[axis] > bounds.maximum[axis])
      return false;
  return true;
}

bool validState(const SparseVoxelTree64StateGPU &state)
{
  if (state.valid == 0u || !sparseVoxelTree64SupportedSide(state.chunkSide))
    return false;
  uint64_t chunkCount = 1u;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (state.logicalChunkResolution[axis] == 0u || !std::isfinite(state.cellSize[axis]) || state.cellSize[axis] <= 0.0f)
      return false;
    if (chunkCount > std::numeric_limits<uint64_t>::max() / state.logicalChunkResolution[axis])
      return false;
    chunkCount *= state.logicalChunkResolution[axis];
    const int64_t last = static_cast<int64_t>(state.originWorldChunk[axis]) + state.logicalChunkResolution[axis] - 1u;
    if (last > std::numeric_limits<int32_t>::max())
      return false;
  }
  if (chunkCount > std::numeric_limits<size_t>::max())
    return false;
  return true;
}

std::vector<std::array<int32_t, 3>> activeChunks(const SparseVoxelTree64StateGPU &state)
{
  std::vector<std::array<int32_t, 3>> chunks;
  const uint64_t count = uint64_t{state.logicalChunkResolution[0]} * state.logicalChunkResolution[1] * state.logicalChunkResolution[2];
  if (count > std::numeric_limits<size_t>::max())
    return chunks;
  chunks.reserve(static_cast<size_t>(count));
  for (uint32_t z = 0u; z < state.logicalChunkResolution[2]; ++z)
    for (uint32_t y = 0u; y < state.logicalChunkResolution[1]; ++y)
      for (uint32_t x = 0u; x < state.logicalChunkResolution[0]; ++x)
        chunks.push_back({
          static_cast<int32_t>(static_cast<int64_t>(state.originWorldChunk[0]) + x),
          static_cast<int32_t>(static_cast<int64_t>(state.originWorldChunk[1]) + y),
          static_cast<int32_t>(static_cast<int64_t>(state.originWorldChunk[2]) + z),
        });
  return chunks;
}

bool intersectsChunk(const SparseVoxelTree64StateGPU &state, const std::array<int32_t, 3> &worldChunk, const SparseVoxelAABB &bounds)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double chunkMinimum = static_cast<double>(worldChunk[axis]) * state.chunkSide * state.cellSize[axis];
    const double chunkMaximum = chunkMinimum + state.chunkSide * state.cellSize[axis];
    if (static_cast<double>(bounds.maximum[axis]) < chunkMinimum || static_cast<double>(bounds.minimum[axis]) > chunkMaximum)
      return false;
  }
  return true;
}

} // namespace

bool sparseVoxelTree64ClassifyDirtyChunks(
    const SparseVoxelTree64StateGPU &state,
    const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
    const bool completeInvalidation,
    std::vector<std::array<int32_t, 3>> &dirtyChunks,
    SparseVoxelTree64SceneClassificationStats *stats,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  dirtyChunks.clear();
  if (stats != nullptr)
    *stats = {};
  if (!validState(state))
    return fail(error, "Tree64 scene classification state is invalid");
  for (const auto &bounds : invalidatedWorldBounds)
    if (!validBounds(bounds))
      return fail(error, "Tree64 scene invalidation bounds are invalid");

  const auto allActive = activeChunks(state);
  if (stats != nullptr)
  {
    stats->invalidationBounds = static_cast<uint32_t>(invalidatedWorldBounds.size());
    stats->activeChunks = static_cast<uint32_t>(allActive.size());
    stats->fullActiveRebuild = !completeInvalidation;
  }
  if (!completeInvalidation)
  {
    dirtyChunks = allActive;
  }
  else
  {
    for (const auto &chunk : allActive)
      for (const auto &bounds : invalidatedWorldBounds)
        if (intersectsChunk(state, chunk, bounds))
        {
          dirtyChunks.push_back(chunk);
          break;
        }
  }
  std::sort(dirtyChunks.begin(), dirtyChunks.end());
  if (stats != nullptr)
    stats->dirtyChunks = static_cast<uint32_t>(dirtyChunks.size());
  return true;
}

bool sparseVoxelTree64MakeBuildSources(
    const SparseVoxelTree64StateGPU &state,
    const std::vector<std::array<int32_t, 3>> &buildChunks,
    const std::vector<SparseVoxelAABB> &residentLeafWorldBounds,
    std::vector<SparseVoxelTree64BuildSourceGPU> &sources,
    SparseVoxelTree64SceneSourceStats *stats,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  sources.clear();
  if (stats != nullptr)
    *stats = {};
  if (!validState(state))
    return fail(error, "Tree64 scene source state is invalid");
  if (buildChunks.size() > std::numeric_limits<uint32_t>::max())
    return fail(error, "Tree64 scene build-job count exceeds 32-bit addressing");
  for (const auto &bounds : residentLeafWorldBounds)
    if (!validBounds(bounds))
      return fail(error, "Tree64 resident leaf bounds are invalid");
  if (stats != nullptr)
    stats->leafBoundsTested = residentLeafWorldBounds.size();

  for (uint32_t jobIndex = 0u; jobIndex < buildChunks.size(); ++jobIndex)
  {
    const auto &chunk = buildChunks[jobIndex];
    for (const auto &bounds : residentLeafWorldBounds)
    {
      if (stats != nullptr)
        ++stats->chunkLeafTests;
      if (!intersectsChunk(state, chunk, bounds))
        continue;
      SparseVoxelTree64BuildSourceGPU source{};
      source.jobIndex = jobIndex;
      bool nonempty = true;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const double chunkMinimum = static_cast<double>(chunk[axis]) * state.chunkSide * state.cellSize[axis];
        const double localMinimum = (static_cast<double>(bounds.minimum[axis]) - chunkMinimum) / state.cellSize[axis];
        const double localMaximum = (static_cast<double>(bounds.maximum[axis]) - chunkMinimum) / state.cellSize[axis];
        source.minimum[axis] = static_cast<float>(std::clamp(localMinimum, 0.0, static_cast<double>(state.chunkSide)));
        source.maximum[axis] = static_cast<float>(std::clamp(localMaximum, 0.0, static_cast<double>(state.chunkSide)));
        nonempty &= source.minimum[axis] < source.maximum[axis];
      }
      if (!nonempty)
        continue;
      sources.push_back(source);
      if (stats != nullptr)
        ++stats->emittedChunkLeaves;
    }
  }
  return true;
}

} // namespace voxel::svraster
