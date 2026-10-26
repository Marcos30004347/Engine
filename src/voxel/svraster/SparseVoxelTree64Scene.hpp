#pragma once

#include "voxel/svraster/SparseVoxelSceneManager.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelTree64SceneClassificationStats
{
  uint32_t invalidationBounds = 0u;
  uint32_t dirtyChunks = 0u;
  uint32_t activeChunks = 0u;
  bool fullActiveRebuild = false;
};

struct SparseVoxelTree64SceneSourceStats
{
  uint64_t leafBoundsTested = 0u;
  uint64_t chunkLeafTests = 0u;
  uint64_t emittedChunkLeaves = 0u;
};

// Converts complete old/new invalidation bounds into a stable unique active
// chunk set. Incomplete bounds deliberately select the entire active range.
bool sparseVoxelTree64ClassifyDirtyChunks(
    const SparseVoxelTree64StateGPU &state,
    const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
    bool completeInvalidation,
    std::vector<std::array<int32_t, 3>> &dirtyChunks,
    SparseVoxelTree64SceneClassificationStats *stats = nullptr,
    std::string *error = nullptr);

// CPU/reference bridge for tests and fallback builders. The production T5
// path traverses the same resident instance/BLAS buffers on the GPU and writes
// directly to cell scratch, avoiding a CPU leaf upload.
bool sparseVoxelTree64MakeBuildSources(
    const SparseVoxelTree64StateGPU &state,
    const std::vector<std::array<int32_t, 3>> &buildChunks,
    const std::vector<SparseVoxelAABB> &residentLeafWorldBounds,
    std::vector<SparseVoxelTree64BuildSourceGPU> &sources,
    SparseVoxelTree64SceneSourceStats *stats = nullptr,
    std::string *error = nullptr);

} // namespace voxel::svraster
