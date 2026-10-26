#include "softwarebvh/SoftwareRayTraceScene.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace softwarebvh
{
namespace
{

uint32_t clampSceneCapacity(const uint64_t value)
{
  return static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(value, 1u), std::numeric_limits<uint32_t>::max()));
}

} // namespace

SoftwareRayTraceScene::SoftwareRayTraceScene() : SoftwareRayTraceScene(Settings{})
{
}

SoftwareRayTraceScene::SoftwareRayTraceScene(Settings settings) : settings_(settings)
{
}

SoftwareRayTraceScene::DispatchIndirectArgsGPU SoftwareRayTraceScene::makeDispatchArgs(const uint32_t itemCount, const uint32_t workgroupSize)
{
  DispatchIndirectArgsGPU args{};
  const uint32_t divisor = std::max(workgroupSize, 1u);
  args.groupCountX = itemCount == 0u ? 0u : (itemCount + divisor - 1u) / divisor;
  return args;
}

SoftwareRayTraceScene::Settings SoftwareRayTraceScene::estimateSettings(
    const uint32_t tlasCWBVHNodeCount,
    const std::vector<SourceBLAS> &sources,
    const std::vector<InstanceInput> &instances,
    const uint32_t workgroupSize)
{
  std::unordered_map<uint32_t, SourceBLAS> sourcesByHandle;
  sourcesByHandle.reserve(sources.size());
  for (const SourceBLAS &source : sources)
  {
    if (source.handle != INVALID_INDEX)
    {
      sourcesByHandle[source.handle] = source;
    }
  }

  uint64_t sceneCWBVHNodeCount = tlasCWBVHNodeCount;
  uint64_t worldBVH8NodeCount = 0u;
  uint64_t worldTriangleCount = 0u;
  uint64_t worldLeafRemapCount = 0u;
  for (const InstanceInput &instance : instances)
  {
    const auto sourceIt = sourcesByHandle.find(instance.blasHandle);
    if (sourceIt == sourcesByHandle.end())
    {
      continue;
    }

    const SourceBLAS &source = sourceIt->second;
    sceneCWBVHNodeCount += source.cwbvhNodeCount;
    worldBVH8NodeCount += source.bvh8NodeCount;
    worldTriangleCount += source.triangleCount;
    worldLeafRemapCount += source.leafRemapCount;
  }

  Settings settings{};
  settings.maxInstances = clampSceneCapacity(instances.size());
  settings.maxSceneCWBVHNodes = clampSceneCapacity(sceneCWBVHNodeCount);
  settings.maxWorldBVH8Nodes = clampSceneCapacity(worldBVH8NodeCount);
  settings.maxWorldTriangles = clampSceneCapacity(worldTriangleCount);
  settings.maxWorldLeafRemapEntries = clampSceneCapacity(worldLeafRemapCount);
  settings.workgroupSize = std::max(workgroupSize, 1u);
  return settings;
}

SoftwareRayTraceScene::BakedKey SoftwareRayTraceScene::makeBakedKey(const InstanceInput &instance)
{
  return BakedKey{
      .sceneInstanceId = instance.sceneInstanceId,
      .instanceIndex = instance.instanceIndex,
  };
}

SoftwareRayTraceScene::BakedKey SoftwareRayTraceScene::makeBakedKey(const InstanceRange &instance)
{
  return BakedKey{
      .sceneInstanceId = instance.sceneInstanceId,
      .instanceIndex = instance.instanceIndex,
  };
}

SoftwareRayTraceScene::Plan SoftwareRayTraceScene::buildPlan(
    const uint32_t tlasCWBVHNodeCount,
    const std::vector<SourceBLAS> &sources,
    const std::vector<InstanceInput> &instances) const
{
  Plan plan{};
  plan.tlasCWBVHNodes = Range{.offset = 0u, .count = tlasCWBVHNodeCount};

  if (tlasCWBVHNodeCount > settings_.maxSceneCWBVHNodes)
  {
    plan.valid = false;
    plan.error = "TLAS CWBVH node range exceeds scene CWBVH capacity";
    return plan;
  }
  if (instances.size() > settings_.maxInstances)
  {
    plan.valid = false;
    plan.error = "Instance count exceeds SoftwareRayTraceScene capacity";
    return plan;
  }

  std::unordered_map<uint32_t, SourceBLAS> sourcesByHandle;
  sourcesByHandle.reserve(sources.size());
  for (const SourceBLAS &source : sources)
  {
    if (source.handle == INVALID_INDEX)
    {
      continue;
    }
    sourcesByHandle[source.handle] = source;
  }

  uint32_t blasCWBVHWriteHead = settings_.maxSceneCWBVHNodes;
  uint32_t worldBVH8Cursor = 0u;
  uint32_t worldTriangleCursor = 0u;
  uint32_t worldLeafRemapCursor = 0u;
  plan.instances.reserve(instances.size());

  for (const InstanceInput &instance : instances)
  {
    InstanceRange range{};
    range.instanceIndex = instance.instanceIndex;
    range.sceneInstanceId = instance.sceneInstanceId;
    range.blasHandle = instance.blasHandle;
    range.flags = instance.flags;
    range.dynamic = (instance.flags & INSTANCE_FLAG_DYNAMIC) != 0u;

    const auto sourceIt = sourcesByHandle.find(instance.blasHandle);
    if (sourceIt == sourcesByHandle.end() ||
        sourceIt->second.cwbvhNodeCount == 0u ||
        sourceIt->second.bvh8NodeCount == 0u)
    {
      plan.instances.push_back(range);
      continue;
    }

    const SourceBLAS &source = sourceIt->second;
    if (source.cwbvhNodeCount != source.bvh8NodeCount)
    {
      plan.valid = false;
      plan.error = "World BLAS CWBVH node count must match BVH8 node count";
      return plan;
    }
    if (source.cwbvhNodeCount > blasCWBVHWriteHead || blasCWBVHWriteHead - source.cwbvhNodeCount < tlasCWBVHNodeCount)
    {
      plan.valid = false;
      plan.error = "World BLAS CWBVH range overlaps TLAS CWBVH range";
      return plan;
    }
    if (worldBVH8Cursor + source.bvh8NodeCount > settings_.maxWorldBVH8Nodes)
    {
      plan.valid = false;
      plan.error = "World BVH8 node range exceeds capacity";
      return plan;
    }
    if (worldTriangleCursor + source.triangleCount > settings_.maxWorldTriangles)
    {
      plan.valid = false;
      plan.error = "World triangle range exceeds capacity";
      return plan;
    }
    if (worldLeafRemapCursor + source.leafRemapCount > settings_.maxWorldLeafRemapEntries)
    {
      plan.valid = false;
      plan.error = "World leaf remap range exceeds capacity";
      return plan;
    }

    blasCWBVHWriteHead -= source.cwbvhNodeCount;
    range.worldCWBVHNodes = Range{.offset = blasCWBVHWriteHead, .count = source.cwbvhNodeCount};
    range.worldBVH8Nodes = Range{.offset = worldBVH8Cursor, .count = source.bvh8NodeCount};
    range.worldTriangles = Range{.offset = worldTriangleCursor, .count = source.triangleCount};
    range.worldLeafRemap = Range{.offset = worldLeafRemapCursor, .count = source.leafRemapCount};
    range.valid = true;
    range.primitiveRemapOffset = source.primitiveRemapOffset;

    worldBVH8Cursor += source.bvh8NodeCount;
    worldTriangleCursor += source.triangleCount;
    worldLeafRemapCursor += source.leafRemapCount;

    const bool hasWorldBake = bakedInstanceRevisions_.find(makeBakedKey(instance)) != bakedInstanceRevisions_.end();
    range.needsUpdate = !hasWorldBake || (range.dynamic && instance.transformDirty);
    if (range.needsUpdate)
    {
      plan.updateInstanceRangeIndices.push_back(static_cast<uint32_t>(plan.instances.size()));
      plan.updateTriangleCount += source.triangleCount;
      plan.updateBVH8NodeCount += source.bvh8NodeCount;
      plan.updateCWBVHNodeCount += source.cwbvhNodeCount;
      plan.updateLeafRemapCount += source.leafRemapCount;
    }

    plan.instances.push_back(range);
  }

  plan.sceneCWBVHBlasBegin = blasCWBVHWriteHead;
  plan.sceneCWBVHNodeCount = settings_.maxSceneCWBVHNodes - blasCWBVHWriteHead + tlasCWBVHNodeCount;
  plan.worldBVH8NodeCount = worldBVH8Cursor;
  plan.worldTriangleCount = worldTriangleCursor;
  plan.worldLeafRemapCount = worldLeafRemapCursor;
  plan.transformTrianglesDispatch = makeDispatchArgs(plan.updateTriangleCount, settings_.workgroupSize);
  plan.compressCWBVHDispatch = makeDispatchArgs(plan.updateBVH8NodeCount, settings_.workgroupSize);
  return plan;
}

std::vector<SoftwareRayTraceScene::WorldInstanceRangeGPU> SoftwareRayTraceScene::buildInstanceRangeGPU(const Plan &plan)
{
  std::vector<WorldInstanceRangeGPU> gpuRanges;
  gpuRanges.reserve(plan.instances.size());
  for (const InstanceRange &range : plan.instances)
  {
    WorldInstanceRangeGPU gpu{};
    gpu.instanceIndex = range.instanceIndex;
    gpu.sceneInstanceId = range.sceneInstanceId;
    gpu.blasHandle = range.blasHandle;
    gpu.flags = range.flags;
    gpu.worldCWBVHNodeOffset = range.worldCWBVHNodes.offset;
    gpu.worldCWBVHNodeCount = range.worldCWBVHNodes.count;
    gpu.worldBVH8NodeOffset = range.worldBVH8Nodes.offset;
    gpu.worldBVH8NodeCount = range.worldBVH8Nodes.count;
    gpu.worldTriangleOffset = range.worldTriangles.offset;
    gpu.worldTriangleCount = range.worldTriangles.count;
    gpu.worldLeafRemapOffset = range.worldLeafRemap.offset;
    gpu.worldLeafRemapCount = range.worldLeafRemap.count;
    gpu.valid = range.valid ? 1u : 0u;
    gpu.dynamic = range.dynamic ? 1u : 0u;
    gpu.needsUpdate = range.needsUpdate ? 1u : 0u;
    gpu.primitiveRemapOffset = range.primitiveRemapOffset;
    gpuRanges.push_back(gpu);
  }
  return gpuRanges;
}

std::vector<SoftwareRayTraceScene::TriangleUpdateItemGPU> SoftwareRayTraceScene::buildTriangleUpdateItems(const Plan &plan, const std::vector<SourceBLAS> &sources)
{
  std::unordered_map<uint32_t, SourceBLAS> sourcesByHandle;
  sourcesByHandle.reserve(sources.size());
  for (const SourceBLAS &source : sources)
  {
    sourcesByHandle[source.handle] = source;
  }

  std::vector<TriangleUpdateItemGPU> items;
  items.reserve(plan.updateTriangleCount);
  for (const uint32_t rangeIndex : plan.updateInstanceRangeIndices)
  {
    if (rangeIndex >= plan.instances.size())
    {
      continue;
    }
    const InstanceRange &range = plan.instances[rangeIndex];
    const auto sourceIt = sourcesByHandle.find(range.blasHandle);
    if (sourceIt == sourcesByHandle.end())
    {
      continue;
    }
    const SourceBLAS &source = sourceIt->second;
    for (uint32_t triangleIndex = 0u; triangleIndex < source.triangleCount; ++triangleIndex)
    {
      items.push_back(TriangleUpdateItemGPU{
          .sourceTriangleIndex = source.localTriangleOffset + triangleIndex,
          .worldTriangleIndex = range.worldTriangles.offset + triangleIndex,
          .instanceRangeIndex = rangeIndex,
      });
    }
  }
  return items;
}

std::vector<SoftwareRayTraceScene::BVH8UpdateItemGPU> SoftwareRayTraceScene::buildBVH8UpdateItems(const Plan &plan, const std::vector<SourceBLAS> &sources)
{
  std::unordered_map<uint32_t, SourceBLAS> sourcesByHandle;
  sourcesByHandle.reserve(sources.size());
  for (const SourceBLAS &source : sources)
  {
    sourcesByHandle[source.handle] = source;
  }

  std::vector<BVH8UpdateItemGPU> items;
  items.reserve(plan.updateBVH8NodeCount);
  for (const uint32_t rangeIndex : plan.updateInstanceRangeIndices)
  {
    if (rangeIndex >= plan.instances.size())
    {
      continue;
    }
    const InstanceRange &range = plan.instances[rangeIndex];
    const auto sourceIt = sourcesByHandle.find(range.blasHandle);
    if (sourceIt == sourcesByHandle.end())
    {
      continue;
    }
    const SourceBLAS &source = sourceIt->second;
    for (uint32_t nodeIndex = 0u; nodeIndex < source.bvh8NodeCount; ++nodeIndex)
    {
      items.push_back(BVH8UpdateItemGPU{
          .sourceNodeIndex = source.localBVH8NodeOffset + nodeIndex,
          .worldNodeIndex = range.worldBVH8Nodes.offset + nodeIndex,
          .instanceRangeIndex = rangeIndex,
          .worldCWBVHNodeOffset = range.worldCWBVHNodes.offset,
          .worldBVH8NodeOffset = range.worldBVH8Nodes.offset,
          .worldLeafRemapOffset = range.worldLeafRemap.offset,
      });
    }
  }
  return items;
}

std::vector<SoftwareRayTraceScene::LeafRemapUpdateItemGPU> SoftwareRayTraceScene::buildLeafRemapUpdateItems(const Plan &plan, const std::vector<SourceBLAS> &sources)
{
  std::unordered_map<uint32_t, SourceBLAS> sourcesByHandle;
  sourcesByHandle.reserve(sources.size());
  for (const SourceBLAS &source : sources)
  {
    sourcesByHandle[source.handle] = source;
  }

  std::vector<LeafRemapUpdateItemGPU> items;
  items.reserve(plan.updateLeafRemapCount);
  for (const uint32_t rangeIndex : plan.updateInstanceRangeIndices)
  {
    if (rangeIndex >= plan.instances.size())
    {
      continue;
    }
    const InstanceRange &range = plan.instances[rangeIndex];
    const auto sourceIt = sourcesByHandle.find(range.blasHandle);
    if (sourceIt == sourcesByHandle.end())
    {
      continue;
    }
    const SourceBLAS &source = sourceIt->second;
    for (uint32_t remapIndex = 0u; remapIndex < source.leafRemapCount; ++remapIndex)
    {
      items.push_back(LeafRemapUpdateItemGPU{
          .sourceLeafRemapIndex = source.localLeafRemapOffset + remapIndex,
          .worldLeafRemapIndex = range.worldLeafRemap.offset + remapIndex,
          .instanceRangeIndex = rangeIndex,
      });
    }
  }
  return items;
}

SoftwareRayTraceScene::BVH8NodeGPU SoftwareRayTraceScene::buildBVH8NodeGPU(const CWBVHBuilder::BVH8Node &node)
{
  BVH8NodeGPU gpu{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    gpu.boundsMin[axis] = node.bounds.minPoint[axis];
    gpu.boundsMax[axis] = node.bounds.maxPoint[axis];
  }
  gpu.cwbvhOutputIndex = node.cwbvhOutputIndex;
  gpu.internalChildBase = node.internalChildBase;
  gpu.leafBase = node.leafBase;
  gpu.validChildMask = node.validChildMask;
  gpu.internalChildMask = node.internalChildMask;
  gpu.leafChildMask = node.leafChildMask;
  for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      gpu.childMin[childSlot][axis] = node.childMin[childSlot][axis];
      gpu.childMax[childSlot][axis] = node.childMax[childSlot][axis];
    }
    gpu.childIndex[childSlot] = node.childIndex[childSlot];
    gpu.childMeta[childSlot] = node.childMeta[childSlot];
  }
  return gpu;
}

SoftwareRayTraceScene::PackedCWBVHNodeGPU SoftwareRayTraceScene::buildPackedCWBVHNodeGPU(const CWBVHBuilder::PackedCWBVHNode &node)
{
  PackedCWBVHNodeGPU gpu{};
  for (uint32_t word = 0u; word < 2u; ++word)
  {
    gpu.orxy[word] = node.orxy[word];
    gpu.oeim[word] = node.oeim[word];
    gpu.ntbi[word] = node.ntbi[word];
    gpu.info[word] = node.info[word];
    gpu.qlox[word] = node.qlox[word];
    gpu.qloy[word] = node.qloy[word];
    gpu.qloz[word] = node.qloz[word];
    gpu.qhix[word] = node.qhix[word];
    gpu.qhiy[word] = node.qhiy[word];
    gpu.qhiz[word] = node.qhiz[word];
  }
  return gpu;
}

void SoftwareRayTraceScene::markUpdatesCompleted(const Plan &plan)
{
  if (!plan.valid)
  {
    return;
  }

  for (const uint32_t rangeIndex : plan.updateInstanceRangeIndices)
  {
    if (rangeIndex >= plan.instances.size())
    {
      continue;
    }
    const InstanceRange &range = plan.instances[rangeIndex];
    if (!range.valid)
    {
      continue;
    }
    bakedInstanceRevisions_[makeBakedKey(range)] = bakeRevision_;
  }
  ++bakeRevision_;
}

void SoftwareRayTraceScene::clearBakeState()
{
  bakedInstanceRevisions_.clear();
  ++bakeRevision_;
}

} // namespace softwarebvh
