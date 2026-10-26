#include "softwarebvh/CWBVHBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace softwarebvh
{

namespace
{

struct BuildPrimitive
{
  uint32_t primitiveIndex = CWBVHBuilder::INVALID_PRIMITIVE_INDEX;
  uint32_t dominantBoneIndex = UINT32_MAX;
  virtualgeometry::AABB bounds{};
  float centroid[3]{0.0f, 0.0f, 0.0f};
};

struct SAHBin
{
  virtualgeometry::AABB bounds{};
  uint32_t count = 0u;
  bool hasBounds = false;
};

struct BVH8BuildChild
{
  uint32_t canonicalNodeIndex = CWBVHBuilder::INVALID_NODE;
  bool isLeaf = false;
};

struct BVH8SplitCandidate
{
  uint32_t frontierIndex = CWBVHBuilder::INVALID_NODE;
  float costReduction = -std::numeric_limits<float>::max();
  float sourceArea = -1.0f;
};

static bool isValidAabb(const virtualgeometry::AABB &bounds)
{
  return bounds.minPoint[0] <= bounds.maxPoint[0] &&
         bounds.minPoint[1] <= bounds.maxPoint[1] &&
         bounds.minPoint[2] <= bounds.maxPoint[2];
}

static virtualgeometry::AABB unionBounds(const virtualgeometry::AABB &a, const virtualgeometry::AABB &b)
{
  return virtualgeometry::AABB::Union(a, b);
}

static uint32_t pack4xU8(const uint32_t x, const uint32_t y, const uint32_t z, const uint32_t w)
{
  return (x & 0xffu) | ((y & 0xffu) << 8u) | ((z & 0xffu) << 16u) | ((w & 0xffu) << 24u);
}

static uint32_t bitCastU32(const float value)
{
  uint32_t result = 0u;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

static uint32_t encodeSignedExponent8(const float extent)
{
  if (!(extent > 0.0f) || !std::isfinite(extent))
  {
    return 0u;
  }

  const float step = extent / 255.0f;
  int clampedExponent = std::clamp(static_cast<int>(std::ceil(std::log2(step))), -128, 127);
  while (clampedExponent < 127 && std::ceil(extent / std::exp2(static_cast<float>(clampedExponent))) > 255.0f)
  {
    ++clampedExponent;
  }
  return static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(clampedExponent)));
}

static float decodeExponentScale(const uint32_t encodedExponent)
{
  const int exponent = static_cast<int>(static_cast<int8_t>(encodedExponent & 0xffu));
  return std::exp2(static_cast<float>(exponent));
}

static uint32_t quantizeLowerBound(const float value, const float parentMin, const float scale)
{
  if (!(scale > 0.0f))
  {
    return 0u;
  }

  uint32_t q = static_cast<uint32_t>(std::clamp(std::floor((value - parentMin) / scale), 0.0f, 255.0f));
  if (q > 0u)
  {
    --q;
  }
  return q;
}

static uint32_t quantizeUpperBound(const float value, const float parentMin, const float scale)
{
  if (!(scale > 0.0f))
  {
    return 0u;
  }

  uint32_t q = static_cast<uint32_t>(std::clamp(std::ceil((value - parentMin) / scale), 0.0f, 255.0f));
  if (q < 255u)
  {
    ++q;
  }
  return q;
}

static float aabbSurfaceArea(const virtualgeometry::AABB &bounds)
{
  const float dx = std::max(0.0f, bounds.maxPoint[0] - bounds.minPoint[0]);
  const float dy = std::max(0.0f, bounds.maxPoint[1] - bounds.minPoint[1]);
  const float dz = std::max(0.0f, bounds.maxPoint[2] - bounds.minPoint[2]);
  return 2.0f * (dx * dy + dy * dz + dz * dx);
}

static virtualgeometry::AABB buildLocalBounds(const virtualgeometry::FallbackMeshData &mesh)
{
  virtualgeometry::AABB bounds{};
  bounds.minPoint[0] = mesh.boundsMin[0];
  bounds.minPoint[1] = mesh.boundsMin[1];
  bounds.minPoint[2] = mesh.boundsMin[2];
  bounds.maxPoint[0] = mesh.boundsMax[0];
  bounds.maxPoint[1] = mesh.boundsMax[1];
  bounds.maxPoint[2] = mesh.boundsMax[2];
  if (isValidAabb(bounds))
  {
    return bounds;
  }

  for (const virtualgeometry::FallbackMeshVertex &vertex : mesh.vertices)
  {
    bounds.expandBy(vertex.position);
  }
  return bounds;
}

static virtualgeometry::AABB computeTriangleBounds(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex)
{
  const uint32_t indexBase = primitiveIndex * 3u;
  if (indexBase + 2u >= mesh.indices.size())
  {
    return virtualgeometry::AABB{};
  }

  virtualgeometry::AABB bounds{};
  for (uint32_t corner = 0u; corner < 3u; ++corner)
  {
    const uint32_t vertexIndex = mesh.indices[indexBase + corner];
    if (vertexIndex >= mesh.vertices.size())
    {
      return virtualgeometry::AABB{};
    }

    bounds.expandBy(mesh.vertices[vertexIndex].position);
  }

  return bounds;
}

static void computeCentroid(const virtualgeometry::AABB &bounds, float outCentroid[3])
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    outCentroid[axis] = (bounds.minPoint[axis] + bounds.maxPoint[axis]) * 0.5f;
  }
}

static void orderBVH8Children(
    std::vector<BVH8BuildChild> &frontier,
    const std::vector<CWBVHBuilder::CanonicalNode> &canonicalNodes,
    const virtualgeometry::AABB &parentBounds)
{
  if (frontier.empty())
  {
    return;
  }

  const float parentCenter[3] = {
      (parentBounds.minPoint[0] + parentBounds.maxPoint[0]) * 0.5f,
      (parentBounds.minPoint[1] + parentBounds.maxPoint[1]) * 0.5f,
      (parentBounds.minPoint[2] + parentBounds.maxPoint[2]) * 0.5f,
  };

  float cost[8][8]{};
  for (uint32_t childIndex = 0u; childIndex < static_cast<uint32_t>(frontier.size()) && childIndex < 8u; ++childIndex)
  {
    const virtualgeometry::AABB &childBounds = canonicalNodes[frontier[childIndex].canonicalNodeIndex].bounds;
    const float childCenter[3] = {
        (childBounds.minPoint[0] + childBounds.maxPoint[0]) * 0.5f,
        (childBounds.minPoint[1] + childBounds.maxPoint[1]) * 0.5f,
        (childBounds.minPoint[2] + childBounds.maxPoint[2]) * 0.5f,
    };

    for (uint32_t slot = 0u; slot < 8u; ++slot)
    {
      const float sx = (slot & 4u) != 0u ? 1.0f : -1.0f;
      const float sy = (slot & 2u) != 0u ? 1.0f : -1.0f;
      const float sz = (slot & 1u) != 0u ? 1.0f : -1.0f;
      const float vx = childCenter[0] - parentCenter[0];
      const float vy = childCenter[1] - parentCenter[1];
      const float vz = childCenter[2] - parentCenter[2];
      cost[childIndex][slot] = -(vx * sx + vy * sy + vz * sz);
    }
  }

  int assignment[8];
  bool slotFilled[8]{};
  std::fill(std::begin(assignment), std::end(assignment), -1);

  for (uint32_t iteration = 0u; iteration < frontier.size() && iteration < 8u; ++iteration)
  {
    int bestChild = -1;
    int bestSlot = -1;
    float bestCost = 0.0f;
    bool hasBest = false;
    for (uint32_t childIndex = 0u; childIndex < frontier.size() && childIndex < 8u; ++childIndex)
    {
      if (assignment[childIndex] >= 0)
      {
        continue;
      }

      for (uint32_t slot = 0u; slot < 8u; ++slot)
      {
        if (slotFilled[slot])
        {
          continue;
        }

        if (!hasBest || cost[childIndex][slot] < bestCost)
        {
          bestChild = static_cast<int>(childIndex);
          bestSlot = static_cast<int>(slot);
          bestCost = cost[childIndex][slot];
          hasBest = true;
        }
      }
    }

    if (bestChild < 0 || bestSlot < 0)
    {
      break;
    }

    assignment[bestChild] = bestSlot;
    slotFilled[bestSlot] = true;
  }

  std::vector<BVH8BuildChild> ordered(8u);
  for (uint32_t childIndex = 0u; childIndex < static_cast<uint32_t>(frontier.size()) && childIndex < 8u; ++childIndex)
  {
    if (assignment[childIndex] >= 0)
    {
      ordered[static_cast<uint32_t>(assignment[childIndex])] = frontier[childIndex];
    }
  }
  frontier = std::move(ordered);
}

static BVH8SplitCandidate chooseBVH8SplitCandidate(
    const std::vector<BVH8BuildChild> &frontier,
    const std::vector<CWBVHBuilder::CanonicalNode> &canonicalNodes)
{
  BVH8SplitCandidate best{};

  for (uint32_t index = 0u; index < static_cast<uint32_t>(frontier.size()); ++index)
  {
    if (frontier[index].isLeaf)
    {
      continue;
    }

    const CWBVHBuilder::CanonicalNode &node = canonicalNodes[frontier[index].canonicalNodeIndex];
    if (node.left == CWBVHBuilder::INVALID_NODE || node.right == CWBVHBuilder::INVALID_NODE)
    {
      continue;
    }

    const CWBVHBuilder::CanonicalNode &left = canonicalNodes[node.left];
    const CWBVHBuilder::CanonicalNode &right = canonicalNodes[node.right];
    const float sourceArea = aabbSurfaceArea(node.bounds);
    const float sourceCost = sourceArea * static_cast<float>(node.subtreePrimitiveCount);
    const float splitCost =
        aabbSurfaceArea(left.bounds) * static_cast<float>(left.subtreePrimitiveCount) +
        aabbSurfaceArea(right.bounds) * static_cast<float>(right.subtreePrimitiveCount);
    const float costReduction = sourceCost - splitCost;

    if (best.frontierIndex == CWBVHBuilder::INVALID_NODE ||
        costReduction > best.costReduction ||
        (costReduction == best.costReduction && sourceArea > best.sourceArea))
    {
      best.frontierIndex = index;
      best.costReduction = costReduction;
      best.sourceArea = sourceArea;
    }
  }

  return best;
}

static void emitBVH8Recursive(
    const std::vector<CWBVHBuilder::CanonicalNode> &canonicalNodes,
    const uint32_t canonicalNodeIndex,
    const uint32_t bvh8NodeIndex,
    std::vector<CWBVHBuilder::BVH8Node> &outNodes,
    std::vector<uint32_t> &outLeafRemap)
{
  if (canonicalNodeIndex >= canonicalNodes.size())
  {
    return;
  }

  if (bvh8NodeIndex >= outNodes.size())
  {
    outNodes.resize(bvh8NodeIndex + 1u);
  }

  const CWBVHBuilder::CanonicalNode &sourceNode = canonicalNodes[canonicalNodeIndex];
  std::vector<BVH8BuildChild> frontier;
  frontier.reserve(8u);

  if (sourceNode.isLeaf)
  {
    frontier.push_back(BVH8BuildChild{
        .canonicalNodeIndex = canonicalNodeIndex,
        .isLeaf = true,
    });
  }
  else
  {
    if (sourceNode.left != CWBVHBuilder::INVALID_NODE)
    {
      frontier.push_back(BVH8BuildChild{
          .canonicalNodeIndex = sourceNode.left,
          .isLeaf = canonicalNodes[sourceNode.left].isLeaf,
      });
    }
    if (sourceNode.right != CWBVHBuilder::INVALID_NODE)
    {
      frontier.push_back(BVH8BuildChild{
          .canonicalNodeIndex = sourceNode.right,
          .isLeaf = canonicalNodes[sourceNode.right].isLeaf,
      });
    }
  }

  while (frontier.size() < 8u)
  {
    uint32_t splitIndex = chooseBVH8SplitCandidate(frontier, canonicalNodes).frontierIndex;

    if (splitIndex == CWBVHBuilder::INVALID_NODE)
    {
      break;
    }

    const BVH8BuildChild childToSplit = frontier[splitIndex];
    const CWBVHBuilder::CanonicalNode &splitNode = canonicalNodes[childToSplit.canonicalNodeIndex];
    frontier.erase(frontier.begin() + splitIndex);

    if (splitNode.left != CWBVHBuilder::INVALID_NODE)
    {
      frontier.insert(
          frontier.begin() + splitIndex,
          BVH8BuildChild{
              .canonicalNodeIndex = splitNode.left,
              .isLeaf = canonicalNodes[splitNode.left].isLeaf,
          });
      ++splitIndex;
    }
    if (splitNode.right != CWBVHBuilder::INVALID_NODE)
    {
      frontier.insert(
          frontier.begin() + splitIndex,
          BVH8BuildChild{
              .canonicalNodeIndex = splitNode.right,
              .isLeaf = canonicalNodes[splitNode.right].isLeaf,
          });
    }
  }

  orderBVH8Children(frontier, canonicalNodes, sourceNode.bounds);

  const uint32_t internalBaseIndex = static_cast<uint32_t>(outNodes.size());
  const uint32_t leafBaseIndex = static_cast<uint32_t>(outLeafRemap.size());
  std::vector<BVH8BuildChild> internalChildren;
  internalChildren.reserve(frontier.size());

  CWBVHBuilder::BVH8Node node{};
  node.bounds = sourceNode.bounds;
  node.cwbvhOutputIndex = bvh8NodeIndex;
  node.internalChildBase = internalBaseIndex;
  node.leafBase = leafBaseIndex;

  uint32_t leafOffset = 0u;
  for (uint32_t childSlot = 0u; childSlot < static_cast<uint32_t>(frontier.size()) && childSlot < 8u; ++childSlot)
  {
    const BVH8BuildChild &child = frontier[childSlot];
    if (child.canonicalNodeIndex == CWBVHBuilder::INVALID_NODE)
    {
      continue;
    }

    const CWBVHBuilder::CanonicalNode &childNode = canonicalNodes[child.canonicalNodeIndex];
    node.validChildMask |= 1u << childSlot;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      node.childMin[childSlot][axis] = childNode.bounds.minPoint[axis];
      node.childMax[childSlot][axis] = childNode.bounds.maxPoint[axis];
    }

    if (!child.isLeaf)
    {
      node.internalChildMask |= 1u << childSlot;
      node.childMeta[childSlot] = CWBVHBuilder::BVH8_CHILD_VALID;
      node.childIndex[childSlot] = internalBaseIndex + static_cast<uint32_t>(internalChildren.size());
      internalChildren.push_back(child);
    }
    else
    {
      node.leafChildMask |= 1u << childSlot;
      node.childMeta[childSlot] = CWBVHBuilder::BVH8_CHILD_VALID | CWBVHBuilder::BVH8_CHILD_LEAF;
      node.childIndex[childSlot] = leafBaseIndex + leafOffset;
      outLeafRemap.push_back(childNode.primitiveIndex);
      ++leafOffset;
    }
  }

  outNodes[bvh8NodeIndex] = node;

  if (!internalChildren.empty())
  {
    outNodes.resize(outNodes.size() + internalChildren.size());
    for (uint32_t internalIndex = 0u; internalIndex < static_cast<uint32_t>(internalChildren.size()); ++internalIndex)
    {
      emitBVH8Recursive(
          canonicalNodes,
          internalChildren[internalIndex].canonicalNodeIndex,
          internalBaseIndex + internalIndex,
          outNodes,
          outLeafRemap);
    }
  }
}

static uint32_t computeDominantBoneIndex(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex)
{
  const uint32_t indexBase = primitiveIndex * 3u;
  if (indexBase + 2u >= mesh.indices.size())
  {
    return UINT32_MAX;
  }

  struct BoneWeightSum
  {
    uint32_t boneIndex = UINT32_MAX;
    float weight = 0.0f;
  };

  BoneWeightSum best{};
  std::vector<BoneWeightSum> sums;
  sums.reserve(12u);

  for (uint32_t corner = 0u; corner < 3u; ++corner)
  {
    const uint32_t vertexIndex = mesh.indices[indexBase + corner];
    if (vertexIndex >= mesh.vertices.size())
    {
      return UINT32_MAX;
    }

    const virtualgeometry::FallbackMeshVertex &vertex = mesh.vertices[vertexIndex];
    for (uint32_t influenceIndex = 0u; influenceIndex < 4u; ++influenceIndex)
    {
      const float weight = vertex.boneWeights[influenceIndex];
      if (weight <= 0.0f)
      {
        continue;
      }

      const uint32_t boneIndex = vertex.boneIndices[influenceIndex];
      auto existing = std::find_if(
          sums.begin(),
          sums.end(),
          [boneIndex](const BoneWeightSum &sum)
          {
            return sum.boneIndex == boneIndex;
          });
      if (existing == sums.end())
      {
        sums.push_back(BoneWeightSum{
            .boneIndex = boneIndex,
            .weight = weight,
        });
        existing = std::prev(sums.end());
      }
      else
      {
        existing->weight += weight;
      }

      if (existing->weight > best.weight || (existing->weight == best.weight && existing->boneIndex < best.boneIndex))
      {
        best = *existing;
      }
    }
  }

  return best.weight > 0.0f ? best.boneIndex : UINT32_MAX;
}

static bool buildTriangleTransform(
    const virtualgeometry::FallbackMeshData &mesh,
    const uint32_t primitiveIndex,
    CWBVHBuilder::TriBaryTransform &outTransform)
{
  const uint32_t indexBase = primitiveIndex * 3u;
  if (indexBase + 2u >= mesh.indices.size())
  {
    return false;
  }

  const uint32_t i0 = mesh.indices[indexBase + 0u];
  const uint32_t i1 = mesh.indices[indexBase + 1u];
  const uint32_t i2 = mesh.indices[indexBase + 2u];
  if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
  {
    return false;
  }

  const virtualgeometry::FallbackMeshVertex &v0 = mesh.vertices[i0];
  const virtualgeometry::FallbackMeshVertex &v1 = mesh.vertices[i1];
  const virtualgeometry::FallbackMeshVertex &v2 = mesh.vertices[i2];

  const float e1[3] = {
      v1.position[0] - v0.position[0],
      v1.position[1] - v0.position[1],
      v1.position[2] - v0.position[2],
  };
  const float e2[3] = {
      v2.position[0] - v0.position[0],
      v2.position[1] - v0.position[1],
      v2.position[2] - v0.position[2],
  };
  const float n[3] = {
      e1[1] * e2[2] - e1[2] * e2[1],
      e1[2] * e2[0] - e1[0] * e2[2],
      e1[0] * e2[1] - e1[1] * e2[0],
  };

  const float det =
      e1[0] * (e2[1] * n[2] - e2[2] * n[1]) -
      e2[0] * (e1[1] * n[2] - e1[2] * n[1]) +
      n[0] * (e1[1] * e2[2] - e1[2] * e2[1]);
  if (std::fabs(det) <= 1e-20f)
  {
    return false;
  }

  const float invDet = 1.0f / det;
  const float row0[3] = {
      (e2[1] * n[2] - e2[2] * n[1]) * invDet,
      (e2[2] * n[0] - e2[0] * n[2]) * invDet,
      (e2[0] * n[1] - e2[1] * n[0]) * invDet,
  };
  const float row1[3] = {
      (n[1] * e1[2] - n[2] * e1[1]) * invDet,
      (n[2] * e1[0] - n[0] * e1[2]) * invDet,
      (n[0] * e1[1] - n[1] * e1[0]) * invDet,
  };
  const float row2[3] = {
      (e1[1] * e2[2] - e1[2] * e2[1]) * invDet,
      (e1[2] * e2[0] - e1[0] * e2[2]) * invDet,
      (e1[0] * e2[1] - e1[1] * e2[0]) * invDet,
  };

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    outTransform.row0[axis] = row0[axis];
    outTransform.row1[axis] = row1[axis];
    outTransform.row2[axis] = row2[axis];
  }
  outTransform.row0[3] =
      -(row0[0] * v0.position[0] + row0[1] * v0.position[1] + row0[2] * v0.position[2]);
  outTransform.row1[3] =
      -(row1[0] * v0.position[0] + row1[1] * v0.position[1] + row1[2] * v0.position[2]);
  outTransform.row2[3] =
      -(row2[0] * v0.position[0] + row2[1] * v0.position[1] + row2[2] * v0.position[2]);
  return true;
}

class BuilderState
{
public:
  explicit BuilderState(const CWBVHBuilder::BuildOptions &options) : options_(options)
  {
    if (options_.sahBinCount < 2u)
    {
      throw std::runtime_error("CWBVHBuilder requires at least 2 SAH bins");
    }
  }

  CWBVHBuilder::BuildResult build(const virtualgeometry::FallbackMeshData &mesh)
  {
    CWBVHBuilder::BuildResult result{};
    result.localBounds = buildLocalBounds(mesh);
    result.primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
    result.primitiveRemap.reserve(result.primitiveCount);
    result.triangleTransforms.resize(result.primitiveCount);
    for (uint32_t primitiveIndex = 0u; primitiveIndex < result.primitiveCount; ++primitiveIndex)
    {
      result.primitiveRemap.push_back(primitiveIndex);
      buildTriangleTransform(mesh, primitiveIndex, result.triangleTransforms[primitiveIndex]);
    }

    std::vector<BuildPrimitive> primitives;
    primitives.reserve(result.primitiveCount);
    for (uint32_t primitiveIndex = 0u; primitiveIndex < result.primitiveCount; ++primitiveIndex)
    {
      const virtualgeometry::AABB primitiveBounds = computeTriangleBounds(mesh, primitiveIndex);
      if (!isValidAabb(primitiveBounds))
      {
        continue;
      }

      BuildPrimitive primitive{};
      primitive.primitiveIndex = primitiveIndex;
      primitive.dominantBoneIndex = computeDominantBoneIndex(mesh, primitiveIndex);
      primitive.bounds = primitiveBounds;
      computeCentroid(primitiveBounds, primitive.centroid);
      primitives.push_back(primitive);
    }

    if (primitives.empty())
    {
      return result;
    }

    if (options_.preferBoneClusters)
    {
      std::stable_sort(
          primitives.begin(),
          primitives.end(),
          [](const BuildPrimitive &a, const BuildPrimitive &b)
          {
            if (a.dominantBoneIndex != b.dominantBoneIndex)
            {
              return a.dominantBoneIndex < b.dominantBoneIndex;
            }
            if (a.centroid[0] != b.centroid[0])
            {
              return a.centroid[0] < b.centroid[0];
            }
            if (a.centroid[1] != b.centroid[1])
            {
              return a.centroid[1] < b.centroid[1];
            }
            return a.centroid[2] < b.centroid[2];
          });
    }

    result.canonicalNodes.reserve(primitives.size() * 2u - 1u);
    result.rootNodeIndex = buildCanonicalNodeRecursive(result.canonicalNodes, primitives, 0u, static_cast<uint32_t>(primitives.size()), CWBVHBuilder::INVALID_NODE);
    CWBVHBuilder::rebuildBVH8(result);
    return result;
  }

  CWBVHBuilder::BuildResult buildFromAabbs(const std::vector<CWBVHBuilder::PrimitiveBounds> &inputPrimitives)
  {
    CWBVHBuilder::BuildResult result{};
    result.primitiveCount = static_cast<uint32_t>(inputPrimitives.size());
    result.primitiveRemap.reserve(result.primitiveCount);

    std::vector<BuildPrimitive> primitives;
    primitives.reserve(result.primitiveCount);
    for (const CWBVHBuilder::PrimitiveBounds &input : inputPrimitives)
    {
      if (input.primitiveIndex == CWBVHBuilder::INVALID_PRIMITIVE_INDEX || !isValidAabb(input.bounds))
      {
        continue;
      }

      if (primitives.empty())
      {
        result.localBounds = input.bounds;
      }
      else
      {
        result.localBounds = unionBounds(result.localBounds, input.bounds);
      }

      result.primitiveRemap.push_back(input.primitiveIndex);
      BuildPrimitive primitive{};
      primitive.primitiveIndex = static_cast<uint32_t>(result.primitiveRemap.size() - 1u);
      primitive.bounds = input.bounds;
      computeCentroid(input.bounds, primitive.centroid);
      primitives.push_back(primitive);
    }

    result.primitiveCount = static_cast<uint32_t>(result.primitiveRemap.size());
    if (primitives.empty())
    {
      result.localBounds = virtualgeometry::AABB{};
      return result;
    }

    result.canonicalNodes.reserve(primitives.size() * 2u - 1u);
    result.rootNodeIndex = buildCanonicalNodeRecursive(result.canonicalNodes, primitives, 0u, static_cast<uint32_t>(primitives.size()), CWBVHBuilder::INVALID_NODE);
    CWBVHBuilder::rebuildBVH8(result);
    return result;
  }

private:
  uint32_t buildCanonicalNodeRecursive(
      std::vector<CWBVHBuilder::CanonicalNode> &nodes,
      std::vector<BuildPrimitive> &primitives,
      const uint32_t begin,
      const uint32_t end,
      const uint32_t parent)
  {
    if (begin >= end)
    {
      return CWBVHBuilder::INVALID_NODE;
    }

    if (end - begin == 1u)
    {
      CWBVHBuilder::CanonicalNode node{};
      node.parent = parent;
      node.bounds = primitives[begin].bounds;
      node.primitiveIndex = primitives[begin].primitiveIndex;
      node.subtreePrimitiveCount = 1u;
      node.isLeaf = true;
      const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
      nodes.push_back(node);
      return nodeIndex;
    }

    virtualgeometry::AABB centroidBounds{};
    centroidBounds.minPoint[0] = primitives[begin].centroid[0];
    centroidBounds.minPoint[1] = primitives[begin].centroid[1];
    centroidBounds.minPoint[2] = primitives[begin].centroid[2];
    centroidBounds.maxPoint[0] = primitives[begin].centroid[0];
    centroidBounds.maxPoint[1] = primitives[begin].centroid[1];
    centroidBounds.maxPoint[2] = primitives[begin].centroid[2];

    virtualgeometry::AABB primitiveBounds = primitives[begin].bounds;
    for (uint32_t index = begin + 1u; index < end; ++index)
    {
      primitiveBounds = unionBounds(primitiveBounds, primitives[index].bounds);
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        centroidBounds.minPoint[axis] = std::min(centroidBounds.minPoint[axis], primitives[index].centroid[axis]);
        centroidBounds.maxPoint[axis] = std::max(centroidBounds.maxPoint[axis], primitives[index].centroid[axis]);
      }
    }

    uint32_t bestAxis = CWBVHBuilder::INVALID_NODE;
    uint32_t bestSplit = 0u;
    float bestCost = primitiveBounds.GetSurfaceArea() * static_cast<float>(end - begin);

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float minCentroid = centroidBounds.minPoint[axis];
      const float maxCentroid = centroidBounds.maxPoint[axis];
      const float extent = maxCentroid - minCentroid;
      if (extent <= 1e-6f)
      {
        continue;
      }

      std::vector<SAHBin> bins(options_.sahBinCount);
      for (uint32_t index = begin; index < end; ++index)
      {
        const float normalized = (primitives[index].centroid[axis] - minCentroid) / extent;
        const uint32_t binIndex = std::min<uint32_t>(
            options_.sahBinCount - 1u,
            static_cast<uint32_t>(normalized * static_cast<float>(options_.sahBinCount)));
        SAHBin &bin = bins[binIndex];
        bin.bounds = bin.hasBounds ? unionBounds(bin.bounds, primitives[index].bounds) : primitives[index].bounds;
        bin.count += 1u;
        bin.hasBounds = true;
      }

      std::vector<virtualgeometry::AABB> leftBounds(options_.sahBinCount - 1u);
      std::vector<virtualgeometry::AABB> rightBounds(options_.sahBinCount - 1u);
      std::vector<uint32_t> leftCounts(options_.sahBinCount - 1u, 0u);
      std::vector<uint32_t> rightCounts(options_.sahBinCount - 1u, 0u);

      virtualgeometry::AABB runningLeftBounds{};
      uint32_t runningLeftCount = 0u;
      bool hasRunningLeftBounds = false;
      for (uint32_t split = 0u; split < options_.sahBinCount - 1u; ++split)
      {
        if (bins[split].hasBounds)
        {
          runningLeftBounds = hasRunningLeftBounds ? unionBounds(runningLeftBounds, bins[split].bounds) : bins[split].bounds;
          hasRunningLeftBounds = true;
        }
        runningLeftCount += bins[split].count;
        leftBounds[split] = runningLeftBounds;
        leftCounts[split] = runningLeftCount;
      }

      virtualgeometry::AABB runningRightBounds{};
      uint32_t runningRightCount = 0u;
      bool hasRunningRightBounds = false;
      for (uint32_t split = options_.sahBinCount - 1u; split-- > 1u;)
      {
        if (bins[split].hasBounds)
        {
          runningRightBounds = hasRunningRightBounds ? unionBounds(runningRightBounds, bins[split].bounds) : bins[split].bounds;
          hasRunningRightBounds = true;
        }
        runningRightCount += bins[split].count;
        rightBounds[split - 1u] = runningRightBounds;
        rightCounts[split - 1u] = runningRightCount;
      }

      if (bins[1].hasBounds)
      {
        runningRightBounds = hasRunningRightBounds ? unionBounds(runningRightBounds, bins[1].bounds) : bins[1].bounds;
      }
      runningRightCount += bins[1].count;
      rightBounds[0] = runningRightBounds;
      rightCounts[0] = runningRightCount;

      for (uint32_t split = 0u; split < options_.sahBinCount - 1u; ++split)
      {
        if (leftCounts[split] == 0u || rightCounts[split] == 0u)
        {
          continue;
        }

        const float cost =
            leftBounds[split].GetSurfaceArea() * static_cast<float>(leftCounts[split]) +
            rightBounds[split].GetSurfaceArea() * static_cast<float>(rightCounts[split]);
        if (cost < bestCost)
        {
          bestCost = cost;
          bestAxis = axis;
          bestSplit = split;
        }
      }
    }

    uint32_t mid = begin + ((end - begin) >> 1u);
    if (bestAxis != CWBVHBuilder::INVALID_NODE)
    {
      const float centroidMin = centroidBounds.minPoint[bestAxis];
      const float centroidMax = centroidBounds.maxPoint[bestAxis];
      const float extent = centroidMax - centroidMin;
      auto midIt = std::partition(
          primitives.begin() + begin,
          primitives.begin() + end,
          [&](const BuildPrimitive &primitive)
          {
            const float normalized = extent <= 1e-6f ? 0.0f : (primitive.centroid[bestAxis] - centroidMin) / extent;
            const uint32_t binIndex = std::min<uint32_t>(
                options_.sahBinCount - 1u,
                static_cast<uint32_t>(normalized * static_cast<float>(options_.sahBinCount)));
            return binIndex <= bestSplit;
          });
      mid = static_cast<uint32_t>(midIt - primitives.begin());
    }

    if (options_.preferBoneClusters &&
        primitives[begin].dominantBoneIndex != primitives[end - 1u].dominantBoneIndex)
    {
      const uint32_t targetMid = begin + ((end - begin) >> 1u);
      uint32_t bestBoundary = begin + 1u;
      uint32_t bestDistance = end - begin;
      for (uint32_t index = begin + 1u; index < end; ++index)
      {
        if (primitives[index - 1u].dominantBoneIndex == primitives[index].dominantBoneIndex)
        {
          continue;
        }

        const uint32_t distance = index > targetMid ? index - targetMid : targetMid - index;
        if (distance < bestDistance)
        {
          bestDistance = distance;
          bestBoundary = index;
        }
      }

      if (bestBoundary > begin && bestBoundary < end)
      {
        mid = bestBoundary;
      }
    }

    if (mid <= begin || mid >= end)
    {
      uint32_t splitAxis = 0u;
      float maxExtent = centroidBounds.maxPoint[0] - centroidBounds.minPoint[0];
      for (uint32_t axis = 1u; axis < 3u; ++axis)
      {
        const float extent = centroidBounds.maxPoint[axis] - centroidBounds.minPoint[axis];
        if (extent > maxExtent)
        {
          maxExtent = extent;
          splitAxis = axis;
        }
      }

      std::nth_element(
          primitives.begin() + begin,
          primitives.begin() + mid,
          primitives.begin() + end,
          [splitAxis](const BuildPrimitive &a, const BuildPrimitive &b)
          {
            return a.centroid[splitAxis] < b.centroid[splitAxis];
          });
    }

    const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
    nodes.push_back(CWBVHBuilder::CanonicalNode{});
    nodes[nodeIndex].parent = parent;

    const uint32_t left = buildCanonicalNodeRecursive(nodes, primitives, begin, mid, nodeIndex);
    const uint32_t right = buildCanonicalNodeRecursive(nodes, primitives, mid, end, nodeIndex);

    CWBVHBuilder::CanonicalNode &node = nodes[nodeIndex];
    node.left = left;
    node.right = right;
    if (left != CWBVHBuilder::INVALID_NODE)
    {
      node.bounds = nodes[left].bounds;
      node.subtreePrimitiveCount += nodes[left].subtreePrimitiveCount;
    }
    if (right != CWBVHBuilder::INVALID_NODE)
    {
      node.bounds = left == CWBVHBuilder::INVALID_NODE ? nodes[right].bounds : unionBounds(node.bounds, nodes[right].bounds);
      node.subtreePrimitiveCount += nodes[right].subtreePrimitiveCount;
    }

    return nodeIndex;
  }

  CWBVHBuilder::BuildOptions options_{};
};

} // namespace

CWBVHBuilder::BuildResult CWBVHBuilder::build(const virtualgeometry::FallbackMeshData &mesh)
{
  return build(mesh, BuildOptions{});
}

CWBVHBuilder::BuildResult CWBVHBuilder::build(const virtualgeometry::FallbackMeshData &mesh, const BuildOptions &options)
{
  return BuilderState(options).build(mesh);
}

CWBVHBuilder::BuildResult CWBVHBuilder::buildFromAabbs(const std::vector<PrimitiveBounds> &primitives)
{
  return buildFromAabbs(primitives, BuildOptions{});
}

CWBVHBuilder::BuildResult CWBVHBuilder::buildFromAabbs(const std::vector<PrimitiveBounds> &primitives, const BuildOptions &options)
{
  CWBVHBuilder::BuildOptions adjustedOptions = options;
  adjustedOptions.preferBoneClusters = false;
  return BuilderState(adjustedOptions).buildFromAabbs(primitives);
}

void CWBVHBuilder::rebuildBVH8(BuildResult &build)
{
  build.bvh8Nodes.clear();
  build.bvh8LeafRemap.clear();
  if (build.rootNodeIndex == INVALID_NODE || build.rootNodeIndex >= build.canonicalNodes.size())
  {
    return;
  }

  build.bvh8Nodes.push_back(BVH8Node{});
  emitBVH8Recursive(build.canonicalNodes, build.rootNodeIndex, 0u, build.bvh8Nodes, build.bvh8LeafRemap);
}

CWBVHBuilder::CompressionResult CWBVHBuilder::compressBVH8(const std::vector<BVH8Node> &bvh8Nodes, const std::vector<uint32_t> &leafRemap)
{
  CompressionResult result{};
  result.leafRemap = leafRemap;
  if (bvh8Nodes.empty())
  {
    return result;
  }

  result.nodes.resize(bvh8Nodes.size());
  for (const BVH8Node &sourceNode : bvh8Nodes)
  {
    if (sourceNode.cwbvhOutputIndex == INVALID_NODE || sourceNode.cwbvhOutputIndex >= result.nodes.size())
    {
      throw std::runtime_error("CWBVHBuilder BVH8 node has invalid CWBVH output index");
    }

    const float parentMinX = sourceNode.bounds.minPoint[0];
    const float parentMinY = sourceNode.bounds.minPoint[1];
    const float parentMinZ = sourceNode.bounds.minPoint[2];
    const uint32_t sxExp = encodeSignedExponent8(sourceNode.bounds.maxPoint[0] - sourceNode.bounds.minPoint[0]);
    const uint32_t syExp = encodeSignedExponent8(sourceNode.bounds.maxPoint[1] - sourceNode.bounds.minPoint[1]);
    const uint32_t szExp = encodeSignedExponent8(sourceNode.bounds.maxPoint[2] - sourceNode.bounds.minPoint[2]);
    const float scaleX = decodeExponentScale(sxExp);
    const float scaleY = decodeExponentScale(syExp);
    const float scaleZ = decodeExponentScale(szExp);

    PackedCWBVHNode node{};
    uint32_t imask = 0u;
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_VALID) == 0u)
      {
        continue;
      }

      const uint32_t arrayIndex = childSlot >> 2u;
      const uint32_t bitOffset = (childSlot & 3u) * 8u;
      node.qlox[arrayIndex] |= quantizeLowerBound(sourceNode.childMin[childSlot][0], parentMinX, scaleX) << bitOffset;
      node.qloy[arrayIndex] |= quantizeLowerBound(sourceNode.childMin[childSlot][1], parentMinY, scaleY) << bitOffset;
      node.qloz[arrayIndex] |= quantizeLowerBound(sourceNode.childMin[childSlot][2], parentMinZ, scaleZ) << bitOffset;
      node.qhix[arrayIndex] |= quantizeUpperBound(sourceNode.childMax[childSlot][0], parentMinX, scaleX) << bitOffset;
      node.qhiy[arrayIndex] |= quantizeUpperBound(sourceNode.childMax[childSlot][1], parentMinY, scaleY) << bitOffset;
      node.qhiz[arrayIndex] |= quantizeUpperBound(sourceNode.childMax[childSlot][2], parentMinZ, scaleZ) << bitOffset;

      if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_LEAF) == 0u)
      {
        imask |= 1u << childSlot;
        node.info[arrayIndex] |= ((1u << 5u) | ((childSlot + 24u) & 0x1fu)) << bitOffset;
      }
      else
      {
        if (sourceNode.childIndex[childSlot] < sourceNode.leafBase)
        {
          throw std::runtime_error("CWBVHBuilder BVH8 leaf child index precedes leaf base");
        }
        const uint32_t leafOffset = sourceNode.childIndex[childSlot] - sourceNode.leafBase;
        if (leafOffset >= 24u)
        {
          throw std::runtime_error("CWBVHBuilder BVH8 leaf offset exceeds CWBVH leaf encoding range");
        }
        if (sourceNode.childIndex[childSlot] >= result.leafRemap.size())
        {
          throw std::runtime_error("CWBVHBuilder BVH8 leaf child index exceeds leaf remap");
        }
        node.info[arrayIndex] |= ((1u << 5u) | (leafOffset & 0x1fu)) << bitOffset;
      }
    }

    node.orxy[0] = bitCastU32(parentMinX);
    node.orxy[1] = bitCastU32(parentMinY);
    node.oeim[0] = bitCastU32(parentMinZ);
    node.oeim[1] = pack4xU8(sxExp, syExp, szExp, imask);
    node.ntbi[0] = sourceNode.internalChildBase == INVALID_NODE ? 0u : sourceNode.internalChildBase;
    node.ntbi[1] = sourceNode.leafBase;
    result.nodes[sourceNode.cwbvhOutputIndex] = node;
  }
  return result;
}

} // namespace softwarebvh
