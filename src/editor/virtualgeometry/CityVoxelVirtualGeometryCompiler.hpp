#pragma once

#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "voxel/svraster/CityVoxelSurfaceAddress.hpp"

#include <functional>

namespace voxel::svraster {

struct CityVoxelVGSettings {
  uint32_t maximumLevels = 12; // Includes LOD0.
  uint32_t maximumNodes = 100000;
  uint64_t maximumEncodedBytes = 256ull * 1024ull * 1024ull;
  uint32_t encodeThreads = 1;
  virtualgeometry::VirtualGeometryBuildSettings pageSettings{};
  virtualgeometry::QuantizationConfig quantization{};
  // Called only for occupied cells and their six face directions. Return true
  // for an interface owned by a connected neighbour. Stable callback results
  // become part of the mesh variant hash and encoded page identity.
  std::function<bool(uint32_t, const std::array<int32_t,3>&, uint32_t)> suppressFace;
};

struct CityVoxelVGAsset {
  struct PageRange {
    uint64_t offset = 0; // Relative to the first VG payload byte.
    uint32_t bytes = 0;
    uint64_t checksum = 0;
  };
  uint64_t geometryId = 0;
  uint64_t geometryAddressHash = 0;
  uint64_t encodedPageHash = 0;
  uint64_t faceOwnershipHash = 0;
  // Cross-asset stitch variants are not encoded yet. A runtime must retain
  // LOD0 on connected interfaces while this is true.
  bool requiresFinestInterfaceCut = false;
  SparseVoxelOctreeVirtualGeometryStats buildStats{};
  std::vector<CityVoxelVGClusterSource> clusterSources;
  virtualgeometry::VirtualGeometryEncodedData encoded;
  std::vector<virtualgeometry::VirtualGeometryBuildPage> buildPages;
  std::vector<virtualgeometry::PageBuffer> pages;
  std::vector<PageRange> pageRanges;
  uint64_t encodedBytes = 0;
};

// Offline adapter from the phase-4 sparse occupancy stream to the existing
// octree VG DAG and page encoder. It never reads a CityVoxel appearance asset.
// Encoded pages remain individually range-readable for phase-9 packaging.
bool buildCityVoxelVirtualGeometry(
    const CityVoxelCompiledGeometry &geometry, const CityVoxelVGSettings &settings,
    CityVoxelVGAsset &asset, std::string *error = nullptr);

} // namespace voxel::svraster
