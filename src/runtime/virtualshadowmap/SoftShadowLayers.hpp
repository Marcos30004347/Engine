#pragma once

#include "math/math.hpp"

#include <cstdint>
#include <vector>

namespace virtualgeometry
{

enum class SoftShadowLightType : uint32_t
{
  Directional = 0u,
  Spot = 1u,
  Point = 2u,
};

struct SoftShadowLightVolume
{
  SoftShadowLightType type = SoftShadowLightType::Point;
  math::Vec3f position = math::Vec3f(0.0f, 0.0f, 0.0f);
  math::Vec3f direction = math::Vec3f(0.0f, -1.0f, 0.0f);
  float range = 0.0f;
  float outerAngleRadians = 0.0f;
  bool shadowCasting = true;
};

struct SoftShadowLayerAssignment
{
  uint32_t layer = 0u;
  bool fallback = false;
};

struct SoftShadowLayerBuildResult
{
  std::vector<SoftShadowLayerAssignment> assignments;
  uint32_t activeLayerCount = 0u;
  uint32_t maxGraphDegree = 0u;
  uint32_t overflowCount = 0u;
};

bool softShadowLightVolumesOverlap(const SoftShadowLightVolume &a, const SoftShadowLightVolume &b);
SoftShadowLayerBuildResult buildSoftShadowLayerAssignment(const std::vector<SoftShadowLightVolume> &lights, uint32_t maxSoftShadowLayers);

} // namespace virtualgeometry
