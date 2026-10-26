#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryBounds.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace softwarebvh
{

class SoftwareTLASManager
{
public:
  static constexpr uint32_t INVALID_NODE = UINT32_MAX;
  static constexpr uint32_t INVALID_INSTANCE_INDEX = UINT32_MAX;
  static constexpr uint32_t INVALID_BLAS_HANDLE = UINT32_MAX;
  static constexpr uint32_t INSTANCE_FLAG_DYNAMIC = 1u << 0u;
  static constexpr uint32_t NODE_FLAG_LEAF = 1u << 0u;
  static constexpr uint32_t NODE_DEPTH_SHIFT = 8u;

  struct Settings
  {
    uint32_t maxInstances = 16384u;
    bool frameLocalBuffers = true;
    uint32_t workgroupSize = 64u;
    bool partialRebuildEnabled = true;
    uint32_t partialRebuildMinLeafCount = 8u;
    float partialRebuildDirtyRatioThreshold = 0.5f;
    float partialRebuildSahGrowthThreshold = 1.35f;
    uint32_t bvh4SahBinCount = 8u;
    float bvh4RefitSahGrowthThreshold = 1.25f;
    bool buildDobbCwbvhNodes = true;
    float dobbAcceptanceAlpha = 0.98f;
    uint32_t dobbMaxConvertedLevels = UINT32_MAX;
    uint32_t dobbRotationSet = 0u;
  };

  struct InstanceBuildInput
  {
    virtualgeometry::InstanceId instanceId = virtualgeometry::INVALID_INSTANCE_ID;
    uint32_t blasHandle = INVALID_BLAS_HANDLE;
    uint32_t flags = 0u;
    math::Mat4f transform = math::Mat4f::identity();
    math::Mat4f previousTransform = math::Mat4f::identity();
    virtualgeometry::AABB worldBounds{};
    std::string sourceName;
    std::string sourcePath;
  };

  struct InstanceGPU
  {
    float transform[16]{};
    float inverseTransform[16]{};
    float previousTransform[16]{};
    float aabbMin[3]{0.0f, 0.0f, 0.0f};
    uint32_t blasHandle = INVALID_BLAS_HANDLE;
    float aabbMax[3]{0.0f, 0.0f, 0.0f};
    uint32_t sceneInstanceId = virtualgeometry::INVALID_INSTANCE_ID;
    uint32_t flags = 0u;
    uint32_t _padding[3] = {0u, 0u, 0u};
  };
  static_assert(sizeof(InstanceGPU) == 240u, "SoftwareTLASManager::InstanceGPU must stay tightly packed");

  struct InstanceAABBGPU
  {
    float minPoint[3]{0.0f, 0.0f, 0.0f};
    uint32_t instanceIndex = INVALID_INSTANCE_INDEX;
    float maxPoint[3]{0.0f, 0.0f, 0.0f};
    uint32_t blasHandle = INVALID_BLAS_HANDLE;
  };
  static_assert(sizeof(InstanceAABBGPU) == 32u, "SoftwareTLASManager::InstanceAABBGPU must stay tightly packed");

  struct CanonicalNodeGPU
  {
    uint32_t parent = INVALID_NODE;
    uint32_t left = INVALID_NODE;
    uint32_t right = INVALID_NODE;
    uint32_t instanceIndex = INVALID_INSTANCE_INDEX;
    float minPoint[3]{0.0f, 0.0f, 0.0f};
    uint32_t flags = 0u;
    float maxPoint[3]{0.0f, 0.0f, 0.0f};
    uint32_t subtreeLeafCount = 0u;
  };
  static_assert(sizeof(CanonicalNodeGPU) == 48u, "SoftwareTLASManager::CanonicalNodeGPU must stay tightly packed");

  struct GPUBuildHLBVHNodeGPU
  {
    uint32_t parent = INVALID_NODE;
    uint32_t left = INVALID_NODE;
    uint32_t right = INVALID_NODE;
    uint32_t padding = 0u;
    float minPoint[3]{0.0f, 0.0f, 0.0f};
    float _padding0 = 0.0f;
    float maxPoint[3]{0.0f, 0.0f, 0.0f};
    float _padding1 = 0.0f;
  };
  static_assert(sizeof(GPUBuildHLBVHNodeGPU) == 48u, "SoftwareTLASManager::GPUBuildHLBVHNodeGPU must stay tightly packed");

  struct CWBVHNodeGPU
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
  static_assert(sizeof(CWBVHNodeGPU) == 80u, "SoftwareTLASManager::CWBVHNodeGPU must stay tightly packed");

  struct DobbProjectionProxy
  {
    float minProjection[13]{};
    float maxProjection[13]{};
    uint32_t valid = 0u;
  };

  struct CWBVHNodeRefGPU
  {
    uint32_t canonicalNodeIndex = INVALID_NODE;
    uint32_t childCanonicalIndices[8] = {
      INVALID_NODE,
      INVALID_NODE,
      INVALID_NODE,
      INVALID_NODE,
      INVALID_NODE,
      INVALID_NODE,
      INVALID_NODE,
      INVALID_NODE,
    };
  };
  static_assert(sizeof(CWBVHNodeRefGPU) == 36u, "SoftwareTLASManager::CWBVHNodeRefGPU must stay tightly packed");

  struct CWBVHNodeMetadataGPU
  {
    uint32_t flags = 0u;
    uint32_t rotationIndex = 0u;
    uint32_t quantizedFlags = 0u;
    uint32_t padding = 0u;
  };
  static_assert(sizeof(CWBVHNodeMetadataGPU) == 16u, "SoftwareTLASManager::CWBVHNodeMetadataGPU must stay tightly packed");

  struct BVH4NodeGPU
  {
    float childMinX[4]{};
    float childMinY[4]{};
    float childMinZ[4]{};
    uint32_t childIndex[4] = {INVALID_NODE, INVALID_NODE, INVALID_NODE, INVALID_NODE};
    float childMaxX[4]{};
    float childMaxY[4]{};
    float childMaxZ[4]{};
    uint32_t childMeta[4]{};
  };
  static_assert(sizeof(BVH4NodeGPU) == 128u, "SoftwareTLASManager::BVH4NodeGPU must stay tightly packed");

  struct DispatchIndirectArgsGPU
  {
    uint32_t groupCountX = 0u;
    uint32_t groupCountY = 1u;
    uint32_t groupCountZ = 1u;
  };
  static_assert(sizeof(DispatchIndirectArgsGPU) == 12u, "SoftwareTLASManager::DispatchIndirectArgsGPU must stay tightly packed");

  struct CountersGPU
  {
    uint32_t instanceCount = 0u;
    uint32_t nodeCount = 0u;
    uint32_t rootNodeIndex = INVALID_NODE;
    uint32_t buildRevision = 0u;
    uint32_t refitRevision = 0u;
    uint32_t dirtyInstanceCount = 0u;
    uint32_t buildRequested = 0u;
    uint32_t refitRequested = 0u;
  };
  static_assert(sizeof(CountersGPU) == 32u, "SoftwareTLASManager::CountersGPU must stay tightly packed");

  struct BuildSummary
  {
    uint32_t instanceCount = 0u;
    uint32_t nodeCount = 0u;
    uint32_t rootNodeIndex = INVALID_NODE;
    uint32_t dirtyInstanceCount = 0u;
    uint32_t buildRevision = 0u;
    uint32_t refitRevision = 0u;
    bool buildRequested = false;
    bool refitRequested = false;
    bool rebuilt = false;
    bool refitted = false;
    bool partialRebuilt = false;
    uint32_t partialRebuildRootCount = 0u;
    bool bvh4Rebuilt = false;
    bool bvh4Refitted = false;
    bool bvh4RebuiltForSah = false;
    uint32_t bvh4NodeCount = 0u;
    uint32_t bvh4LeafCount = 0u;
    uint32_t bvh4MaxDepth = 0u;
    float bvh4BuildSahScore = 0.0f;
    float bvh4CurrentSahScore = 0.0f;
    float bvh4SahGrowth = 1.0f;
  };

  explicit SoftwareTLASManager(rendering::RenderGraph *renderGraph);
  SoftwareTLASManager(rendering::RenderGraph *renderGraph, Settings settings);
  ~SoftwareTLASManager();

  BuildSummary rebuild(const std::vector<InstanceBuildInput> &instances, uint32_t dirtyInstanceCount);
  BuildSummary refit(const std::vector<InstanceBuildInput> &instances, const std::vector<uint32_t> &dirtyInstanceIndices);
  BuildSummary markClean();
  BuildSummary clear();

  const Settings &getSettings() const
  {
    return settings_;
  }

  const BuildSummary &getLastBuildSummary() const
  {
    return lastBuildSummary_;
  }

  const std::vector<InstanceBuildInput> &getInstanceInputs() const
  {
    return instanceInputs_;
  }

  // GPU instance index -> position in the vector passed to rebuild/refit.
  //
  // The build compacts: an input with an invalid AABB is dropped, so a BVH4
  // leaf's child index is a position among the *accepted* instances and is not
  // the caller's own index. Any consumer that keeps its own instance numbering
  // - the Tree64 dynamic layer keeps stable, sparse slots - has to come back
  // through this map, or it dereferences a real but unrelated instance.
  const std::vector<uint32_t> &getInstanceIndexToInputIndex() const
  {
    return instanceIndexToInputIndex_;
  }

  const rendering::Buffer &getInstancesBuffer() const
  {
    return instancesBuffer_;
  }

  const rendering::Buffer &getInstanceAabbsBuffer() const
  {
    return instanceAabbsBuffer_;
  }

  const rendering::Buffer &getCanonicalNodesBuffer() const
  {
    return canonicalNodesBuffer_;
  }

  const rendering::Buffer &getCwbvhNodesBuffer() const
  {
    return cwbvhNodesBuffer_;
  }

  const rendering::Buffer &getBuildHLBVHNodesBuffer() const
  {
    return buildHLBVHNodesBuffer_;
  }

  const rendering::Buffer &getCwbvhNodeRefsBuffer() const
  {
    return cwbvhNodeRefsBuffer_;
  }

  const rendering::Buffer &getDobbCwbvhNodesBuffer() const
  {
    return dobbCwbvhNodesBuffer_;
  }

  const rendering::Buffer &getCwbvhNodeMetadataBuffer() const
  {
    return cwbvhNodeMetadataBuffer_;
  }

  const rendering::Buffer &getBvh4NodesBuffer() const
  {
    return bvh4NodesBuffer_;
  }

  const rendering::Buffer &getBuildMortonCodesBuffer() const
  {
    return buildMortonCodesBuffer_;
  }

  const rendering::Buffer &getBuildInstanceIndicesBuffer() const
  {
    return buildInstanceIndicesBuffer_;
  }

  const rendering::Buffer &getBuildFlagsBuffer() const
  {
    return buildFlagsBuffer_;
  }

  const rendering::Buffer &getBuildSignalsBuffer() const
  {
    return buildSignalsBuffer_;
  }

  const rendering::Buffer &getBuildDebugBuffer() const
  {
    return buildDebugBuffer_;
  }

  const rendering::Buffer &getLeafInstanceIndicesBuffer() const
  {
    return leafInstanceIndicesBuffer_;
  }

  const rendering::Buffer &getBuildIndirectArgsBuffer() const
  {
    return buildIndirectArgsBuffer_;
  }

  const rendering::Buffer &getBuildSortCountBuffer() const
  {
    return buildSortCountBuffer_;
  }

  const rendering::Buffer &getBuildSortIndirectArgsBuffer() const
  {
    return buildSortIndirectArgsBuffer_;
  }

  const rendering::Buffer &getCountersBuffer() const
  {
    return countersBuffer_;
  }

  const rendering::Buffer &getDirtyInstanceIndicesBuffer() const
  {
    return dirtyInstanceIndicesBuffer_;
  }

  const rendering::Buffer &getRefitIndirectArgsBuffer() const
  {
    return refitIndirectArgsBuffer_;
  }

  const rendering::Buffer &getRefitCwbvhIndirectArgsBuffer() const
  {
    return refitCwbvhIndirectArgsBuffer_;
  }

  uint64_t getInstancesBufferSize() const
  {
    return instancesBufferSize_;
  }

  uint64_t getInstanceAabbsBufferSize() const
  {
    return instanceAabbsBufferSize_;
  }

  uint64_t getCanonicalNodesBufferSize() const
  {
    return canonicalNodesBufferSize_;
  }

  uint64_t getCwbvhNodesBufferSize() const
  {
    return cwbvhNodesBufferSize_;
  }

  uint64_t getBuildHLBVHNodesBufferSize() const
  {
    return buildHLBVHNodesBufferSize_;
  }

  uint64_t getCwbvhNodeRefsBufferSize() const
  {
    return cwbvhNodeRefsBufferSize_;
  }

  uint64_t getDobbCwbvhNodesBufferSize() const
  {
    return dobbCwbvhNodesBufferSize_;
  }

  uint64_t getCwbvhNodeMetadataBufferSize() const
  {
    return cwbvhNodeMetadataBufferSize_;
  }

  uint64_t getBvh4NodesBufferSize() const
  {
    return bvh4NodesBufferSize_;
  }

  uint64_t getBuildMortonCodesBufferSize() const
  {
    return buildMortonCodesBufferSize_;
  }

  uint64_t getBuildInstanceIndicesBufferSize() const
  {
    return buildInstanceIndicesBufferSize_;
  }

  uint64_t getBuildFlagsBufferSize() const
  {
    return buildFlagsBufferSize_;
  }

  uint64_t getBuildSignalsBufferSize() const
  {
    return buildSignalsBufferSize_;
  }

  uint64_t getBuildDebugBufferSize() const
  {
    return buildDebugBufferSize_;
  }

  uint64_t getLeafInstanceIndicesBufferSize() const
  {
    return leafInstanceIndicesBufferSize_;
  }

  uint64_t getBuildIndirectArgsBufferSize() const
  {
    return buildIndirectArgsBufferSize_;
  }

  uint64_t getBuildSortCountBufferSize() const
  {
    return buildSortCountBufferSize_;
  }

  uint64_t getBuildSortIndirectArgsBufferSize() const
  {
    return buildSortIndirectArgsBufferSize_;
  }

  uint64_t getCountersBufferSize() const
  {
    return countersBufferSize_;
  }

  uint64_t getDirtyInstanceIndicesBufferSize() const
  {
    return dirtyInstanceIndicesBufferSize_;
  }

  uint64_t getRefitIndirectArgsBufferSize() const
  {
    return refitIndirectArgsBufferSize_;
  }

  uint64_t getRefitCwbvhIndirectArgsBufferSize() const
  {
    return refitCwbvhIndirectArgsBufferSize_;
  }

  uint32_t getCwbvhNodeCount() const
  {
    return static_cast<uint32_t>(cwbvhNodesGPU_.size());
  }

  uint32_t getDobbCwbvhNodeCount() const
  {
    return static_cast<uint32_t>(dobbCwbvhNodesGPU_.size());
  }

  uint32_t getBvh4NodeCount() const
  {
    return static_cast<uint32_t>(bvh4NodesGPU_.size());
  }

  uint32_t getLeafInstanceCount() const
  {
    return static_cast<uint32_t>(leafInstanceIndicesGPU_.size());
  }

  uint32_t getCanonicalNodeCount() const
  {
    return static_cast<uint32_t>(canonicalNodesGPU_.size());
  }

  uint32_t getMaxInstanceCount() const
  {
    return settings_.maxInstances;
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const;

private:
  struct BuildPrimitive
  {
    uint32_t instanceIndex = 0u;
    uint32_t rotationCandidateIndex = 0u;
    DobbProjectionProxy dobbProxy{};
    virtualgeometry::AABB bounds{};
    float centroid[3]{0.0f, 0.0f, 0.0f};
  };

  struct CanonicalNodeCPU
  {
    virtualgeometry::AABB bounds{};
    uint32_t parent = INVALID_NODE;
    uint32_t left = INVALID_NODE;
    uint32_t right = INVALID_NODE;
    uint32_t instanceIndex = INVALID_INSTANCE_INDEX;
    uint32_t depth = 0u;
    uint32_t subtreeLeafCount = 0u;
    bool isLeaf = false;
  };

  struct BVH4ChildCPU
  {
    virtualgeometry::AABB bounds{};
    uint32_t index = INVALID_NODE;
    uint32_t rotationCandidateIndex = 0u;
    DobbProjectionProxy dobbProxy{};
    bool isLeaf = false;
  };

  struct BVH4NodeCPU
  {
    virtualgeometry::AABB bounds{};
    BVH4ChildCPU children[4]{};
    uint32_t childCount = 0u;
    uint32_t subtreeLeafCount = 0u;
    uint32_t depth = 0u;
    uint32_t rotationCandidateIndex = 0u;
    DobbProjectionProxy dobbProxy{};
  };

  struct BVH4Stats
  {
    uint32_t nodeCount = 0u;
    uint32_t leafCount = 0u;
    uint32_t maxDepth = 0u;
    float sahScore = 0.0f;
  };

  struct BVH4Split
  {
    bool valid = false;
    uint32_t axis = 0u;
    uint32_t mid = 0u;
    float cost = 0.0f;
  };

  struct PackedCwbvhChild
  {
    virtualgeometry::AABB bounds{};
    uint32_t index = INVALID_NODE;
    uint32_t rotationCandidateIndex = 0u;
    DobbProjectionProxy dobbProxy{};
    bool isLeaf = false;
  };

  void initializeResources();
  rendering::Buffer createManagedBuffer(rendering::BufferInfo info, bool frameLocal);
  void initializeFrameOverrideBuffer(const rendering::Buffer &buffer, uint64_t size, rendering::BufferUsage usage);
  rendering::BufferId getFrameOverrideBufferId(const rendering::Buffer &buffer, uint32_t frameSlot) const;
  void writeManagedBuffer(const rendering::Buffer &buffer, uint64_t offset, uint64_t size, const void *data) const;
  void uploadIdleState(uint32_t dirtyInstanceCount);
  void uploadBuildOutputs(uint32_t dirtyInstanceCount, bool rebuilt, bool refitted);
  uint32_t rebuildGpuState(const std::vector<InstanceBuildInput> &instances);
  void refitCanonicalPath(uint32_t nodeIndex);
  void syncCanonicalGpuNode(uint32_t nodeIndex);
  void syncCanonicalGpuState();
  uint32_t rebuildDirtyCanonicalSubtrees(const std::vector<uint32_t> &dirtyLeafNodeIndices, const std::vector<float> &preRefitSurfaceAreas);
  void collectSubtreeNodeIndices(uint32_t nodeIndex, std::vector<uint32_t> &nodeIndices) const;
  void collectSubtreePrimitives(uint32_t nodeIndex, std::vector<BuildPrimitive> &primitives) const;
  uint32_t rebuildCanonicalNodeRecursiveInPlace(std::vector<BuildPrimitive> &primitives, uint32_t begin, uint32_t end, uint32_t parent, uint32_t depth, const std::vector<uint32_t> &nodePool, uint32_t &nodePoolCursor);
  void rebuildCanonicalSubtree(uint32_t rootNodeIndex);
  void rebuildCwbvhState();
  void packCwbvhFromBvh4Node(uint32_t bvh4NodeIndex, uint32_t packedNodeIndex);
  void encodePackedCwbvhNode(
      const std::vector<PackedCwbvhChild> &frontier,
      const virtualgeometry::AABB &sourceBounds,
      uint32_t internalBaseIndex,
      uint32_t leafBaseIndex,
      uint32_t depth,
      bool useDobb,
      CWBVHNodeGPU &outNode,
      CWBVHNodeMetadataGPU &outMetadata) const;
  void rebuildBvh4State(bool markAsBuildBaseline);
  bool refitBvh4State();
  void remapBvh4Instances(const std::vector<uint32_t> &oldInstanceToNewInstance);
  BVH4Split findBvh4Split(std::vector<BuildPrimitive> &primitives, uint32_t begin, uint32_t end);
  uint32_t buildBvh4NodeRecursive(std::vector<BuildPrimitive> &primitives, uint32_t begin, uint32_t end, uint32_t depth);
  BVH4Stats computeBvh4Stats() const;
  void syncBvh4GpuState();

  static bool isValidAabb(const virtualgeometry::AABB &bounds);
  static void computeCentroid(const virtualgeometry::AABB &bounds, float outCentroid[3]);
  static virtualgeometry::AABB unionBounds(const virtualgeometry::AABB &a, const virtualgeometry::AABB &b);

  uint32_t buildCanonicalNodeRecursive(std::vector<BuildPrimitive> &primitives, uint32_t begin, uint32_t end, uint32_t parent, uint32_t depth);

  rendering::RenderGraph *renderGraph_ = nullptr;
  Settings settings_{};

  rendering::Buffer instancesBuffer_;
  rendering::Buffer instanceAabbsBuffer_;
  rendering::Buffer canonicalNodesBuffer_;
  rendering::Buffer buildHLBVHNodesBuffer_;
  rendering::Buffer cwbvhNodesBuffer_;
  rendering::Buffer cwbvhNodeRefsBuffer_;
  rendering::Buffer dobbCwbvhNodesBuffer_;
  rendering::Buffer cwbvhNodeMetadataBuffer_;
  rendering::Buffer bvh4NodesBuffer_;
  rendering::Buffer buildMortonCodesBuffer_;
  rendering::Buffer buildInstanceIndicesBuffer_;
  rendering::Buffer buildFlagsBuffer_;
  rendering::Buffer buildSignalsBuffer_;
  rendering::Buffer buildDebugBuffer_;
  rendering::Buffer leafInstanceIndicesBuffer_;
  rendering::Buffer buildIndirectArgsBuffer_;
  rendering::Buffer buildSortCountBuffer_;
  rendering::Buffer buildSortIndirectArgsBuffer_;
  rendering::Buffer refitIndirectArgsBuffer_;
  rendering::Buffer refitCwbvhIndirectArgsBuffer_;
  rendering::Buffer dirtyInstanceIndicesBuffer_;
  rendering::Buffer countersBuffer_;

  uint64_t instancesBufferSize_ = 0u;
  uint64_t instanceAabbsBufferSize_ = 0u;
  uint64_t canonicalNodesBufferSize_ = 0u;
  uint64_t buildHLBVHNodesBufferSize_ = 0u;
  uint64_t cwbvhNodesBufferSize_ = 0u;
  uint64_t cwbvhNodeRefsBufferSize_ = 0u;
  uint64_t dobbCwbvhNodesBufferSize_ = 0u;
  uint64_t cwbvhNodeMetadataBufferSize_ = 0u;
  uint64_t bvh4NodesBufferSize_ = 0u;
  uint64_t buildMortonCodesBufferSize_ = 0u;
  uint64_t buildInstanceIndicesBufferSize_ = 0u;
  uint64_t buildFlagsBufferSize_ = 0u;
  uint64_t buildSignalsBufferSize_ = 0u;
  uint64_t buildDebugBufferSize_ = 0u;
  uint64_t leafInstanceIndicesBufferSize_ = 0u;
  uint64_t buildIndirectArgsBufferSize_ = 0u;
  uint64_t buildSortCountBufferSize_ = 0u;
  uint64_t buildSortIndirectArgsBufferSize_ = 0u;
  uint64_t refitIndirectArgsBufferSize_ = 0u;
  uint64_t refitCwbvhIndirectArgsBufferSize_ = 0u;
  uint64_t dirtyInstanceIndicesBufferSize_ = 0u;
  uint64_t countersBufferSize_ = 0u;

  std::unordered_map<std::string, std::vector<rendering::BufferId>> frameOverrideBufferIds_;

  std::vector<InstanceBuildInput> instanceInputs_;
  std::vector<CanonicalNodeCPU> canonicalNodesCPU_;
  std::vector<InstanceGPU> instancesGPU_;
  std::vector<InstanceAABBGPU> instanceAabbsGPU_;
  std::vector<CanonicalNodeGPU> canonicalNodesGPU_;
  std::vector<CWBVHNodeGPU> cwbvhNodesGPU_;
  std::vector<CWBVHNodeRefGPU> cwbvhNodeRefsGPU_;
  std::vector<CWBVHNodeGPU> dobbCwbvhNodesGPU_;
  std::vector<CWBVHNodeMetadataGPU> cwbvhNodeMetadataGPU_;
  std::vector<BVH4NodeCPU> bvh4NodesCPU_;
  std::vector<BVH4NodeGPU> bvh4NodesGPU_;
  std::vector<uint32_t> leafInstanceIndicesGPU_;
  std::vector<uint32_t> dirtyInstanceIndicesGPU_;
  std::vector<uint32_t> compactedInputToInstanceIndex_;
  std::vector<uint32_t> instanceIndexToInputIndex_;
  std::vector<uint32_t> instanceIndexToLeafNodeIndex_;
  std::unordered_map<virtualgeometry::InstanceId, uint32_t> instanceIdToInstanceIndex_;
  BuildSummary lastBuildSummary_{};
};

} // namespace softwarebvh
