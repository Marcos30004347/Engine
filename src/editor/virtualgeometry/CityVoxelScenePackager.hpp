#pragma once
#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include <unordered_map>

namespace virtualgeometry {
struct CityVoxelScenePackOptions {
  // Pretriangulated hardware ray-trace surfaces, one page per 8,192
  // triangles. A raster-only city leaves them out; they dominate file size.
  bool tracePages = true;
  // Threads building VG pages ahead of the sequential page writer.
  uint32_t workers = 1;
  // Dictionary LZ4 pages (see CityVoxelSceneCompression); readers decode them.
  bool compressPages = true;
  // VG pages from the welded-surface pipeline (lattice clusters) instead of
  // the brick octree; a geometry it cannot build keeps an octree page.
  bool surfaceVirtualGeometry = false;
};

// Offline bridge from compiled shared assets and integer dwelling placements
// to a checked single-file scene. Trace pages contain pretriangulated surfaces.
bool writeCityVoxelCompiledScene(const std::string &path,
    const CityVoxelStaticScene &city,
    const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
    const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
    const std::vector<voxel::svraster::CityVoxelMaterialReference> &materials,
    uint64_t sourceHash, uint64_t settingsHash,
    const voxel::svraster::CityVoxelSceneLimits &limits,
    std::string *error = nullptr,
    const std::unordered_map<uint64_t,uint64_t> *ownerByPlacementId = nullptr,
    const CityVoxelScenePackOptions &options = {});

// Repackage placements against an existing checked asset library. Geometry,
// appearance, trace and VG pages are copied verbatim, so instancing a city
// never rebuilds or expands shared model data.
bool repackCityVoxelSceneInstances(const std::string &libraryPath,
    const std::string &outputPath, const CityVoxelStaticScene &city,
    const std::unordered_map<uint64_t,uint64_t> &ownerByPlacementId,
    uint64_t sourceHash, uint64_t settingsHash,
    const voxel::svraster::CityVoxelSceneLimits &limits,
    std::string *error = nullptr);
}
