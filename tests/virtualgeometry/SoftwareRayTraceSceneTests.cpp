#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "softwarebvh/SoftwareRayTraceScene.hpp"
#include "softwarebvh/rendering/SoftwareWorldBLASUpdatePass.hpp"

namespace
{

using Scene = softwarebvh::SoftwareRayTraceScene;

void expectRange(const Scene::Range &range, uint32_t offset, uint32_t count)
{
  assert(range.offset == offset);
  assert(range.count == count);
}

void sceneArenaAllocatesTlasLeftAndBlasRight()
{
  Scene::Settings settings{};
  settings.maxInstances = 8u;
  settings.maxSceneCWBVHNodes = 32u;
  settings.maxWorldBVH8Nodes = 32u;
  settings.maxWorldTriangles = 32u;
  settings.maxWorldLeafRemapEntries = 32u;
  settings.workgroupSize = 4u;
  Scene scene(settings);

  const std::vector<Scene::SourceBLAS> sources = {
      Scene::SourceBLAS{.handle = 10u, .bvh8NodeCount = 3u, .cwbvhNodeCount = 3u, .triangleCount = 5u, .leafRemapCount = 5u},
      Scene::SourceBLAS{.handle = 11u, .bvh8NodeCount = 4u, .cwbvhNodeCount = 4u, .triangleCount = 6u, .leafRemapCount = 6u},
  };
  const std::vector<Scene::InstanceInput> instances = {
      Scene::InstanceInput{.instanceIndex = 0u, .sceneInstanceId = 100u, .blasHandle = 10u, .flags = 0u},
      Scene::InstanceInput{.instanceIndex = 1u, .sceneInstanceId = 101u, .blasHandle = 11u, .flags = Scene::INSTANCE_FLAG_DYNAMIC},
      Scene::InstanceInput{.instanceIndex = 2u, .sceneInstanceId = 102u, .blasHandle = 10u, .flags = 0u},
  };

  Scene::Plan plan = scene.buildPlan(5u, sources, instances);
  assert(plan.valid);
  expectRange(plan.tlasCWBVHNodes, 0u, 5u);
  assert(plan.sceneCWBVHBlasBegin == 22u);
  assert(plan.sceneCWBVHBlasBegin >= plan.tlasCWBVHNodes.count);
  assert(plan.instances.size() == 3u);
  expectRange(plan.instances[0u].worldCWBVHNodes, 29u, 3u);
  expectRange(plan.instances[1u].worldCWBVHNodes, 25u, 4u);
  expectRange(plan.instances[2u].worldCWBVHNodes, 22u, 3u);
  expectRange(plan.instances[0u].worldBVH8Nodes, 0u, 3u);
  expectRange(plan.instances[1u].worldBVH8Nodes, 3u, 4u);
  expectRange(plan.instances[2u].worldBVH8Nodes, 7u, 3u);
  expectRange(plan.instances[0u].worldTriangles, 0u, 5u);
  expectRange(plan.instances[1u].worldTriangles, 5u, 6u);
  expectRange(plan.instances[2u].worldTriangles, 11u, 5u);
  assert(plan.updateInstanceRangeIndices.size() == 3u);
  assert(plan.updateTriangleCount == 16u);
  assert(plan.updateBVH8NodeCount == 10u);
  assert(plan.updateCWBVHNodeCount == 10u);
  assert(plan.updateLeafRemapCount == 16u);
  assert(plan.transformTrianglesDispatch.groupCountX == 4u);
  assert(plan.compressCWBVHDispatch.groupCountX == 3u);
}

void sceneBakeStateSkipsStaticAndQueuesDirtyDynamic()
{
  Scene::Settings settings{};
  settings.maxInstances = 4u;
  settings.maxSceneCWBVHNodes = 24u;
  settings.maxWorldBVH8Nodes = 16u;
  settings.maxWorldTriangles = 16u;
  settings.maxWorldLeafRemapEntries = 16u;
  settings.workgroupSize = 8u;
  Scene scene(settings);

  const std::vector<Scene::SourceBLAS> sources = {
      Scene::SourceBLAS{.handle = 1u, .bvh8NodeCount = 2u, .cwbvhNodeCount = 2u, .triangleCount = 3u, .leafRemapCount = 3u},
  };
  std::vector<Scene::InstanceInput> instances = {
      Scene::InstanceInput{.instanceIndex = 0u, .sceneInstanceId = 10u, .blasHandle = 1u, .flags = 0u},
      Scene::InstanceInput{.instanceIndex = 1u, .sceneInstanceId = 11u, .blasHandle = 1u, .flags = Scene::INSTANCE_FLAG_DYNAMIC},
  };

  Scene::Plan firstPlan = scene.buildPlan(2u, sources, instances);
  assert(firstPlan.valid);
  assert(firstPlan.updateInstanceRangeIndices.size() == 2u);
  scene.markUpdatesCompleted(firstPlan);

  Scene::Plan cleanPlan = scene.buildPlan(2u, sources, instances);
  assert(cleanPlan.valid);
  assert(cleanPlan.updateInstanceRangeIndices.empty());
  assert(cleanPlan.transformTrianglesDispatch.groupCountX == 0u);
  assert(cleanPlan.compressCWBVHDispatch.groupCountX == 0u);

  instances[1u].transformDirty = true;
  Scene::Plan dirtyPlan = scene.buildPlan(2u, sources, instances);
  assert(dirtyPlan.valid);
  assert(dirtyPlan.updateInstanceRangeIndices.size() == 1u);
  assert(dirtyPlan.updateInstanceRangeIndices[0u] == 1u);
  assert(dirtyPlan.updateTriangleCount == 3u);
  assert(dirtyPlan.updateCWBVHNodeCount == 2u);
  assert(dirtyPlan.updateLeafRemapCount == 3u);
}

void sceneRecompileRequeuesWorldCwbvhBakeAfterInstanceUpdates()
{
  Scene::Settings settings{};
  settings.maxInstances = 4u;
  settings.maxSceneCWBVHNodes = 32u;
  settings.maxWorldBVH8Nodes = 16u;
  settings.maxWorldTriangles = 16u;
  settings.maxWorldLeafRemapEntries = 16u;
  settings.workgroupSize = 4u;
  Scene scene(settings);

  const std::vector<Scene::SourceBLAS> sources = {
      Scene::SourceBLAS{.handle = 7u, .bvh8NodeCount = 2u, .cwbvhNodeCount = 2u, .triangleCount = 3u, .leafRemapCount = 3u},
  };
  std::vector<Scene::InstanceInput> instances = {
      Scene::InstanceInput{.instanceIndex = 0u, .sceneInstanceId = 100u, .blasHandle = 7u, .flags = 0u},
      Scene::InstanceInput{.instanceIndex = 1u, .sceneInstanceId = 101u, .blasHandle = 7u, .flags = Scene::INSTANCE_FLAG_DYNAMIC},
  };

  const Scene::Plan initialPlan = scene.buildPlan(3u, sources, instances);
  assert(initialPlan.valid);
  assert(initialPlan.updateInstanceRangeIndices.size() == 2u);
  assert(initialPlan.updateTriangleCount == 6u);
  assert(initialPlan.updateBVH8NodeCount == 4u);
  scene.markUpdatesCompleted(initialPlan);

  instances[1u].transformDirty = true;
  const Scene::Plan movedDynamicPlan = scene.buildPlan(3u, sources, instances);
  assert(movedDynamicPlan.valid);
  assert(movedDynamicPlan.updateInstanceRangeIndices.size() == 1u);
  assert(movedDynamicPlan.updateInstanceRangeIndices[0u] == 1u);
  assert(movedDynamicPlan.updateTriangleCount == 3u);
  assert(movedDynamicPlan.updateBVH8NodeCount == 2u);
  assert(movedDynamicPlan.instances[0u].worldCWBVHNodes.offset == initialPlan.instances[0u].worldCWBVHNodes.offset);
  assert(movedDynamicPlan.instances[1u].worldCWBVHNodes.offset == initialPlan.instances[1u].worldCWBVHNodes.offset);
  scene.markUpdatesCompleted(movedDynamicPlan);

  instances[1u].transformDirty = false;
  const Scene::Plan cleanPlan = scene.buildPlan(3u, sources, instances);
  assert(cleanPlan.valid);
  assert(cleanPlan.updateInstanceRangeIndices.empty());

  Scene rebuiltGraphScene(settings);
  const Scene::Plan recompiledGraphPlan = rebuiltGraphScene.buildPlan(3u, sources, instances);
  assert(recompiledGraphPlan.valid);
  assert(recompiledGraphPlan.updateInstanceRangeIndices.size() == 2u);
  assert(recompiledGraphPlan.updateTriangleCount == 6u);
  assert(recompiledGraphPlan.updateBVH8NodeCount == 4u);
  assert(recompiledGraphPlan.instances[0u].worldCWBVHNodes.offset == initialPlan.instances[0u].worldCWBVHNodes.offset);
  assert(recompiledGraphPlan.instances[1u].worldCWBVHNodes.offset == initialPlan.instances[1u].worldCWBVHNodes.offset);
}

void updateJobBuildersRebaseLocalSourceRangesToWorldRanges()
{
  Scene::Settings settings{};
  settings.maxInstances = 4u;
  settings.maxSceneCWBVHNodes = 64u;
  settings.maxWorldBVH8Nodes = 16u;
  settings.maxWorldTriangles = 16u;
  settings.maxWorldLeafRemapEntries = 16u;
  settings.workgroupSize = 4u;
  Scene scene(settings);

  const std::vector<Scene::SourceBLAS> sources = {
      Scene::SourceBLAS{
          .handle = 10u,
          .localBVH8NodeOffset = 20u,
          .localTriangleOffset = 30u,
          .localLeafRemapOffset = 40u,
          .primitiveRemapOffset = 50u,
          .bvh8NodeCount = 2u,
          .cwbvhNodeCount = 2u,
          .triangleCount = 3u,
          .leafRemapCount = 3u,
      },
  };
  const std::vector<Scene::InstanceInput> instances = {
      Scene::InstanceInput{.instanceIndex = 2u, .sceneInstanceId = 100u, .blasHandle = 10u, .flags = Scene::INSTANCE_FLAG_DYNAMIC, .transformDirty = true},
  };

  Scene::Plan plan = scene.buildPlan(5u, sources, instances);
  assert(plan.valid);
  assert(plan.updateInstanceRangeIndices.size() == 1u);
  assert(plan.updateTriangleCount == 3u);
  assert(plan.updateBVH8NodeCount == 2u);
  assert(plan.updateCWBVHNodeCount == 2u);
  assert(plan.updateLeafRemapCount == 3u);

  const std::vector<Scene::WorldInstanceRangeGPU> gpuRanges = Scene::buildInstanceRangeGPU(plan);
  assert(gpuRanges.size() == 1u);
  assert(gpuRanges[0u].instanceIndex == 2u);
  assert(gpuRanges[0u].sceneInstanceId == 100u);
  assert(gpuRanges[0u].worldCWBVHNodeOffset == plan.instances[0u].worldCWBVHNodes.offset);
  assert(gpuRanges[0u].worldBVH8NodeOffset == 0u);
  assert(gpuRanges[0u].worldTriangleOffset == 0u);
  assert(gpuRanges[0u].worldLeafRemapOffset == 0u);
  assert(gpuRanges[0u].valid == 1u);
  assert(gpuRanges[0u].dynamic == 1u);
  assert(gpuRanges[0u].needsUpdate == 1u);
  assert(gpuRanges[0u].primitiveRemapOffset == 50u);

  const std::vector<Scene::TriangleUpdateItemGPU> triangleItems = Scene::buildTriangleUpdateItems(plan, sources);
  assert(triangleItems.size() == 3u);
  assert(triangleItems[0u].sourceTriangleIndex == 30u);
  assert(triangleItems[0u].worldTriangleIndex == 0u);
  assert(triangleItems[0u].instanceRangeIndex == 0u);
  assert(triangleItems[2u].sourceTriangleIndex == 32u);
  assert(triangleItems[2u].worldTriangleIndex == 2u);

  const std::vector<Scene::BVH8UpdateItemGPU> bvh8Items = Scene::buildBVH8UpdateItems(plan, sources);
  assert(bvh8Items.size() == 2u);
  assert(bvh8Items[0u].sourceNodeIndex == 20u);
  assert(bvh8Items[0u].worldNodeIndex == 0u);
  assert(bvh8Items[0u].instanceRangeIndex == 0u);
  assert(bvh8Items[0u].worldCWBVHNodeOffset == plan.instances[0u].worldCWBVHNodes.offset);
  assert(bvh8Items[0u].worldBVH8NodeOffset == 0u);
  assert(bvh8Items[0u].worldLeafRemapOffset == 0u);
  assert(bvh8Items[1u].sourceNodeIndex == 21u);
  assert(bvh8Items[1u].worldNodeIndex == 1u);

  const std::vector<Scene::LeafRemapUpdateItemGPU> leafItems = Scene::buildLeafRemapUpdateItems(plan, sources);
  assert(leafItems.size() == 3u);
  assert(leafItems[0u].sourceLeafRemapIndex == 40u);
  assert(leafItems[0u].worldLeafRemapIndex == 0u);
  assert(leafItems[0u].instanceRangeIndex == 0u);
  assert(leafItems[2u].sourceLeafRemapIndex == 42u);
  assert(leafItems[2u].worldLeafRemapIndex == 2u);
  assert(leafItems[2u].instanceRangeIndex == 0u);
}

void gpuUploadStructsMatchBVH8AndCWBVHCompression()
{
  std::vector<softwarebvh::CWBVHBuilder::PrimitiveBounds> primitives(4u);
  for (uint32_t primitiveIndex = 0u; primitiveIndex < static_cast<uint32_t>(primitives.size()); ++primitiveIndex)
  {
    const float x = static_cast<float>(primitiveIndex & 1u) * 2.0f;
    const float y = static_cast<float>(primitiveIndex >> 1u) * 2.0f;
    primitives[primitiveIndex].primitiveIndex = primitiveIndex;
    primitives[primitiveIndex].bounds.minPoint[0] = x;
    primitives[primitiveIndex].bounds.minPoint[1] = y;
    primitives[primitiveIndex].bounds.minPoint[2] = 0.25f * static_cast<float>(primitiveIndex);
    primitives[primitiveIndex].bounds.maxPoint[0] = x + 1.0f;
    primitives[primitiveIndex].bounds.maxPoint[1] = y + 1.5f;
    primitives[primitiveIndex].bounds.maxPoint[2] = 1.0f + 0.25f * static_cast<float>(primitiveIndex);
  }

  const softwarebvh::CWBVHBuilder::BuildResult build = softwarebvh::CWBVHBuilder::buildFromAabbs(primitives);
  const softwarebvh::CWBVHBuilder::CompressionResult compressed =
      softwarebvh::CWBVHBuilder::compressBVH8(build.bvh8Nodes, build.bvh8LeafRemap);
  assert(!build.bvh8Nodes.empty());
  assert(compressed.nodes.size() == build.bvh8Nodes.size());

  const Scene::BVH8NodeGPU gpuBVH8 = Scene::buildBVH8NodeGPU(build.bvh8Nodes[0u]);
  assert(gpuBVH8.cwbvhOutputIndex == build.bvh8Nodes[0u].cwbvhOutputIndex);
  assert(gpuBVH8.internalChildBase == build.bvh8Nodes[0u].internalChildBase);
  assert(gpuBVH8.leafBase == build.bvh8Nodes[0u].leafBase);
  assert(gpuBVH8.validChildMask == build.bvh8Nodes[0u].validChildMask);
  assert(gpuBVH8.internalChildMask == build.bvh8Nodes[0u].internalChildMask);
  assert(gpuBVH8.leafChildMask == build.bvh8Nodes[0u].leafChildMask);
  for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
  {
    assert(gpuBVH8.childIndex[childSlot] == build.bvh8Nodes[0u].childIndex[childSlot]);
    assert(gpuBVH8.childMeta[childSlot] == build.bvh8Nodes[0u].childMeta[childSlot]);
  }

  const Scene::PackedCWBVHNodeGPU gpuPacked = Scene::buildPackedCWBVHNodeGPU(compressed.nodes[0u]);
  for (uint32_t word = 0u; word < 2u; ++word)
  {
    assert(gpuPacked.orxy[word] == compressed.nodes[0u].orxy[word]);
    assert(gpuPacked.oeim[word] == compressed.nodes[0u].oeim[word]);
    assert(gpuPacked.ntbi[word] == compressed.nodes[0u].ntbi[word]);
    assert(gpuPacked.info[word] == compressed.nodes[0u].info[word]);
    assert(gpuPacked.qlox[word] == compressed.nodes[0u].qlox[word]);
    assert(gpuPacked.qloy[word] == compressed.nodes[0u].qloy[word]);
    assert(gpuPacked.qloz[word] == compressed.nodes[0u].qloz[word]);
    assert(gpuPacked.qhix[word] == compressed.nodes[0u].qhix[word]);
    assert(gpuPacked.qhiy[word] == compressed.nodes[0u].qhiy[word]);
    assert(gpuPacked.qhiz[word] == compressed.nodes[0u].qhiz[word]);
  }
}

void estimatedSettingsScaleWithReferencedInstances()
{
  const std::vector<Scene::SourceBLAS> sources = {
      Scene::SourceBLAS{.handle = 10u, .bvh8NodeCount = 3u, .cwbvhNodeCount = 3u, .triangleCount = 5u, .leafRemapCount = 5u},
      Scene::SourceBLAS{.handle = 11u, .bvh8NodeCount = 4u, .cwbvhNodeCount = 4u, .triangleCount = 6u, .leafRemapCount = 6u},
      Scene::SourceBLAS{.handle = 12u, .bvh8NodeCount = 1000u, .cwbvhNodeCount = 1000u, .triangleCount = 1000u, .leafRemapCount = 1000u},
  };
  const std::vector<Scene::InstanceInput> instances = {
      Scene::InstanceInput{.instanceIndex = 0u, .sceneInstanceId = 100u, .blasHandle = 10u},
      Scene::InstanceInput{.instanceIndex = 1u, .sceneInstanceId = 101u, .blasHandle = 10u},
      Scene::InstanceInput{.instanceIndex = 2u, .sceneInstanceId = 102u, .blasHandle = 11u},
      Scene::InstanceInput{.instanceIndex = 3u, .sceneInstanceId = 103u, .blasHandle = 99u},
  };

  const Scene::Settings settings = Scene::estimateSettings(7u, sources, instances, 32u);
  assert(settings.maxInstances == 4u);
  assert(settings.maxSceneCWBVHNodes == 17u);
  assert(settings.maxWorldBVH8Nodes == 10u);
  assert(settings.maxWorldTriangles == 16u);
  assert(settings.maxWorldLeafRemapEntries == 16u);
  assert(settings.workgroupSize == 32u);

  Scene scene(settings);
  const Scene::Plan plan = scene.buildPlan(7u, sources, instances);
  assert(plan.valid);
  assert(plan.sceneCWBVHNodeCount == 17u);
  assert(plan.worldBVH8NodeCount == 10u);
  assert(plan.worldTriangleCount == 16u);
  assert(plan.worldLeafRemapCount == 16u);
  assert(!plan.instances[3u].valid);
}

void sceneReportsCapacityFailures()
{
  Scene::Settings settings{};
  settings.maxInstances = 2u;
  settings.maxSceneCWBVHNodes = 8u;
  settings.maxWorldBVH8Nodes = 4u;
  settings.maxWorldTriangles = 4u;
  settings.maxWorldLeafRemapEntries = 4u;
  Scene scene(settings);

  const std::vector<Scene::SourceBLAS> sources = {
      Scene::SourceBLAS{.handle = 1u, .bvh8NodeCount = 4u, .cwbvhNodeCount = 4u, .triangleCount = 2u, .leafRemapCount = 2u},
  };
  const std::vector<Scene::InstanceInput> instances = {
      Scene::InstanceInput{.instanceIndex = 0u, .sceneInstanceId = 10u, .blasHandle = 1u, .flags = 0u},
      Scene::InstanceInput{.instanceIndex = 1u, .sceneInstanceId = 11u, .blasHandle = 1u, .flags = 0u},
  };

  Scene::Plan overlapPlan = scene.buildPlan(2u, sources, instances);
  assert(!overlapPlan.valid);
  assert(overlapPlan.error.find("overlaps") != std::string::npos);

  Scene::Plan tlasPlan = scene.buildPlan(9u, sources, {});
  assert(!tlasPlan.valid);
  assert(tlasPlan.error.find("TLAS") != std::string::npos);

  const std::vector<Scene::SourceBLAS> mismatchedSources = {
      Scene::SourceBLAS{.handle = 2u, .bvh8NodeCount = 2u, .cwbvhNodeCount = 3u, .triangleCount = 1u, .leafRemapCount = 1u},
  };
  const std::vector<Scene::InstanceInput> mismatchedInstances = {
      Scene::InstanceInput{.instanceIndex = 0u, .sceneInstanceId = 20u, .blasHandle = 2u},
  };
  Scene::Plan mismatchPlan = scene.buildPlan(1u, mismatchedSources, mismatchedInstances);
  assert(!mismatchPlan.valid);
  assert(mismatchPlan.error.find("match BVH8") != std::string::npos);
}

} // namespace

int main()
{
  sceneArenaAllocatesTlasLeftAndBlasRight();
  sceneBakeStateSkipsStaticAndQueuesDirtyDynamic();
  sceneRecompileRequeuesWorldCwbvhBakeAfterInstanceUpdates();
  updateJobBuildersRebaseLocalSourceRangesToWorldRanges();
  gpuUploadStructsMatchBVH8AndCWBVHCompression();
  estimatedSettingsScaleWithReferencedInstances();
  sceneReportsCapacityFailures();
  return 0;
}
