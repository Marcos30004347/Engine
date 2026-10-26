#pragma once

#include "procedural/world/hierarchy/DwellingVoxelAssembly.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"

#include <string>
#include <vector>

namespace virtualgeometry {

// Offline, source-indexed sampling. Coordinates are in the assembly's shared
// integer lattice, so independently compiled parts meet on identical cells.
// The caller can release each returned vector after ownership resolution.
bool sampleCityVoxelDwellingCells(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    const procedural::world::hierarchy::Skeleton &skeleton,
    std::vector<std::vector<voxel::svraster::CityVoxelCell>> &result,
    std::string *error = nullptr);

} // namespace virtualgeometry
