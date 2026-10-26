#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "math/math.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination::worldgi
{

class WorldGILightBVHBuildPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t lightCapacity = kWorldGILightGridMaxLights;
    uint32_t frameIndex = 0u;
    bool enabled = true;
    math::Vec3f directionalLightDirection = math::Vec3f(0.0f, -1.0f, 0.0f);
    math::Vec3f directionalLightColor = math::Vec3f(1.0f);
    float directionalLightIntensity = 1.0f;
    // Angular radius of the directional emitter. Stored in shapeParams.x so
    // visibility samplers can trace the actual sun disk instead of a delta
    // direction and temporally converge to a soft penumbra.
    float directionalLightAngularRadius = 0.0f;
    bool directionalLightEnabled = true;
    uint32_t directionalLightRevision = 0u;
    // Authored point, sphere and planar emitters, in any number up to whatever
    // capacity is left after the two infinite lights.
    std::vector<WorldGIFiniteLight> finiteLights;
    math::Vec3f skyUp = math::Vec3f(0.0f, 1.0f, 0.0f);
    math::Vec3f skyHorizonColor = math::Vec3f(0.55f, 0.72f, 1.0f);
    math::Vec3f skyZenithColor = math::Vec3f(0.08f, 0.24f, 0.55f);
    math::Vec3f skyGroundColor = math::Vec3f(0.05f, 0.045f, 0.04f);
    float skyHorizonExponent = 0.35f;
    float skyIntensity = 1.0f;
    bool skyEnabled = false;
    bool skyAtmosphereSH = false;
    uint32_t skyRevision = 0u;
  };

  WorldGILightBVHBuildPass(
      rendering::Buffer lightBuffer,
      rendering::Buffer bvhNodeBuffer,
      rendering::Buffer bvhInfoBuffer,
      Settings settings)
      : lightBuffer_(lightBuffer),
        bvhNodeBuffer_(bvhNodeBuffer),
        bvhInfoBuffer_(bvhInfoBuffer),
        settings_(sanitizeSettings(settings))
  {
  }

  void setSettings(Settings settings)
  {
    settings_ = sanitizeSettings(settings);
    // This pass records no GPU build commands: the CPU builder uploads the
    // finished light list and BVH directly. Command buffers are cached by the
    // RenderGraph, so runtime edits must upload into the current frame-local
    // buffer slot here instead of waiting for recordCommandBuffer().
    if (renderGraph != nullptr)
      buildAndUpload();
  }

  void recordCommandBuffer(rendering::CommandRecorder &) override
  {
    buildAndUpload();
  }

private:
  struct Bounds
  {
    float min[3]{0.0f, 0.0f, 0.0f};
    float max[3]{0.0f, 0.0f, 0.0f};
  };

  struct LightRecord
  {
    uint32_t lightIndex = UINT32_MAX;
    uint32_t nodeType = WorldGILightBVHNodeType_Invalid;
    Bounds bounds{};
    float centroid[3]{0.0f, 0.0f, 0.0f};
    float importance = 0.0f;
  };

  static Settings sanitizeSettings(Settings settings)
  {
    settings.lightCapacity = std::clamp(settings.lightCapacity, 1u, kWorldGILightGridMaxLights);
    settings.directionalLightIntensity = std::max(0.0f, settings.directionalLightIntensity);
    settings.directionalLightAngularRadius = std::clamp(
        settings.directionalLightAngularRadius, 0.0f, 0.25f);
    for (WorldGIFiniteLight &light : settings.finiteLights)
    {
      light.range = std::max(0.0f, light.range);
      light.intensity = std::max(0.0f, light.intensity);
      light.radius = std::max(0.0f, light.radius);
      light.halfWidth = std::max(0.0f, light.halfWidth);
      light.halfHeight = std::max(0.0f, light.halfHeight);
    }
    settings.skyHorizonExponent = std::max(0.01f, settings.skyHorizonExponent);
    settings.skyIntensity = std::max(0.0f, settings.skyIntensity);
    return settings;
  }

  static bool lightIsEnabled(const WorldGILightGPU &light)
  {
    return (light.flags[0] & WorldGILightFlag_Enabled) != 0u && light.flags[1] != WorldGILightType_Invalid && light.colorIntensity[3] > 0.0f;
  }

  static Bounds emptyBounds()
  {
    const float inf = std::numeric_limits<float>::infinity();
    return Bounds{.min = {inf, inf, inf}, .max = {-inf, -inf, -inf}};
  }

  static void expand(Bounds &bounds, const Bounds &other)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      bounds.min[axis] = std::min(bounds.min[axis], other.min[axis]);
      bounds.max[axis] = std::max(bounds.max[axis], other.max[axis]);
    }
  }

  static Bounds lightBounds(const WorldGILightGPU &light, uint32_t &nodeType)
  {
    nodeType = WorldGILightBVHNodeType_Leaf;
    float extent[3] = {
        std::max(light.positionRange[3], 0.0f),
        std::max(light.positionRange[3], 0.0f),
        std::max(light.positionRange[3], 0.0f),
    };
    if (light.flags[1] == WorldGILightType_Sphere)
    {
      const float radius = std::max(light.positionRange[3], 0.0f) + std::max(light.shapeParams[0], 0.0f);
      extent[0] = radius;
      extent[1] = radius;
      extent[2] = radius;
    }
    else if (light.flags[1] == WorldGILightType_Planar)
    {
      float normal[3] = {light.directionType[0], light.directionType[1], light.directionType[2]};
      const float normalLength = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
      if (normalLength > 1.0e-6f)
        for (float &component : normal)
          component /= normalLength;
      else
        normal[1] = -1.0f;
      const float reference[3] = {std::abs(normal[1]) < 0.999f ? 0.0f : 1.0f, std::abs(normal[1]) < 0.999f ? 1.0f : 0.0f, 0.0f};
      float tangent[3] = {
          reference[1] * normal[2] - reference[2] * normal[1],
          reference[2] * normal[0] - reference[0] * normal[2],
          reference[0] * normal[1] - reference[1] * normal[0],
      };
      const float tangentLength = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
      for (float &component : tangent)
        component /= std::max(tangentLength, 1.0e-6f);
      const float bitangent[3] = {
          normal[1] * tangent[2] - normal[2] * tangent[1],
          normal[2] * tangent[0] - normal[0] * tangent[2],
          normal[0] * tangent[1] - normal[1] * tangent[0],
      };
      const float halfWidth = std::max(light.shapeParams[1], 0.0f);
      const float halfHeight = std::max(light.shapeParams[2], 0.0f);
      const float range = std::max(light.positionRange[3], 0.0f);
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        extent[axis] = std::abs(tangent[axis]) * halfWidth + std::abs(bitangent[axis]) * halfHeight + range;
    }
    return Bounds{
        .min = {light.positionRange[0] - extent[0], light.positionRange[1] - extent[1], light.positionRange[2] - extent[2]},
        .max = {light.positionRange[0] + extent[0], light.positionRange[1] + extent[1], light.positionRange[2] + extent[2]},
    };
  }

  static float lightImportance(const WorldGILightGPU &light)
  {
    const float luminance = 0.2126f * light.colorIntensity[0] + 0.7152f * light.colorIntensity[1] + 0.0722f * light.colorIntensity[2];
    return std::max(0.0f, luminance) * std::max(0.0f, light.colorIntensity[3]);
  }

  uint32_t buildLightList(std::vector<WorldGILightGPU> &lights) const
  {
    lights.assign(settings_.lightCapacity, WorldGILightGPU{});
    uint32_t lightCount = 0u;

    auto pushLight = [&](const WorldGILightGPU &light)
    {
      if (lightCount < settings_.lightCapacity)
        lights[lightCount++] = light;
    };

    WorldGILightGPU directional{};
    directional.directionType[0] = settings_.directionalLightDirection.x();
    directional.directionType[1] = settings_.directionalLightDirection.y();
    directional.directionType[2] = settings_.directionalLightDirection.z();
    directional.colorIntensity[0] = settings_.directionalLightColor.x();
    directional.colorIntensity[1] = settings_.directionalLightColor.y();
    directional.colorIntensity[2] = settings_.directionalLightColor.z();
    directional.colorIntensity[3] = settings_.directionalLightEnabled ? settings_.directionalLightIntensity : 0.0f;
    directional.shapeParams[0] =
        settings_.directionalLightAngularRadius;
    directional.flags[0] = settings_.directionalLightEnabled ? WorldGILightFlag_Enabled : WorldGILightFlag_None;
    directional.flags[1] = WorldGILightType_Directional;
    directional.flags[2] = 0u;
    directional.flags[3] = settings_.directionalLightRevision;
    pushLight(directional);

    WorldGILightGPU sky{};
    sky.positionRange[0] = settings_.skyHorizonColor.x();
    sky.positionRange[1] = settings_.skyHorizonColor.y();
    sky.positionRange[2] = settings_.skyHorizonColor.z();
    sky.directionType[0] = settings_.skyUp.x();
    sky.directionType[1] = settings_.skyUp.y();
    sky.directionType[2] = settings_.skyUp.z();
    sky.shapeParams[0] = settings_.skyGroundColor.x();
    sky.shapeParams[1] = settings_.skyGroundColor.y();
    sky.shapeParams[2] = settings_.skyGroundColor.z();
    sky.shapeParams[3] = settings_.skyHorizonExponent;
    sky.colorIntensity[0] = settings_.skyZenithColor.x();
    sky.colorIntensity[1] = settings_.skyZenithColor.y();
    sky.colorIntensity[2] = settings_.skyZenithColor.z();
    sky.colorIntensity[3] = settings_.skyEnabled ? settings_.skyIntensity : 0.0f;
    sky.flags[0] = settings_.skyEnabled
        ? WorldGILightFlag_Enabled |
            (settings_.skyAtmosphereSH
                ? WorldGILightFlag_AtmosphereSH : WorldGILightFlag_None)
        : WorldGILightFlag_None;
    sky.flags[1] = WorldGILightType_Sky;
    sky.flags[2] = 4u;
    sky.flags[3] = settings_.skyRevision;
    pushLight(sky);

    // The two infinite lights are pushed first so that a scene authored with
    // more finite lights than capacity drops the surplus emitters rather than
    // silently losing its sky.
    for (const WorldGIFiniteLight &light : settings_.finiteLights)
      pushLight(worldGIMakeFiniteLight(light));

    return lightCount;
  }

  uint32_t emitNode(
      std::vector<WorldGILightBVHNodeGPU> &nodes,
      std::vector<uint32_t> &recordIndices,
      const std::vector<LightRecord> &records,
      uint32_t begin,
      uint32_t end) const
  {
    const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
    nodes.emplace_back();
    if (end <= begin)
      return nodeIndex;

    if (end - begin == 1u)
    {
      const LightRecord &record = records[recordIndices[begin]];
      WorldGILightBVHNodeGPU &node = nodes[nodeIndex];
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        node.boundsMinNodeType[axis] = record.bounds.min[axis];
        node.boundsMaxImportance[axis] = record.bounds.max[axis];
      }
      node.boundsMinNodeType[3] = static_cast<float>(record.nodeType);
      node.boundsMaxImportance[3] = record.importance;
      node.childOrLightCount[0] = kWorldGILightBVHInvalidNode;
      node.childOrLightCount[1] = kWorldGILightBVHInvalidNode;
      node.childOrLightCount[2] = record.lightIndex;
      node.childOrLightCount[3] = 0u;
      return nodeIndex;
    }

    Bounds bounds = emptyBounds();
    float centroidMin[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    float centroidMax[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    float importance = 0.0f;
    for (uint32_t index = begin; index < end; ++index)
    {
      const LightRecord &record = records[recordIndices[index]];
      expand(bounds, record.bounds);
      importance += record.importance;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        centroidMin[axis] = std::min(centroidMin[axis], record.centroid[axis]);
        centroidMax[axis] = std::max(centroidMax[axis], record.centroid[axis]);
      }
    }

    uint32_t splitAxis = 0u;
    float widestExtent = centroidMax[0] - centroidMin[0];
    for (uint32_t axis = 1u; axis < 3u; ++axis)
    {
      const float extent = centroidMax[axis] - centroidMin[axis];
      if (extent > widestExtent)
      {
        widestExtent = extent;
        splitAxis = axis;
      }
    }

    const uint32_t mid = begin + (end - begin) / 2u;
    std::nth_element(
        recordIndices.begin() + begin,
        recordIndices.begin() + mid,
        recordIndices.begin() + end,
        [&](uint32_t lhs, uint32_t rhs)
        {
          return records[lhs].centroid[splitAxis] < records[rhs].centroid[splitAxis];
        });

    const uint32_t leftChild = emitNode(nodes, recordIndices, records, begin, mid);
    const uint32_t rightChild = emitNode(nodes, recordIndices, records, mid, end);
    WorldGILightBVHNodeGPU &node = nodes[nodeIndex];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      node.boundsMinNodeType[axis] = bounds.min[axis];
      node.boundsMaxImportance[axis] = bounds.max[axis];
    }
    node.boundsMinNodeType[3] = static_cast<float>(WorldGILightBVHNodeType_Internal);
    node.boundsMaxImportance[3] = importance;
    node.childOrLightCount[0] = leftChild;
    node.childOrLightCount[1] = rightChild;
    node.childOrLightCount[2] = UINT32_MAX;
    node.childOrLightCount[3] = 0u;
    return nodeIndex;
  }

  void buildAndUpload()
  {
    std::vector<WorldGILightGPU> lights;
    const uint32_t lightCount = buildLightList(lights);
    renderGraph->bufferWrite(lightBuffer_, 0u, static_cast<uint64_t>(lights.size()) * sizeof(WorldGILightGPU), lights.data());

    std::vector<LightRecord> records;
    records.reserve(lightCount);
    std::vector<uint32_t> infiniteLightIndices;
    infiniteLightIndices.reserve(lightCount);
    if (settings_.enabled)
    {
      for (uint32_t lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
      {
        const WorldGILightGPU &light = lights[lightIndex];
        if (!lightIsEnabled(light))
          continue;

        LightRecord record{};
        record.lightIndex = lightIndex;
        if (light.flags[1] == WorldGILightType_Directional ||
            light.flags[1] == WorldGILightType_Sky)
        {
          // Infinite emitters are queried separately by the shader. Keep
          // their compact records at the beginning of the node array and do
          // not let them inflate the finite-light BVH bounds.
          infiniteLightIndices.push_back(lightIndex);
          continue;
        }
        record.bounds = lightBounds(light, record.nodeType);
        record.importance = lightImportance(light);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
          record.centroid[axis] = (record.bounds.min[axis] + record.bounds.max[axis]) * 0.5f;
        records.push_back(record);
      }
    }

    std::vector<WorldGILightBVHNodeGPU> nodes;
    nodes.reserve(worldGILightBVHNodeCapacity(settings_.lightCapacity));
    for (const uint32_t lightIndex : infiniteLightIndices)
    {
      WorldGILightBVHNodeGPU node{};
      node.boundsMinNodeType[3] =
          static_cast<float>(WorldGILightBVHNodeType_Infinite);
      node.boundsMaxImportance[3] = lightImportance(lights[lightIndex]);
      node.childOrLightCount[2] = lightIndex;
      node.childOrLightCount[3] = 1u;
      nodes.push_back(node);
    }
    uint32_t rootNodeIndex = kWorldGILightBVHInvalidNode;
    if (!records.empty())
    {
      std::vector<uint32_t> recordIndices(records.size());
      std::iota(recordIndices.begin(), recordIndices.end(), 0u);
      rootNodeIndex = emitNode(nodes, recordIndices, records, 0u, static_cast<uint32_t>(recordIndices.size()));
    }

    const uint32_t builtNodeCount = static_cast<uint32_t>(nodes.size());
    nodes.resize(worldGILightBVHNodeCapacity(settings_.lightCapacity));
    WorldGILightBVHInfoGPU info{};
    info.nodeCount = builtNodeCount;
    info.rootNodeIndex = rootNodeIndex;
    info.lightCount = lightCount;
    info.infiniteLightCount =
        static_cast<uint32_t>(infiniteLightIndices.size());

    renderGraph->bufferWrite(bvhNodeBuffer_, 0u, static_cast<uint64_t>(nodes.size()) * sizeof(WorldGILightBVHNodeGPU), nodes.data());
    renderGraph->bufferWrite(bvhInfoBuffer_, 0u, sizeof(info), &info);
  }

  rendering::Buffer lightBuffer_{};
  rendering::Buffer bvhNodeBuffer_{};
  rendering::Buffer bvhInfoBuffer_{};
  Settings settings_{};
};

} // namespace globalillumination::worldgi
