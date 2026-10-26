#pragma once

#include "voxel/svraster/CityVoxelAssetCompiler.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry {

// Surface cells one dwelling may voxelise (10 cm cells: ~40,000 m2 of
// surface). Halls and large compounds went past the old 1M cap.
inline constexpr size_t kCityVoxelDwellingCellBudget = 4'000'000u;

// Resolve one dwelling or one terrain seam batch before assets are compiled.
// Higher priority owns a shared voxel; equal priorities use stable identity.
// All parts remain separate assets and the union of occupied cells is retained.
struct CityVoxelAuthoredPart {
  uint64_t stableId=0;
  uint16_t priority=0;
  std::vector<voxel::svraster::CityVoxelCell> cells;
};

struct CityVoxelPartOwnershipReport {
  uint64_t inputCells=0, uniqueCells=0, clippedCells=0;
  uint64_t equalPriorityConflicts=0;
  uint32_t fullyCoveredPartIndex=UINT32_MAX;
  std::vector<uint32_t> fullyCoveredPartIndices;
};

bool resolveCityVoxelPartOwnership(std::vector<CityVoxelAuthoredPart> &parts,
    CityVoxelPartOwnershipReport &report,std::string *error=nullptr,
    bool permitFullyCovered=false);

} // namespace virtualgeometry
