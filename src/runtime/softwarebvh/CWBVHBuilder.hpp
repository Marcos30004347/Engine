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
    enum class SplitHeuristic : uint32_t
    {
      BinnedSAH = 0u,
      ExactSAH = 1u,
    };

    uint32_t sahBinCount = 8u;
    bool preferBoneClusters = true;
    SplitHeuristic splitHeuristic = SplitHeuristic::BinnedSAH;
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

  struct CWBVHNodeMetadata
  {
    uint32_t flags = 0u;
    uint32_t rotationIndex = 0u;
    uint32_t quantizedFlags = 0u;
    uint32_t padding = 0u;
  };

  struct DobbProjectionProxy
  {
    float minProjection[13]{};
    float maxProjection[13]{};
    uint32_t valid = 0u;
  };

  struct CompressionOptions
  {
    bool buildDobbNodes = true;
    float dobbAcceptanceAlpha = 0.98f;
    uint32_t dobbMaxConvertedLevels = 6u;
    uint32_t dobbRotationSet = 0u;
    const std::vector<uint32_t> *primitiveDobbRotationIndices = nullptr;
    const std::vector<DobbProjectionProxy> *primitiveDobbProxies = nullptr;
  };

  struct CompressionStats
  {
    uint32_t nodeCount = 0u;
    uint32_t validChildCount = 0u;
    uint32_t internalChildCount = 0u;
    uint32_t leafChildCount = 0u;
    uint32_t conservativeChildCount = 0u;
    uint32_t nonConservativeChildCount = 0u;
    uint32_t expandedChildCount = 0u;
    uint32_t saturatedLowerBounds = 0u;
    uint32_t saturatedUpperBounds = 0u;
    double sourceSurfaceAreaSum = 0.0;
    double decodedSurfaceAreaSum = 0.0;
    float maxAxisExpansion = 0.0f;
    float maxSurfaceAreaExpansion = 1.0f;
  };

  struct CompressionResult
  {
    std::vector<PackedCWBVHNode> nodes;
    std::vector<PackedCWBVHNode> dobbNodes;
    std::vector<CWBVHNodeMetadata> nodeMetadata;
    std::vector<uint32_t> leafRemap;
    CompressionStats stats;
  };

  struct BuildResult
  {
    virtualgeometry::AABB localBounds{};
    uint32_t primitiveCount = 0u;
    uint32_t rootNodeIndex = INVALID_NODE;
    std::vector<uint32_t> primitiveRemap;
    std::vector<uint32_t> primitiveDobbRotationIndices;
    std::vector<DobbProjectionProxy> primitiveDobbProxies;
    std::vector<TriBaryTransform> triangleTransforms;
    std::vector<CanonicalNode> canonicalNodes;
    std::vector<BVH8Node> bvh8Nodes;
    std::vector<uint32_t> bvh8LeafRemap;
  };

  static BuildResult build(const virtualgeometry::FallbackMeshData &mesh);
  static BuildResult build(const virtualgeometry::FallbackMeshData &mesh, const BuildOptions &options);
  static BuildResult buildOffline(const virtualgeometry::FallbackMeshData &mesh);
  static BuildResult buildFromAabbs(const std::vector<PrimitiveBounds> &primitives);
  static BuildResult buildFromAabbs(const std::vector<PrimitiveBounds> &primitives, const BuildOptions &options);
  static void rebuildBVH8(BuildResult &build);
  static CompressionResult compressBVH8(const std::vector<BVH8Node> &bvh8Nodes, const std::vector<uint32_t> &leafRemap);
  static CompressionResult compressBVH8(const std::vector<BVH8Node> &bvh8Nodes, const std::vector<uint32_t> &leafRemap, const CompressionOptions &options);
};

} // namespace softwarebvh
