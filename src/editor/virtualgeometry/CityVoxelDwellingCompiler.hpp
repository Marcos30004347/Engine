#pragma once

#include "editor/virtualgeometry/CityVoxelPartOwnership.hpp"
#include "procedural/world/hierarchy/DwellingVoxelAssembly.hpp"

namespace virtualgeometry {

struct CityVoxelCompiledDwellingPart {
  uint32_t sourcePartIndex=0;
  voxel::svraster::CityVoxelCompiledPlacement placement;
};

// One cell-lattice block of a dwelling's final cells. The asset is stored in
// canonical orientation; the house sees it turned by `quarterTurn` (about +Y,
// the same sense as a frame yaw) at `frameOrigin` in that turned frame.
struct CityVoxelCompiledDwellingBlock {
  std::array<int32_t,3> blockCell{};
  uint8_t quarterTurn=0;
  std::array<int32_t,3> frameOrigin{};
  voxel::svraster::CityVoxelCompiledPlacement placement;
};

// A lattice block in canonical orientation: `cells` start at the origin, and
// the source sees cell l at R^t(frameOrigin + l) for R a +Y quarter turn,
// (x,z)->(-z,x) applied to continuous coordinates.
struct CityVoxelCanonicalBlock {
  std::array<int32_t,3> blockCell{};
  uint8_t quarterTurn=0;
  std::array<int32_t,3> frameOrigin{};
  std::vector<voxel::svraster::CityVoxelCell> cells;
};

// Cuts a disjoint cell set into across x up x across blocks, each reduced to
// its smallest encoding over position and the four quarter turns.
bool sliceCityVoxelBlocks(const std::vector<voxel::svraster::CityVoxelCell> &cells,
    int32_t across,int32_t up,std::vector<CityVoxelCanonicalBlock> &result,
    std::string *error=nullptr);

// Zero uses the dwelling's own cell (voxelsPerCell) on that axis.
struct CityVoxelDwellingBlockSettings {
  uint32_t horizontalVoxels=0;
  uint32_t verticalVoxels=0;
};

struct CityVoxelDwellingCompileReport {
  CityVoxelPartOwnershipReport ownership;
  uint32_t compiledParts=0, omittedGlassParts=0, omittedCoveredStaticParts=0;
  uint32_t omittedPairedFacadeSides=0;
  uint32_t omittedCoveredMovableParts=0; // Block bakes only.
  uint32_t omittedDecorativeOverlays=0;
  uint64_t overlaidCells=0;
  uint32_t fullyCoveredSourcePartIndex=UINT32_MAX;
};

// Consumes one dwelling's authored cell sets. Source part identity survives
// deduplication so door/window placements can be moved independently. The
// cell producer must use assembly.grid's 10 cm integer lattice.
bool compileCityVoxelDwellingParts(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    std::vector<std::vector<voxel::svraster::CityVoxelCell>> &&partCells,
    voxel::svraster::CityVoxelAssetCompiler &assets,
    std::vector<CityVoxelCompiledDwellingPart> &result,
    CityVoxelDwellingCompileReport &report,
    std::string *error=nullptr);

// Native offline entry point: sample the typed skeleton at 10 cm, resolve
// cross-part ownership, then submit reusable geometry and appearance assets.
bool compileCityVoxelDwelling(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    const procedural::world::hierarchy::Skeleton &skeleton,
    voxel::svraster::CityVoxelAssetCompiler &assets,
    std::vector<CityVoxelCompiledDwellingPart> &result,
    CityVoxelDwellingCompileReport &report,
    std::string *error=nullptr);

// Far-field proxy: OR-reduce cells into factor^3 coarse cells (floor
// division, so the coarse lattice nests in the fine one); each coarse cell
// keeps the most common exact colour/material of its fine cells.
void downsampleCityVoxelCells(const std::vector<voxel::svraster::CityVoxelCell> &cells,
    int32_t factor,std::vector<voxel::svraster::CityVoxelCell> &result);

// The dwelling's final disjoint opaque cells: sampled, overlaid, clipped by
// ownership. Reads only its arguments.
bool resolveCityVoxelDwellingCells(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    const procedural::world::hierarchy::Skeleton &skeleton,
    std::vector<voxel::svraster::CityVoxelCell> &cells,uint32_t &pitch,
    CityVoxelDwellingCompileReport &report,std::string *error=nullptr);

// The two halves of compileCityVoxelDwellingBlocks. Slicing reads only its
// arguments and may run on any thread; submission mutates the shared asset
// registry and must be serialised by the caller.
bool sliceCityVoxelDwelling(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    const procedural::world::hierarchy::Skeleton &skeleton,
    const CityVoxelDwellingBlockSettings &settings,
    std::vector<CityVoxelCanonicalBlock> &blocks,uint32_t &pitch,
    CityVoxelDwellingCompileReport &report,std::string *error=nullptr);
bool submitCityVoxelDwellingBlocks(uint64_t dwellingId,uint32_t pitch,
    const std::vector<CityVoxelCanonicalBlock> &blocks,
    voxel::svraster::CityVoxelAssetCompiler &assets,
    std::vector<CityVoxelCompiledDwellingBlock> &result,std::string *error=nullptr);

// Shared-kit entry point: resolve the dwelling exactly as above, then cut the
// final cells into cell-grid blocks deduplicated across position and quarter
// turn. Identical bays in any house, on any side, become one asset.
bool compileCityVoxelDwellingBlocks(
    const procedural::world::hierarchy::DwellingVoxelAssembly &assembly,
    const procedural::world::hierarchy::Skeleton &skeleton,
    voxel::svraster::CityVoxelAssetCompiler &assets,
    const CityVoxelDwellingBlockSettings &settings,
    std::vector<CityVoxelCompiledDwellingBlock> &result,
    CityVoxelDwellingCompileReport &report,
    std::string *error=nullptr);

} // namespace virtualgeometry
