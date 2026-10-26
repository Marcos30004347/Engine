#pragma once

#include "virtualgeometry/rendering/VirtualGeometryComputeRasterPass.hpp"
#include "voxel/svraster/rendering/TerrainMetalComputeRasterPass.hpp"

#include <string>

namespace virtualgeometry::gpgpu
{

// Native Metal software raster backend for ordinary Virtual Geometry decoded
// clusters.  The implementation still lives in the historical sparse-voxel
// source file because terrain was the first generated-payload producer, but the
// contract is now VG-owned: it consumes the VG culling SW cluster queue and the
// decoded VG page buffers, then emits a deterministic packed depth/payload
// stream for the final merge.
using VirtualGeometryMetalComputeRasterPass =
    ::voxel::svraster::rendering::TerrainMetalComputeRasterPass;
using VirtualGeometryMetalComputeRasterPassPlan =
    ::voxel::svraster::rendering::TerrainMetalComputeRasterPassPlan;

inline bool buildVirtualGeometryMetalComputeRasterPassPlan(
    const ::voxel::svraster::SparseVoxelTerrainComputeRasterRouteDecision &route,
    const ::voxel::svraster::SparseVoxelTerrainComputeRasterCapabilities
        &capabilities,
    const ::voxel::svraster::SparseVoxelTerrainComputeRasterSettings &settings,
    VirtualGeometryMetalComputeRasterPassPlan &plan,
    std::string *error = nullptr)
{
  return ::voxel::svraster::rendering::buildTerrainMetalComputeRasterPassPlan(
      route,
      capabilities,
      settings,
      plan,
      error);
}

inline constexpr uint32_t kVirtualGeometryMetalComputeRasterTileSize =
    ::voxel::svraster::rendering::kTerrainMetalComputeRasterTileSize;
inline constexpr uint32_t kVirtualGeometryMetalComputeRasterKernelCount =
    ::voxel::svraster::rendering::kTerrainMetalComputeRasterKernelCount;
inline constexpr const char *kVirtualGeometryMetalComputeRasterShaderPath =
    ::voxel::svraster::rendering::kTerrainMetalComputeRasterShaderPath;
inline constexpr const char *kVirtualGeometryMetalComputeRasterMetallibPath =
    ::voxel::svraster::rendering::kTerrainMetalComputeRasterMetallibPath;

} // namespace virtualgeometry::gpgpu
