#pragma once

#include "editor/voxel/SparseVoxelOBJAnalysis.hpp"
#include "voxel/svraster/SparseVoxelScene.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace voxel::svraster
{

struct SparseVoxelOBJBuildStatistics
{
  uint64_t testedCells = 0u;
  uint64_t occupiedLeafCount = 0u;
  uint64_t hierarchyNodeCount = 0u;
};

// Voxelizing a large OBJ runs for tens of seconds with nothing on screen, so
// callers need a way to say what is happening. `fraction` is in [0, 1] within
// the named phase, or negative when the phase cannot report a ratio.
using SparseVoxelOBJBuildProgressCallback =
    std::function<void(const char *phase, float fraction)>;

bool buildSparseVoxelOBJScene(
    const SparseVoxelOBJAnalysis &analysis,
    const SparseVoxelOBJImportSettings &settings,
    SparseVoxelScene &scene,
    SparseVoxelOBJBuildStatistics *statistics = nullptr,
    std::string *error = nullptr,
    const std::atomic_bool *cancelled = nullptr,
    const SparseVoxelOBJBuildProgressCallback &progress = {},
    // The caller has usually just analyzed the source. Re-analyzing re-reads
    // and re-hashes the whole OBJ purely to detect an edit between the two
    // calls, which on a large scene costs as much as the voxelization itself.
    bool revalidateSource = true);

bool makeBootstrapSparseVoxelPreview(
    const SparseVoxelScene &scene,
    BootstrapSparseVoxelModel &model,
    BootstrapSparseVoxelLOD &hierarchy,
    std::string *error = nullptr);

} // namespace voxel::svraster
