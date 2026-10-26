#pragma once

#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "voxel/svraster/SparseVoxelScene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry {

struct CityVoxelOverlapCutouts {
  // Indices refer to source.instances. Cells occupied by any cutter are
  // removed from the target before either asset is compiled. The cutter stays
  // an independently addressable placement and may later be moved or hidden.
  uint32_t targetInstance = UINT32_MAX;
  std::vector<uint32_t> cutterInstances;
};

// Offline bridge for the existing material-aware OBJ voxelizer. It expands
// uniform octree leaves into exact source cells one model at a time, then lets
// CityVoxelAssetCompiler deduplicate geometry independently of appearance.
// The OBJ importer currently emits world-space models with identity instances.
bool compileCityVoxelSparseScene(
    const voxel::svraster::SparseVoxelScene &source,
    voxel::svraster::CityVoxelAssetCompiler &compiler,
    uint32_t pitchMicrometres,
    uint64_t maximumExpandedCellsPerModel,
    uint64_t firstStableId,
    std::vector<voxel::svraster::CityVoxelCompiledPlacement> &placements,
    std::string *error = nullptr,
    const CityVoxelOverlapCutouts *cutouts = nullptr);

} // namespace virtualgeometry
