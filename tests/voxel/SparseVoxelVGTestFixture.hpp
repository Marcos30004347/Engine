#pragma once

// Shared fixture for the sparse voxel virtual geometry GPU suites: a synthetic
// 64-cell voxel chunk built into a streamable virtual geometry asset, a camera
// helper, and the CPU replica of the culler's projected-error test.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "math/math.hpp"
#include "os/Logger.hpp"
#include "rendering/core/Camera.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

namespace vgfixture
{

namespace svr = voxel::svraster;

inline uint32_t columnHeight(const uint32_t x, const uint32_t z, const uint32_t side)
{
  uint32_t height = 4u;
  if (x >= side / 2u)
    height += 6u;
  if (z >= side * 3u / 4u)
    height += 3u;
  if (x == side / 4u && z == side / 4u)
    height += 9u;
  if (x > side / 8u && x < side / 8u + 3u && z > side / 2u && z < side / 2u + 3u)
    height = height > 3u ? height - 3u : 1u;
  // A ridge, so the surface has enough relief to occlude itself.
  if (z > side / 3u && z < side / 3u + 4u)
    height += 5u + (x % 3u);
  return height;
}

inline bool buildChunkOctree(const uint32_t side, svr::SparseVoxelBrickHierarchy &hierarchy, std::string *error)
{
  std::vector<svr::SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<svr::SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> leaves;

  constexpr uint32_t kShellDepth = 3u;
  for (uint32_t x = 0u; x < side; ++x)
    for (uint32_t z = 0u; z < side; ++z)
    {
      const uint32_t top = columnHeight(x, z, side);
      uint32_t lowestNeighbour = top;
      const std::array<std::array<int32_t, 2>, 4> neighbours{{{{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}}}};
      for (const auto &offset : neighbours)
      {
        const int32_t nx = static_cast<int32_t>(x) + offset[0];
        const int32_t nz = static_cast<int32_t>(z) + offset[1];
        if (nx < 0 || nz < 0 || nx >= static_cast<int32_t>(side) || nz >= static_cast<int32_t>(side))
          continue;
        lowestNeighbour = std::min(lowestNeighbour, columnHeight(static_cast<uint32_t>(nx), static_cast<uint32_t>(nz), side));
      }
      const uint32_t floor = std::min(top > kShellDepth ? top - kShellDepth : 0u, lowestNeighbour > 0u ? lowestNeighbour - 1u : 0u);
      for (uint32_t y = floor; y < top; ++y)
      {
        svr::SparseVoxelCenterLengthGPU cell{};
        cell.center = {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f};
        cell.length = 1.0f;
        svr::SparseVoxelMaterialGPU material{};
        material.albedoRGBA8 = 0xff7f9f5fu;
        material.materialId = (x / 8u + z / 8u) % 3u;
        leaves.push_back(static_cast<uint32_t>(centersLengths.size()));
        centersLengths.push_back(cell);
        materials.push_back(material);
      }
    }

  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.maximumBricks = 1u << 16u;
  return svr::buildSparseVoxelBrickHierarchy(centersLengths, materials, leaves, settings, hierarchy, error);
}

struct ChunkAsset
{
  virtualgeometry::VirtualGeometryBuildData build;
  svr::SparseVoxelOctreeVirtualGeometryStats stats;
  virtualgeometry::VirtualGeometryEncodedData encoded;
  uint32_t leafClusterCount = 0u;
  uint32_t maximumLevel = 0u;
  bool valid = false;
};

inline ChunkAsset buildChunkAsset(const uint32_t side, const uint32_t groupsPerPage)
{
  ChunkAsset asset{};
  svr::SparseVoxelBrickHierarchy hierarchy{};
  std::string error;
  if (!buildChunkOctree(side, hierarchy, &error))
  {
    os::Logger::errorf("voxel chunk build failed: %s", error.c_str());
    return asset;
  }
  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  octreeSettings.originOffset = {static_cast<double>(side) * 0.5, 0.0, static_cast<double>(side) * 0.5};
  virtualgeometry::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = groupsPerPage;
  buildSettings.maxRootPageGroups = 1u;
  if (!svr::buildSparseVoxelOctreeVirtualGeometry(
          hierarchy, 0u, static_cast<uint32_t>(hierarchy.nodes.size()), octreeSettings, buildSettings, asset.build, &asset.stats, &error))
  {
    os::Logger::errorf("octree virtual geometry build failed: %s", error.c_str());
    return asset;
  }
  virtualgeometry::QuantizationConfig quantization{};
  quantization.quantization_factor = 4u;
  quantization.unit_scale = 100.0f;
  quantization.padMeshlets = true;
  asset.encoded = virtualgeometry::VirtualGeometryCompressor::encode(asset.build, quantization);
  for (uint32_t cluster = 0u; cluster < asset.build.clusterDAG.size(); ++cluster)
    asset.leafClusterCount += asset.build.clusterDAG[cluster].empty() ? 1u : 0u;
  for (const auto &source : asset.stats.clusterSources)
    asset.maximumLevel = std::max(asset.maximumLevel, source.level);
  asset.valid = !asset.encoded.pages.empty() && !asset.encoded.hierarchy.empty();
  return asset;
}

struct CameraState
{
  math::Mat4f view;
  math::Mat4f projection;
  math::Vec3f eye;
};

inline CameraState makeCamera(const math::Vec3f &eye, const math::Vec3f &target, const float nearPlane, const float farPlane)
{
  rendering::Camera camera(60.0f * 3.14159265f / 180.0f, eye, math::Vec3f(0.0f, 0.0f, -1.0f), true);
  camera.setAspectRatio(1.0f);
  camera.setNearFar(nearPlane, farPlane);
  camera.lookAt(target);
  camera.updateMatrices();
  return CameraState{camera.getViewMatrix(), camera.getProjectionMatrix(), camera.getPosition()};
}

// calculateProjectedError in culling_multipass.wgsl, identity model matrix.
inline float projectedError(
    const CameraState &camera, const float center[3], const float radius, const float error, const float nearPlane, const uint32_t imageHeight)
{
  const float dx = center[0] - camera.eye[0];
  const float dy = center[1] - camera.eye[1];
  const float dz = center[2] - camera.eye[2];
  const float distance = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - radius, nearPlane);
  const float proj11 = camera.projection.data[5];
  return (error / distance) * proj11 * 0.5f * static_cast<float>(imageHeight);
}

} // namespace vgfixture
