#pragma once

#include "math/math.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster
{

inline math::Mat4f sparseVoxelJitterProjection(
    const math::Mat4f &projection, const math::Vec2f &jitterPixels,
    const uint32_t width, const uint32_t height)
{
  math::Mat4f result = projection;
  result.at(0, 2) -= 2.0f * jitterPixels[0] / static_cast<float>(std::max(1u, width));
  result.at(1, 2) += 2.0f * jitterPixels[1] / static_cast<float>(std::max(1u, height));
  return result;
}

// RH -Z/Y-up projection; VG negates clip Y for its top-left viewport.
// Sparse rays use +Z/Y-down. Derive their intrinsics from the same matrix
// that rasterized the surface, including jitter and asymmetric frusta.
inline std::array<float, 4> sparseVoxelRasterFocalPrincipal(
    const math::Mat4f &projection, const uint32_t width, const uint32_t height)
{
  const float halfWidth = 0.5f * static_cast<float>(width);
  const float halfHeight = 0.5f * static_cast<float>(height);
  return {projection.at(0, 0) * halfWidth, projection.at(1, 1) * halfHeight,
          (1.0f - projection.at(0, 2)) * halfWidth,
          (1.0f + projection.at(1, 2)) * halfHeight};
}

} // namespace voxel::svraster
