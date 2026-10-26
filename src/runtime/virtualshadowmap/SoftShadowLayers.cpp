#include "virtualshadowmap/SoftShadowLayers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace virtualgeometry
{

namespace
{

float conservativeRadius(const SoftShadowLightVolume &light)
{
  if (!light.shadowCasting)
  {
    return 0.0f;
  }
  if (light.type == SoftShadowLightType::Directional)
  {
    return std::numeric_limits<float>::infinity();
  }
  return std::max(light.range, 0.0f);
}

bool isGlobalLight(const SoftShadowLightVolume &light)
{
  return light.shadowCasting && light.type == SoftShadowLightType::Directional;
}

} // namespace

bool softShadowLightVolumesOverlap(const SoftShadowLightVolume &a, const SoftShadowLightVolume &b)
{
  if (!a.shadowCasting || !b.shadowCasting)
  {
    return false;
  }
  if (isGlobalLight(a) || isGlobalLight(b))
  {
    return true;
  }

  const float radiusA = conservativeRadius(a);
  const float radiusB = conservativeRadius(b);
  const float overlapDistance = radiusA + radiusB;
  const math::Vec3f delta = a.position - b.position;
  return delta.dot(delta) <= overlapDistance * overlapDistance;
}

SoftShadowLayerBuildResult buildSoftShadowLayerAssignment(const std::vector<SoftShadowLightVolume> &lights, uint32_t maxSoftShadowLayers)
{
  SoftShadowLayerBuildResult result{};
  result.assignments.assign(lights.size(), {});
  for (SoftShadowLayerAssignment &assignment : result.assignments)
  {
    assignment.fallback = true;
  }

  if (lights.empty() || maxSoftShadowLayers == 0u)
  {
    result.overflowCount = static_cast<uint32_t>(lights.size());
    for (SoftShadowLayerAssignment &assignment : result.assignments)
    {
      assignment.fallback = true;
    }
    return result;
  }

  const size_t lightCount = lights.size();
  std::vector<std::vector<uint32_t>> adjacency(lightCount);
  std::vector<uint32_t> order;
  order.reserve(lightCount);

  for (uint32_t lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
  {
    if (lights[lightIndex].shadowCasting)
    {
      order.push_back(lightIndex);
    }
  }

  for (uint32_t a = 0u; a < lightCount; ++a)
  {
    for (uint32_t b = a + 1u; b < lightCount; ++b)
    {
      if (!softShadowLightVolumesOverlap(lights[a], lights[b]))
      {
        continue;
      }
      adjacency[a].push_back(b);
      adjacency[b].push_back(a);
    }
  }

  for (const std::vector<uint32_t> &edges : adjacency)
  {
    result.maxGraphDegree = std::max<uint32_t>(result.maxGraphDegree, static_cast<uint32_t>(edges.size()));
  }

  std::stable_sort(
      order.begin(),
      order.end(),
      [&](uint32_t a, uint32_t b)
      {
        const bool globalA = isGlobalLight(lights[a]);
        const bool globalB = isGlobalLight(lights[b]);
        if (globalA != globalB)
        {
          return globalA;
        }
        if (adjacency[a].size() != adjacency[b].size())
        {
          return adjacency[a].size() > adjacency[b].size();
        }
        return a < b;
      });

  for (uint32_t lightIndex : order)
  {
    bool usedLayers[64]{};
    for (uint32_t neighbor : adjacency[lightIndex])
    {
      const SoftShadowLayerAssignment &neighborAssignment = result.assignments[neighbor];
      if (!neighborAssignment.fallback && neighborAssignment.layer < 64u)
      {
        usedLayers[neighborAssignment.layer] = true;
      }
    }

    uint32_t selectedLayer = maxSoftShadowLayers;
    for (uint32_t layer = 0u; layer < maxSoftShadowLayers && layer < 64u; ++layer)
    {
      if (!usedLayers[layer])
      {
        selectedLayer = layer;
        break;
      }
    }

    if (selectedLayer >= maxSoftShadowLayers || selectedLayer >= 64u)
    {
      result.assignments[lightIndex].fallback = true;
      ++result.overflowCount;
      continue;
    }

    result.assignments[lightIndex].layer = selectedLayer;
    result.assignments[lightIndex].fallback = false;
    result.activeLayerCount = std::max(result.activeLayerCount, selectedLayer + 1u);
  }

  return result;
}

} // namespace virtualgeometry
