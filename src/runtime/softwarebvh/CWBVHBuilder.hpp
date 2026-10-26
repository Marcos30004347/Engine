#pragma once

#include <cstdint>
#include <vector>

#include "virtualgeometry/FallbackMeshFile.hpp"
#include "virtualgeometry/VirtualGeometryBounds.hpp"

namespace softwarebvh
{

class CWBVHBuilder
{
public:
  static constexpr uint32_t INVALID_NODE = UINT32_MAX;
  static constexpr uint32_t INVALID_PRIMITIVE_INDEX = UINT32_MAX;

  struct BuildOptions
  {
    uint32_t sahBinCount = 8u;
    bool preferBoneClusters = true;
  };

  struct CanonicalNode
  {
    virtualgeometry::AABB bounds{};
    uint32_t parent = INVALID_NODE;
    uint32_t left = INVALID_NODE;
    uint32_t right = INVALID_NODE;
    uint32_t primitiveIndex = INVALID_PRIMITIVE_INDEX;
    uint32_t subtreePrimitiveCount = 0u;
    bool isLeaf = false;
  };

  struct TriBaryTransform
  {
    float row0[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float row1[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float row2[4]{0.0f, 0.0f, 0.0f, 0.0f};
  };

  struct PrimitiveBounds
  {
    virtualgeometry::AABB bounds{};
    uint32_t primitiveIndex = INVALID_PRIMITIVE_INDEX;
  };

  static constexpr uint32_t BVH8_CHILD_VALID = 1u << 0u;
  static constexpr uint32_t BVH8_CHILD_LEAF = 1u << 1u;

  struct BVH8Node
  {
    virtualgeometry::AABB bounds{};
    float childMin[8][3]{};
    float childMax[8][3]{};
    uint32_t childIndex[8] = {
        INVALID_NODE,
        INVALID_NODE,
        INVALID_NODE,
        INVALID_NODE,
        INVALID_NODE,
        INVALID_NODE,
        INVALID_NODE,
        INVALID_NODE,
    };
    uint32_t childMeta[8]{};
    uint32_t cwbvhOutputIndex = INVALID_NODE;
    uint32_t internalChildBase = INVALID_NODE;
    uint32_t leafBase = 0u;
    uint32_t validChildMask = 0u;
    uint32_t internalChildMask = 0u;
    uint32_t leafChildMask = 0u;
  };

  struct PackedCWBVHNode
  {
    uint32_t orxy[2]{};
    uint32_t oeim[2]{};
    uint32_t ntbi[2]{};
    uint32_t info[2]{};
    uint32_t qlox[2]{};
    uint32_t qloy[2]{};
    uint32_t qloz[2]{};
    uint32_t qhix[2]{};
    uint32_t qhiy[2]{};
    uint32_t qhiz[2]{};
  };

  struct CompressionResult
  {
    std::vector<PackedCWBVHNode> nodes;
    std::vector<uint32_t> leafRemap;
  };

  struct BuildResult
  {
    virtualgeometry::AABB localBounds{};
    uint32_t primitiveCount = 0u;
    uint32_t rootNodeIndex = INVALID_NODE;
    std::vector<uint32_t> primitiveRemap;
    std::vector<TriBaryTransform> triangleTransforms;
    std::vector<CanonicalNode> canonicalNodes;
    std::vector<BVH8Node> bvh8Nodes;
    std::vector<uint32_t> bvh8LeafRemap;
  };

  static BuildResult build(const virtualgeometry::FallbackMeshData &mesh);
  static BuildResult build(const virtualgeometry::FallbackMeshData &mesh, const BuildOptions &options);
  static BuildResult buildFromAabbs(const std::vector<PrimitiveBounds> &primitives);
  static BuildResult buildFromAabbs(const std::vector<PrimitiveBounds> &primitives, const BuildOptions &options);
  static void rebuildBVH8(BuildResult &build);
  static CompressionResult compressBVH8(const std::vector<BVH8Node> &bvh8Nodes, const std::vector<uint32_t> &leafRemap);
};

} // namespace softwarebvh
