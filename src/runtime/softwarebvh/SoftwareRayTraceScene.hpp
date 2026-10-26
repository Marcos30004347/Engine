#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "softwarebvh/CWBVHBuilder.hpp"

namespace softwarebvh
{

class SoftwareRayTraceScene
{
public:
  static constexpr uint32_t INVALID_INDEX = UINT32_MAX;
  static constexpr uint32_t INSTANCE_FLAG_DYNAMIC = 1u << 0u;

  struct Settings
  {
    uint32_t maxInstances = 16384u;
    uint32_t maxSceneCWBVHNodes = 16u * 1024u * 1024u;
    uint32_t maxWorldBVH8Nodes = 8u * 1024u * 1024u;
    uint32_t maxWorldTriangles = 8u * 1024u * 1024u;
    uint32_t maxWorldLeafRemapEntries = 8u * 1024u * 1024u;
    uint32_t workgroupSize = 64u;
  };

  struct Range
  {
    uint32_t offset = 0u;
    uint32_t count = 0u;
  };

  struct SourceBLAS
  {
    uint32_t handle = INVALID_INDEX;
    uint32_t localBVH8NodeOffset = 0u;
    uint32_t localTriangleOffset = 0u;
    uint32_t localLeafRemapOffset = 0u;
    uint32_t primitiveRemapOffset = 0u;
    uint32_t bvh8NodeCount = 0u;
    uint32_t cwbvhNodeCount = 0u;
    uint32_t triangleCount = 0u;
    uint32_t leafRemapCount = 0u;
  };

  struct InstanceInput
  {
    uint32_t instanceIndex = INVALID_INDEX;
    uint32_t sceneInstanceId = INVALID_INDEX;
    uint32_t blasHandle = INVALID_INDEX;
    uint32_t flags = 0u;
    bool transformDirty = false;
  };

  struct DispatchIndirectArgsGPU
  {
    uint32_t groupCountX = 0u;
    uint32_t groupCountY = 1u;
    uint32_t groupCountZ = 1u;
  };
  static_assert(sizeof(DispatchIndirectArgsGPU) == 12u, "SoftwareRayTraceScene::DispatchIndirectArgsGPU must stay tightly packed");

  struct TriBaryTransformGPU
  {
    float row0[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float row1[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float row2[4]{0.0f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(TriBaryTransformGPU) == 48u, "SoftwareRayTraceScene::TriBaryTransformGPU must stay tightly packed");

  struct InstanceTransformGPU
  {
    float transform[16]{};
    float inverseTransform[16]{};
  };
  static_assert(sizeof(InstanceTransformGPU) == 128u, "SoftwareRayTraceScene::InstanceTransformGPU must stay tightly packed");

  struct WorldInstanceRangeGPU
  {
    uint32_t instanceIndex = INVALID_INDEX;
    uint32_t sceneInstanceId = INVALID_INDEX;
    uint32_t blasHandle = INVALID_INDEX;
    uint32_t flags = 0u;
    uint32_t worldCWBVHNodeOffset = 0u;
    uint32_t worldCWBVHNodeCount = 0u;
    uint32_t worldBVH8NodeOffset = 0u;
    uint32_t worldBVH8NodeCount = 0u;
    uint32_t worldTriangleOffset = 0u;
    uint32_t worldTriangleCount = 0u;
    uint32_t worldLeafRemapOffset = 0u;
    uint32_t worldLeafRemapCount = 0u;
    uint32_t valid = 0u;
    uint32_t dynamic = 0u;
    uint32_t needsUpdate = 0u;
    uint32_t primitiveRemapOffset = 0u;
  };
  static_assert(sizeof(WorldInstanceRangeGPU) == 64u, "SoftwareRayTraceScene::WorldInstanceRangeGPU must stay tightly packed");

  struct UpdateCountersGPU
  {
    uint32_t updateInstanceCount = 0u;
    uint32_t updateTriangleCount = 0u;
    uint32_t updateBVH8NodeCount = 0u;
    uint32_t updateCWBVHNodeCount = 0u;
    uint32_t updateLeafRemapCount = 0u;
  };
  static_assert(sizeof(UpdateCountersGPU) == 20u, "SoftwareRayTraceScene::UpdateCountersGPU must stay tightly packed");

  struct TriangleUpdateItemGPU
  {
    uint32_t sourceTriangleIndex = 0u;
    uint32_t worldTriangleIndex = 0u;
    uint32_t instanceRangeIndex = 0u;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(TriangleUpdateItemGPU) == 16u, "SoftwareRayTraceScene::TriangleUpdateItemGPU must stay tightly packed");

  struct BVH8UpdateItemGPU
  {
    uint32_t sourceNodeIndex = 0u;
    uint32_t worldNodeIndex = 0u;
    uint32_t instanceRangeIndex = 0u;
    uint32_t worldCWBVHNodeOffset = 0u;
    uint32_t worldBVH8NodeOffset = 0u;
    uint32_t worldLeafRemapOffset = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
  };
  static_assert(sizeof(BVH8UpdateItemGPU) == 32u, "SoftwareRayTraceScene::BVH8UpdateItemGPU must stay tightly packed");

  struct LeafRemapUpdateItemGPU
  {
    uint32_t sourceLeafRemapIndex = 0u;
    uint32_t worldLeafRemapIndex = 0u;
    uint32_t instanceRangeIndex = 0u;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(LeafRemapUpdateItemGPU) == 16u, "SoftwareRayTraceScene::LeafRemapUpdateItemGPU must stay tightly packed");

  struct WorldLeafEntryGPU
  {
    uint32_t worldTriangleIndex = INVALID_INDEX;
    uint32_t originalPrimitiveIndex = INVALID_INDEX;
  };
  static_assert(sizeof(WorldLeafEntryGPU) == 8u, "SoftwareRayTraceScene::WorldLeafEntryGPU must stay tightly packed");

  struct BVH8NodeGPU
  {
    float boundsMin[3]{0.0f, 0.0f, 0.0f};
    uint32_t cwbvhOutputIndex = INVALID_INDEX;
    float boundsMax[3]{0.0f, 0.0f, 0.0f};
    uint32_t internalChildBase = INVALID_INDEX;
    float childMin[8][4]{};
    float childMax[8][4]{};
    uint32_t childIndex[8] = {
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
        INVALID_INDEX,
    };
    uint32_t childMeta[8]{};
    uint32_t leafBase = 0u;
    uint32_t validChildMask = 0u;
    uint32_t internalChildMask = 0u;
    uint32_t leafChildMask = 0u;
  };
  static_assert(sizeof(BVH8NodeGPU) == 368u, "SoftwareRayTraceScene::BVH8NodeGPU must stay tightly packed");

  struct PackedCWBVHNodeGPU
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
  static_assert(sizeof(PackedCWBVHNodeGPU) == 80u, "SoftwareRayTraceScene::PackedCWBVHNodeGPU must stay tightly packed");

  struct InstanceRange
  {
    uint32_t instanceIndex = INVALID_INDEX;
    uint32_t sceneInstanceId = INVALID_INDEX;
    uint32_t blasHandle = INVALID_INDEX;
    uint32_t flags = 0u;
    Range worldCWBVHNodes{};
    Range worldBVH8Nodes{};
    Range worldTriangles{};
    Range worldLeafRemap{};
    bool valid = false;
    bool dynamic = false;
    bool needsUpdate = false;
    uint32_t primitiveRemapOffset = 0u;
  };

  struct Plan
  {
    Range tlasCWBVHNodes{};
    uint32_t sceneCWBVHNodeCount = 0u;
    uint32_t sceneCWBVHBlasBegin = 0u;
    uint32_t worldBVH8NodeCount = 0u;
    uint32_t worldTriangleCount = 0u;
    uint32_t worldLeafRemapCount = 0u;
    std::vector<InstanceRange> instances;
    std::vector<uint32_t> updateInstanceRangeIndices;
    uint32_t updateTriangleCount = 0u;
    uint32_t updateBVH8NodeCount = 0u;
    uint32_t updateCWBVHNodeCount = 0u;
    uint32_t updateLeafRemapCount = 0u;
    DispatchIndirectArgsGPU transformTrianglesDispatch{};
    DispatchIndirectArgsGPU compressCWBVHDispatch{};
    bool valid = true;
    std::string error;
  };

  SoftwareRayTraceScene();
  explicit SoftwareRayTraceScene(Settings settings);

  const Settings &getSettings() const
  {
    return settings_;
  }

	  Plan buildPlan(
	      uint32_t tlasCWBVHNodeCount,
	      const std::vector<SourceBLAS> &sources,
	      const std::vector<InstanceInput> &instances) const;

	  static Settings estimateSettings(
	      uint32_t tlasCWBVHNodeCount,
	      const std::vector<SourceBLAS> &sources,
	      const std::vector<InstanceInput> &instances,
	      uint32_t workgroupSize = 64u);
	  static std::vector<WorldInstanceRangeGPU> buildInstanceRangeGPU(const Plan &plan);
  static std::vector<TriangleUpdateItemGPU> buildTriangleUpdateItems(const Plan &plan, const std::vector<SourceBLAS> &sources);
  static std::vector<BVH8UpdateItemGPU> buildBVH8UpdateItems(const Plan &plan, const std::vector<SourceBLAS> &sources);
  static std::vector<LeafRemapUpdateItemGPU> buildLeafRemapUpdateItems(const Plan &plan, const std::vector<SourceBLAS> &sources);
  static BVH8NodeGPU buildBVH8NodeGPU(const CWBVHBuilder::BVH8Node &node);
  static PackedCWBVHNodeGPU buildPackedCWBVHNodeGPU(const CWBVHBuilder::PackedCWBVHNode &node);

  void markUpdatesCompleted(const Plan &plan);
  void clearBakeState();

private:
  struct BakedKey
  {
    uint32_t sceneInstanceId = INVALID_INDEX;
    uint32_t instanceIndex = INVALID_INDEX;

    bool operator==(const BakedKey &other) const
    {
      return sceneInstanceId == other.sceneInstanceId && instanceIndex == other.instanceIndex;
    }
  };

  struct BakedKeyHash
  {
    size_t operator()(const BakedKey &key) const
    {
      return (static_cast<size_t>(key.sceneInstanceId) << 32u) ^ static_cast<size_t>(key.instanceIndex);
    }
  };

  static DispatchIndirectArgsGPU makeDispatchArgs(uint32_t itemCount, uint32_t workgroupSize);
  static BakedKey makeBakedKey(const InstanceInput &instance);
  static BakedKey makeBakedKey(const InstanceRange &instance);

  Settings settings_{};
  std::unordered_map<BakedKey, uint32_t, BakedKeyHash> bakedInstanceRevisions_;
  uint32_t bakeRevision_ = 1u;
};

} // namespace softwarebvh
