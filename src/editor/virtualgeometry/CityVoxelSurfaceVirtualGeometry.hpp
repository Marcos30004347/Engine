#pragma once

#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "voxel/svraster/CityVoxelSceneFormat.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry
{

// City geometry through the welded-surface pipeline instead of the brick
// octree: the occupied cells meshed as one closed surface, merged losslessly,
// simplified by pixel error with the stock builder, and given lattice view
// UVs. The package is the same 'CVGV' page the octree route writes, with one
// cluster source per cluster in lattice mode (`kCityVoxelLatticeClusterScale`
// in cellScale): the resolve then finds each pixel's voxel from the UV and
// the depth along the normal instead of from a brick-local 8x8 window.
constexpr uint32_t kCityVoxelLatticeClusterScale = 0x80000001u;

struct CityVoxelSurfaceVGStats
{
  uint64_t rawTriangles = 0u;
  uint64_t lod0Triangles = 0u;
  uint64_t clusters = 0u;
  uint32_t maxClusterVertices = 0u;
  uint64_t packageBytes = 0u;
};

bool buildCityVoxelSurfaceVGPackage(const voxel::svraster::CityVoxelCompiledGeometry &geometry, uint64_t maximumBytes,
                                    std::vector<uint8_t> &package, CityVoxelSurfaceVGStats *stats = nullptr,
                                    std::string *error = nullptr);

struct CityVoxelSurfaceRebuildStats
{
  uint32_t geometries = 0u;
  uint32_t failed = 0u; // Kept their octree page.
  uint64_t rawTriangles = 0u;
  uint64_t lod0Triangles = 0u;
  uint64_t clusters = 0u;
  uint64_t oldVGBytes = 0u;
  uint64_t newVGBytes = 0u;
  double buildSeconds = 0.0;
  std::vector<std::pair<std::string, uint32_t>> failureReasons; // Most common first.
};

// Copies a .cvscene page for page, rebuilding every VirtualGeometry page from
// its geometry page with the pipeline above (in parallel batches). A geometry
// that fails keeps its octree page, and is counted.
bool rebuildCityVoxelSceneSurfaceVG(const std::string &inputPath, const std::string &outputPath,
                                    const voxel::svraster::CityVoxelSceneLimits &limits, uint32_t threads,
                                    CityVoxelSurfaceRebuildStats *stats = nullptr, std::string *error = nullptr);

struct CityVoxelHouseSceneStats
{
  uint32_t houses = 0u;          // (spatial cell, owner) groups.
  uint32_t mergedHouses = 0u;    // Became one geometry.
  uint32_t splitHouses = 0u;     // Too big for one asset: kept their blocks.
  uint32_t sourcePlacements = 0u;
  uint32_t placements = 0u;
  uint32_t geometries = 0u;      // After the compiler's deduplication.
  uint64_t cells = 0u;
  double seconds = 0.0;
};

// One mesh per house: every (spatial cell, owner) group of a scene's
// placements becomes one cell set in the owner's frame - blocks in a local
// frame are placed by translation only, so this is an exact union - which
// the asset compiler turns into one geometry, one appearance and one
// placement (identical houses still share an asset). Written with
// surface VG pages. A house over the compiler's limits keeps its blocks.
bool buildCityVoxelHouseScene(const std::string &inputPath, const std::string &outputPath,
                              const voxel::svraster::CityVoxelSceneLimits &limits, uint32_t threads,
                              CityVoxelHouseSceneStats *stats = nullptr, std::string *error = nullptr);

struct CityVoxelFarSceneStats
{
  uint32_t rebuilt = 0u;
  uint32_t kept = 0u; // Kept their old page.
  uint64_t triangles = 0u;
  uint64_t oldVGBytes = 0u;
  uint64_t newVGBytes = 0u;
  double seconds = 0.0;
  std::vector<std::pair<std::string, uint32_t>> keptReasons;
};

// Far proxies whose shape comes from the detailed 10 cm cells instead of the
// proxies' own coarse voxels: for each far geometry, its owner's cells are
// gathered from the detailed scene, meshed, merged, simplified as a whole to
// `farErrorMetres` (what a proxy is drawn at), and built into a DAG whose
// lattice UVs address the far geometry's coarse voxels for colour. Every
// other page of the far scene is copied as it is.
bool rebuildCityVoxelFarSceneFromDetail(const std::string &detailPath, const std::string &farPath, const std::string &outputPath,
                                        const voxel::svraster::CityVoxelSceneLimits &limits, uint32_t threads, double farErrorMetres,
                                        CityVoxelFarSceneStats *stats = nullptr, std::string *error = nullptr);

} // namespace virtualgeometry
