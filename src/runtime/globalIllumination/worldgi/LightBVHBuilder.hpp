#pragma once
#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <vector>

namespace globalillumination::worldgi {
// Shared spatial median builder. Nodes are emitted in preorder; childOrLightCount.w
// is the exclusive end of the subtree, permitting bounded stackless enumeration.
// Light IDs belong to the caller's registry and are never reordered by the tree.
struct LightBVHBuilder {
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

  static uint32_t emitNode(
      std::vector<WorldGILightBVHNodeGPU> &nodes,
      std::vector<uint32_t> &recordIndices,
      const std::vector<LightRecord> &records,
      uint32_t begin,
      uint32_t end)
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
      node.childOrLightCount[3] = static_cast<uint32_t>(nodes.size());
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
          const float a = records[lhs].centroid[splitAxis], b = records[rhs].centroid[splitAxis];
          return a == b ? records[lhs].lightIndex < records[rhs].lightIndex : a < b;
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
    node.childOrLightCount[3] = static_cast<uint32_t>(nodes.size());
    return nodeIndex;
  }

  static uint32_t appendFiniteTree(std::vector<WorldGILightBVHNodeGPU> &nodes,
      const std::vector<LightRecord> &records) {
    if (records.empty()) return kWorldGILightBVHInvalidNode;
    std::vector<uint32_t> indices(records.size());
    std::iota(indices.begin(), indices.end(), 0u);
    return emitNode(nodes, indices, records, 0u, static_cast<uint32_t>(records.size()));
  }
};
}
