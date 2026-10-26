#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelOccupancyVolume.hpp"
#include "voxel/svraster/SparseVoxelSceneManager.hpp"
#include "voxel/svraster/rendering/SparseVoxelBootstrapPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelLODPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancySourcePass.hpp"
#include "voxel/svraster/rendering/SparseVoxelTLASCullingPass.hpp"

#include <algorithm>
#include <array>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

std::array<float, 16> transform(const float x, const float y, const float z, const float sx = 1.0f, const float sy = 1.0f, const float sz = 1.0f)
{
  return {sx, 0.0f, 0.0f, 0.0f, 0.0f, sy, 0.0f, 0.0f, 0.0f, 0.0f, sz, 0.0f, x, y, z, 1.0f};
}

std::array<float, 3> transformPoint(const std::array<float, 16> &matrix, const std::array<float, 3> &point)
{
  return {
    matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
    matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
    matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14],
  };
}

void expectIdentityProduct(const std::array<float, 16> &left, const std::array<float, 16> &right)
{
  for (uint32_t column = 0u; column < 4u; ++column)
    for (uint32_t row = 0u; row < 4u; ++row)
    {
      float value = 0.0f;
      for (uint32_t inner = 0u; inner < 4u; ++inner)
        value += left[inner * 4u + row] * right[column * 4u + inner];
      assert(std::abs(value - (row == column ? 1.0f : 0.0f)) < 1.0e-5f);
    }
}

void testOneInstanceDerivedStateAndRevisions()
{
  SparseVoxelScene scene = makeExteriorShellScene(17u);
  scene.instances[0].modelToWorld = transform(3.0f, -2.0f, 7.0f, -2.0f, 3.0f, 0.5f);
  SparseVoxelSceneManager manager;
  std::string error;
  assert(manager.setScene(scene, &error));
  assert(error.empty());
  assert(manager.sceneRevision() == 1u && manager.topologyRevision() == 1u && manager.transformRevision() == 1u);
  assert(manager.runtimeInstances().size() == 1u);
  const auto &instance = manager.runtimeInstances()[0];
  assert(instance.stableId == 0u && instance.gpu.blasIndex == 0u);
  expectIdentityProduct(instance.gpu.modelToWorld, instance.gpu.worldToModel);
  assert(instance.gpu.worldBoundsMin[0] == 1.0f && instance.gpu.worldBoundsMax[0] == 5.0f);
  assert(instance.gpu.worldBoundsMin[1] == -5.0f && instance.gpu.worldBoundsMax[1] == 1.0f);
  assert(instance.gpu.worldBoundsMin[2] == 6.5f && instance.gpu.worldBoundsMax[2] == 7.5f);
  assert(instance.gpu.previousModelToWorld == instance.gpu.modelToWorld);
  assert(manager.blasRootRanges().size() == 1u);
  assert(manager.blasRootRanges()[0].firstRoot == scene.models[0].firstRoot);
  assert(manager.blasRootRanges()[0].rootCount == scene.models[0].rootCount);
  assert(manager.tlas().nodes.size() == 1u && manager.tlas().rootNode == 0u && manager.tlas().maxDepth == 1u);
  assert(manager.tlas().instanceIndices == std::vector<uint32_t>{0u});
  assert(validateSparseVoxelTLAS(manager, &error));

  const auto moved = transform(-4.0f, 1.0f, 5.0f, 1.0f, 2.0f, 3.0f);
  assert(manager.setInstanceTransform(0u, moved, &error));
  assert(manager.sceneRevision() == 2u && manager.topologyRevision() == 1u && manager.transformRevision() == 2u);
  assert(manager.runtimeInstances()[0].gpu.previousModelToWorld == scene.instances[0].modelToWorld);
  manager.commitPreviousTransforms();
  assert(manager.runtimeInstances()[0].gpu.previousModelToWorld == moved);
  assert(!manager.setInstanceTransform(1u, moved, &error));
  assert(!error.empty());
}

void testEmptyScene()
{
  SparseVoxelScene scene{};
  SparseVoxelSceneManager manager;
  std::string error;
  assert(manager.setScene(scene, &error));
  assert(manager.runtimeInstances().empty());
  assert(manager.tlas().nodes.empty());
  assert(manager.tlas().rootNode == kInvalidSparseVoxelIndex);
  assert(manager.tlas().maxDepth == 0u);
  assert(validateSparseVoxelTLAS(manager, &error));
}

void testDeterministicBreadthFirstBVH8()
{
  SparseVoxelScene scene = makeExteriorShellScene(23u);
  scene.instances.clear();
  scene.tlasFanout = 8u;
  scene.tlasLeafSize = 3u;
  constexpr uint32_t instanceCount = 137u;
  for (uint32_t index = 0u; index < instanceCount; ++index)
  {
    const float x = static_cast<float>(static_cast<int32_t>((index * 37u) % 31u) - 15);
    const float y = static_cast<float>(static_cast<int32_t>((index * 19u) % 23u) - 11);
    const float z = 10.0f + static_cast<float>((index * 11u) % 17u);
    scene.instances.push_back({0u, 0u, transform(x, y, z, 0.5f + 0.1f * static_cast<float>(index % 5u), 1.0f, 0.75f)});
  }
  SparseVoxelSceneManager first;
  SparseVoxelSceneManager second;
  std::string error;
  assert(first.setScene(scene, &error));
  assert(second.setScene(scene, &error));
  assert(first.tlas().maxDepth >= 3u);
  assert(first.tlas().nodes.size() == second.tlas().nodes.size());
  assert(first.tlas().instanceIndices == second.tlas().instanceIndices);
  assert(std::memcmp(first.tlas().nodes.data(), second.tlas().nodes.data(), first.tlas().nodes.size() * sizeof(SparseVoxelTLASNodeGPU)) == 0);
  assert(validateSparseVoxelTLAS(first, &error));

  std::vector<uint32_t> membership(instanceCount, 0u);
  for (const uint32_t instance : first.tlas().instanceIndices)
  {
    assert(instance < instanceCount);
    ++membership[instance];
  }
  assert(
      std::all_of(
          membership.begin(),
          membership.end(),
          [](const uint32_t count)
          {
            return count == 1u;
          }));
  for (const auto &node : first.tlas().nodes)
  {
    assert(node.childCount <= 8u);
    assert(node.instanceCount <= 3u);
    if (node.childCount != 0u)
      assert(node.firstChild + node.childCount <= first.tlas().nodes.size());
  }
}

void testAffineRotationBounds()
{
  SparseVoxelScene scene = makeExteriorShellScene(31u);
  // Column-major: 90-degree Z rotation, non-uniform scale and translation.
  scene.instances[0].modelToWorld = {0.0f, 2.0f, 0.0f, 0.0f, -3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 4.0f, 5.0f, 6.0f, 1.0f};
  SparseVoxelSceneManager manager;
  std::string error;
  assert(manager.setScene(scene, &error));
  const auto &gpu = manager.runtimeInstances()[0].gpu;
  expectIdentityProduct(gpu.modelToWorld, gpu.worldToModel);
  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    const auto world = transformPoint(
        gpu.modelToWorld,
        {
          (corner & 1u) != 0u ? 1.0f : -1.0f,
          (corner & 2u) != 0u ? 1.0f : -1.0f,
          (corner & 4u) != 0u ? 1.0f : -1.0f,
        });
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      assert(world[axis] >= gpu.worldBoundsMin[axis] && world[axis] <= gpu.worldBoundsMax[axis]);
  }
}

void runG10()
{
  testOneInstanceDerivedStateAndRevisions();
  testEmptyScene();
  testDeterministicBreadthFirstBVH8();
  testAffineRotationBounds();
}

SparseVoxelRasterUniformsGPU cullingCamera()
{
  SparseVoxelRasterUniformsGPU raster{};
  raster.worldToCamera = transform(0.0f, 0.0f, 0.0f);
  raster.cameraToWorld = raster.worldToCamera;
  raster.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.1f};
  raster.focalPrincipal = {32.0f, 32.0f, 32.0f, 32.0f};
  raster.viewportTiles = {64u, 64u, 4u, 4u};
  return raster;
}

bool cpuFrustumVisible(const SparseVoxelInstanceGPU &instance, const SparseVoxelRasterUniformsGPU &raster)
{
  bool outsideLeft = true;
  bool outsideRight = true;
  bool outsideTop = true;
  bool outsideBottom = true;
  bool outsideNear = true;
  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    const std::array<float, 3> world{
      (corner & 1u) != 0u ? instance.worldBoundsMax[0] : instance.worldBoundsMin[0],
      (corner & 2u) != 0u ? instance.worldBoundsMax[1] : instance.worldBoundsMin[1],
      (corner & 4u) != 0u ? instance.worldBoundsMax[2] : instance.worldBoundsMin[2],
    };
    const auto view = transformPoint(raster.worldToCamera, world);
    const float left = raster.focalPrincipal[0] * view[0] + raster.focalPrincipal[2] * view[2];
    const float right = raster.focalPrincipal[0] * view[0] + (raster.focalPrincipal[2] - static_cast<float>(raster.viewportTiles[0])) * view[2];
    const float top = raster.focalPrincipal[1] * view[1] + raster.focalPrincipal[3] * view[2];
    const float bottom = raster.focalPrincipal[1] * view[1] + (raster.focalPrincipal[3] - static_cast<float>(raster.viewportTiles[1])) * view[2];
    outsideLeft &= left < 0.0f;
    outsideRight &= right > 0.0f;
    outsideTop &= top < 0.0f;
    outsideBottom &= bottom > 0.0f;
    outsideNear &= view[2] < raster.cameraWorldPositionNear[3];
  }
  return !(outsideLeft || outsideRight || outsideTop || outsideBottom || outsideNear);
}

struct TLASReadback
{
  SparseVoxelFrameCountersGPU counters{};
  std::vector<VisibleInstanceGPU> visible;
  std::vector<SparseVoxelLODSeedGPU> seeds;
  std::vector<uint32_t> flags;
  std::vector<IndirectDispatchArgsGPU> dispatch;
  std::vector<uint32_t> reasons;
};

struct TLASOcclusionFixture
{
  bool enabled = false;
  bool reverseZ = true;
  bool historyValid = false;
  bool previousFrame = false;
  bool recovery = false;
  bool prepassGuarantee = false;
  float hzbDepth = 0.0f;
  float depthBias = 1.0e-5f;
  int32_t mipBias = 0;
  std::vector<uint32_t> preSubmitted;
  bool occupancyQuery = false;
  std::vector<SparseVoxelAABB> occupancyQueryBoxes;
};

TLASReadback runTLASCulling(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const SparseVoxelSceneManager &manager,
    const SparseVoxelRasterUniformsGPU &raster,
    const uint32_t visibleCapacity,
    const uint32_t queueCapacity,
    const TLASOcclusionFixture occlusion = {})
{
  using namespace ::rendering;
  RenderGraph graph(&rhi);
  const uint32_t instanceCount = static_cast<uint32_t>(manager.runtimeInstances().size());
  const uint32_t nodeCount = static_cast<uint32_t>(manager.tlas().nodes.size());
  const uint32_t maxDepth = std::max(1u, manager.tlas().maxDepth);
  const uint32_t blockCount = std::max(1u, (instanceCount + kSparseVoxelTLASWorkgroupSize - 1u) / kSparseVoxelTLASWorkgroupSize);
  auto storage = [&](const char *name, const uint64_t size, const bool indirect = false)
  {
    return graph.createBuffer({
      .name = name,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc | (indirect ? BufferUsage::BufferUsage_Indirect : BufferUsage::BufferUsage_None),
    });
  };
  const Buffer counters = storage("g11.counters", sizeof(SparseVoxelFrameCountersGPU));
  const Buffer instances = storage("g11.instances", uint64_t{instanceCount} * sizeof(SparseVoxelInstanceGPU));
  const Buffer nodes = storage("g11.nodes", uint64_t{nodeCount} * sizeof(SparseVoxelTLASNodeGPU));
  const Buffer instanceIndices = storage("g11.instanceIndices", uint64_t{instanceCount} * sizeof(uint32_t));
  const Buffer blasRoots = storage("g11.blasRoots", uint64_t{manager.blasRootRanges().size()} * sizeof(SparseVoxelBLASRootRangeGPU));
  const Buffer queueA = storage("g11.queueA", uint64_t{queueCapacity} * sizeof(TLASQueueElementGPU));
  const Buffer queueB = storage("g11.queueB", uint64_t{queueCapacity} * sizeof(TLASQueueElementGPU));
  const Buffer queueCounts = storage("g11.queueCounts", uint64_t{maxDepth} * sizeof(uint32_t));
  const Buffer dispatch = storage("g11.dispatch", uint64_t{maxDepth} * sizeof(IndirectDispatchArgsGPU), true);
  const Buffer flags = storage("g11.flags", uint64_t{instanceCount} * sizeof(uint32_t));
  const Buffer offsets = storage("g11.offsets", uint64_t{instanceCount} * sizeof(SparseVoxelVisibilityOffsetGPU));
  const Buffer blocks = storage("g11.blocks", uint64_t{blockCount} * sizeof(SparseVoxelTLASScanBlockGPU));
  const Buffer visible = storage("g11.visible", uint64_t{visibleCapacity} * sizeof(VisibleInstanceGPU));
  const Buffer seeds = storage("g11.seeds", uint64_t{visibleCapacity} * sizeof(SparseVoxelLODSeedGPU));
  const Buffer preSubmitted = storage("g21.preSubmitted", uint64_t{instanceCount} * sizeof(uint32_t));
  const Buffer reasons = storage("g21.reasons", uint64_t{instanceCount} * sizeof(uint32_t));
  const Buffer countersReadback = graph.createBuffer({.name = "g11.counters.read", .size = sizeof(SparseVoxelFrameCountersGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer visibleReadback = graph.createBuffer(
      {.name = "g11.visible.read",
       .size = std::max<uint64_t>(sizeof(VisibleInstanceGPU), uint64_t{visibleCapacity} * sizeof(VisibleInstanceGPU)),
       .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer seedsReadback = graph.createBuffer(
      {.name = "g11.seeds.read",
       .size = std::max<uint64_t>(sizeof(SparseVoxelLODSeedGPU), uint64_t{visibleCapacity} * sizeof(SparseVoxelLODSeedGPU)),
       .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer flagsReadback =
      graph.createBuffer({.name = "g11.flags.read", .size = std::max<uint64_t>(sizeof(uint32_t), uint64_t{instanceCount} * sizeof(uint32_t)), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer dispatchReadback =
      graph.createBuffer({.name = "g11.dispatch.read", .size = uint64_t{maxDepth} * sizeof(IndirectDispatchArgsGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer reasonsReadback =
      graph.createBuffer({.name = "g21.reasons.read", .size = std::max<uint64_t>(sizeof(uint32_t), uint64_t{instanceCount} * sizeof(uint32_t)), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Texture hzb = graph.createTexture({
    .name = "g21.hzb",
    .format = Format::Format_R32Float,
    .height = 16u,
    .width = 16u,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  SparseVoxelFrameCountersGPU zeroCounters{};
  graph.bufferWrite(counters, 0u, sizeof(zeroCounters), &zeroCounters);
  std::vector<SparseVoxelInstanceGPU> gpuInstances;
  gpuInstances.reserve(instanceCount);
  for (const auto &instance : manager.runtimeInstances())
    gpuInstances.push_back(instance.gpu);
  if (!gpuInstances.empty())
    graph.bufferWrite(instances, 0u, uint64_t{instanceCount} * sizeof(gpuInstances[0]), gpuInstances.data());
  if (!manager.tlas().nodes.empty())
    graph.bufferWrite(nodes, 0u, uint64_t{nodeCount} * sizeof(SparseVoxelTLASNodeGPU), const_cast<SparseVoxelTLASNodeGPU *>(manager.tlas().nodes.data()));
  if (!manager.tlas().instanceIndices.empty())
    graph.bufferWrite(instanceIndices, 0u, uint64_t{instanceCount} * sizeof(uint32_t), const_cast<uint32_t *>(manager.tlas().instanceIndices.data()));
  if (!manager.blasRootRanges().empty())
    graph.bufferWrite(blasRoots, 0u, manager.blasRootRanges().size() * sizeof(SparseVoxelBLASRootRangeGPU), const_cast<SparseVoxelBLASRootRangeGPU *>(manager.blasRootRanges().data()));
  std::vector<uint32_t> submitted(instanceCount, 0u);
  std::copy_n(occlusion.preSubmitted.begin(), std::min(submitted.size(), occlusion.preSubmitted.size()), submitted.begin());
  if (!submitted.empty())
    graph.bufferWrite(preSubmitted, 0u, submitted.size() * sizeof(uint32_t), submitted.data());

  SparseVoxelTLASCullingUniformsGPU culling{};
  culling.instanceCount = instanceCount;
  culling.nodeCount = nodeCount;
  culling.rootNode = manager.tlas().rootNode;
  culling.maxDepth = maxDepth;
  culling.queueCapacity = queueCapacity;
  culling.visibleInstanceCapacity = visibleCapacity;
  culling.lodQueueCapacity = instanceCount * 8u;
  culling.scanBlockCount = blockCount;
  culling.blasCount = static_cast<uint32_t>(manager.blasRootRanges().size());
  culling.cullingFlags = occlusion.occupancyQuery ? kSparseVoxelCullingOccupancyQuery : kSparseVoxelCullingFrustum;
  if (occlusion.enabled)
    culling.cullingFlags |= kSparseVoxelCullingOcclusion;
  if (occlusion.reverseZ)
    culling.cullingFlags |= kSparseVoxelCullingReverseZ;
  if (occlusion.previousFrame)
    culling.cullingFlags |= kSparseVoxelCullingPreviousFrame;
  if (occlusion.recovery)
    culling.cullingFlags |= kSparseVoxelCullingRecovery;
  if (occlusion.prepassGuarantee)
    culling.cullingFlags |= kSparseVoxelCullingPrepassGuarantee;
  culling.hzbWidth = 16u;
  culling.hzbHeight = 16u;
  culling.hzbMipCount = 1u;
  culling.hzbFirstValidMip = 0u;
  culling.occlusionDepthBias = occlusion.depthBias;
  culling.occlusionMipBias = occlusion.mipBias;
  culling.historyValid = occlusion.historyValid ? 1u : 0u;
  culling.occupancyQueryBoxCount = static_cast<uint32_t>(std::min<size_t>(occlusion.occupancyQueryBoxes.size(), culling.occupancyQueryMinimum.size()));
  for (uint32_t queryIndex = 0u; queryIndex < culling.occupancyQueryBoxCount; ++queryIndex)
  {
    std::copy(occlusion.occupancyQueryBoxes[queryIndex].minimum.begin(), occlusion.occupancyQueryBoxes[queryIndex].minimum.end(), culling.occupancyQueryMinimum[queryIndex].begin());
    std::copy(occlusion.occupancyQueryBoxes[queryIndex].maximum.begin(), occlusion.occupancyQueryBoxes[queryIndex].maximum.end(), culling.occupancyQueryMaximum[queryIndex].begin());
  }
  const svrRendering::SparseVoxelTLASCullingPass::Buffers passBuffers{
    counters,
    instances,
    nodes,
    instanceIndices,
    blasRoots,
    queueA,
    queueB,
    queueCounts,
    dispatch,
    flags,
    offsets,
    blocks,
    visible,
    seeds,
    preSubmitted,
    reasons,
  };
  uint32_t passIndex = 0u;
  auto add = [&](const std::string &name, const svrRendering::SparseVoxelTLASCullingPass::Kernel kernel, const uint32_t depth)
  {
    culling.depth = depth;
    graph.registerPass<svrRendering::SparseVoxelTLASCullingPass>(
        name, passIndex++, kernel, passBuffers, svrRendering::SparseVoxelTLASCullingPass::Settings{raster, culling}, svrRendering::SparseVoxelTLASCullingPass::OcclusionResources{hzb});
  };
  add("g11.initialize", svrRendering::SparseVoxelTLASCullingPass::Kernel::Initialize, 0u);
  for (uint32_t depth = 0u; depth < manager.tlas().maxDepth; ++depth)
  {
    add("g11.traverse." + std::to_string(depth), svrRendering::SparseVoxelTLASCullingPass::Kernel::Traverse, depth);
    if (depth + 1u < manager.tlas().maxDepth)
      add("g11.finalize." + std::to_string(depth), svrRendering::SparseVoxelTLASCullingPass::Kernel::FinalizeLevel, depth);
  }
  add("g11.scan", svrRendering::SparseVoxelTLASCullingPass::Kernel::ScanVisibility, 0u);
  add("g11.scanBlocks", svrRendering::SparseVoxelTLASCullingPass::Kernel::ScanBlockSums, 0u);
  add("g11.scatter", svrRendering::SparseVoxelTLASCullingPass::Kernel::ScatterVisibility, 0u);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("g11.copyCounters", passIndex++, counters, 0u, sizeof(SparseVoxelFrameCountersGPU), countersReadback, 0u, sizeof(SparseVoxelFrameCountersGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g11.copyVisible",
      passIndex++,
      visible,
      0u,
      std::max<uint64_t>(sizeof(VisibleInstanceGPU), uint64_t{visibleCapacity} * sizeof(VisibleInstanceGPU)),
      visibleReadback,
      0u,
      std::max<uint64_t>(sizeof(VisibleInstanceGPU), uint64_t{visibleCapacity} * sizeof(VisibleInstanceGPU)));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g11.copySeeds",
      passIndex++,
      seeds,
      0u,
      std::max<uint64_t>(sizeof(SparseVoxelLODSeedGPU), uint64_t{visibleCapacity} * sizeof(SparseVoxelLODSeedGPU)),
      seedsReadback,
      0u,
      std::max<uint64_t>(sizeof(SparseVoxelLODSeedGPU), uint64_t{visibleCapacity} * sizeof(SparseVoxelLODSeedGPU)));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g11.copyFlags",
      passIndex++,
      flags,
      0u,
      std::max<uint64_t>(sizeof(uint32_t), uint64_t{instanceCount} * sizeof(uint32_t)),
      flagsReadback,
      0u,
      std::max<uint64_t>(sizeof(uint32_t), uint64_t{instanceCount} * sizeof(uint32_t)));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g11.copyDispatch", passIndex++, dispatch, 0u, uint64_t{maxDepth} * sizeof(IndirectDispatchArgsGPU), dispatchReadback, 0u, uint64_t{maxDepth} * sizeof(IndirectDispatchArgsGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g21.copyReasons",
      passIndex++,
      reasons,
      0u,
      std::max<uint64_t>(sizeof(uint32_t), uint64_t{instanceCount} * sizeof(uint32_t)),
      reasonsReadback,
      0u,
      std::max<uint64_t>(sizeof(uint32_t), uint64_t{instanceCount} * sizeof(uint32_t)));
  graph.compile();
  std::vector<float> hzbPixels(16u * 16u, occlusion.hzbDepth);
  graph.textureWrite(hzb, hzbPixels.size() * sizeof(float), hzbPixels.data());
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  TLASReadback result{};
  graph.bufferRead(
      countersReadback,
      0u,
      sizeof(result.counters),
      [&](const void *data)
      {
        std::memcpy(&result.counters, data, sizeof(result.counters));
      });
  result.visible.resize(visibleCapacity);
  result.seeds.resize(visibleCapacity);
  result.flags.resize(instanceCount);
  result.dispatch.resize(maxDepth);
  result.reasons.resize(instanceCount);
  if (visibleCapacity != 0u)
  {
    graph.bufferRead(
        visibleReadback,
        0u,
        uint64_t{visibleCapacity} * sizeof(VisibleInstanceGPU),
        [&](const void *data)
        {
          std::memcpy(result.visible.data(), data, uint64_t{visibleCapacity} * sizeof(VisibleInstanceGPU));
        });
    graph.bufferRead(
        seedsReadback,
        0u,
        uint64_t{visibleCapacity} * sizeof(SparseVoxelLODSeedGPU),
        [&](const void *data)
        {
          std::memcpy(result.seeds.data(), data, uint64_t{visibleCapacity} * sizeof(SparseVoxelLODSeedGPU));
        });
  }
  if (instanceCount != 0u)
    graph.bufferRead(
        flagsReadback,
        0u,
        uint64_t{instanceCount} * sizeof(uint32_t),
        [&](const void *data)
        {
          std::memcpy(result.flags.data(), data, uint64_t{instanceCount} * sizeof(uint32_t));
        });
  graph.bufferRead(
      dispatchReadback,
      0u,
      uint64_t{maxDepth} * sizeof(IndirectDispatchArgsGPU),
      [&](const void *data)
      {
        std::memcpy(result.dispatch.data(), data, uint64_t{maxDepth} * sizeof(IndirectDispatchArgsGPU));
      });
  if (instanceCount != 0u)
    graph.bufferRead(
        reasonsReadback,
        0u,
        uint64_t{instanceCount} * sizeof(uint32_t),
        [&](const void *data)
        {
          std::memcpy(result.reasons.data(), data, uint64_t{instanceCount} * sizeof(uint32_t));
        });
  return result;
}

void expectSameTLASReadback(
    const TLASReadback &left,
    const TLASReadback &right)
{
  assert(std::memcmp(
             &left.counters, &right.counters,
             sizeof(SparseVoxelFrameCountersGPU)) == 0);
  assert(left.flags == right.flags);
  assert(left.reasons == right.reasons);
  assert(left.dispatch.size() == right.dispatch.size());
  assert(std::memcmp(
             left.dispatch.data(), right.dispatch.data(),
             left.dispatch.size() * sizeof(IndirectDispatchArgsGPU)) == 0);
  const size_t visibleCount = std::min<size_t>(
      left.counters.visibleInstanceCount, left.visible.size());
  assert(visibleCount <= right.visible.size());
  assert(std::memcmp(
             left.visible.data(), right.visible.data(),
             visibleCount * sizeof(VisibleInstanceGPU)) == 0);
  assert(std::memcmp(
             left.seeds.data(), right.seeds.data(),
             visibleCount * sizeof(SparseVoxelLODSeedGPU)) == 0);
}

SparseVoxelScene makeCullingScene()
{
  SparseVoxelScene scene = makeExteriorShellScene(41u);
  scene.instances.clear();
  scene.tlasFanout = 4u;
  scene.tlasLeafSize = 2u;
  const std::array<std::array<float, 3>, 13> positions{{
    {0.0f, 0.0f, 5.0f},
    {-6.0f, 0.0f, 5.0f},
    {-7.5f, 0.0f, 5.0f},
    {6.0f, 0.0f, 5.0f},
    {7.5f, 0.0f, 5.0f},
    {0.0f, -6.0f, 5.0f},
    {0.0f, -7.5f, 5.0f},
    {0.0f, 6.0f, 5.0f},
    {0.0f, 7.5f, 5.0f},
    {0.0f, 0.0f, 0.05f},
    {0.0f, 0.0f, -2.0f},
    {2.0f, 1.0f, 12.0f},
    {-40.0f, 30.0f, 8.0f},
  }};
  for (const auto &position : positions)
    scene.instances.push_back({0u, 0u, transform(position[0], position[1], position[2])});
  return scene;
}

bool intersectsAny(const SparseVoxelInstanceGPU &instance, const std::vector<SparseVoxelAABB> &queryBoxes)
{
  for (const SparseVoxelAABB &query : queryBoxes)
  {
    bool intersects = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      intersects &= instance.worldBoundsMax[axis] >= query.minimum[axis] && query.maximum[axis] >= instance.worldBoundsMin[axis];
    if (intersects)
      return true;
  }
  return false;
}

std::vector<uint32_t> expectedOccupancyInstances(const SparseVoxelSceneManager &manager, const std::vector<SparseVoxelAABB> &queryBoxes)
{
  std::vector<uint32_t> expected;
  for (const SparseVoxelRuntimeInstance &instance : manager.runtimeInstances())
    if (intersectsAny(instance.gpu, queryBoxes))
      expected.push_back(instance.stableId);
  return expected;
}

void expectOccupancyTLASQuery(::rendering::backend::vulkan::VulkanRHI &rhi, const SparseVoxelSceneManager &manager, const SparseVoxelRasterUniformsGPU &raster, const std::vector<SparseVoxelAABB> &queryBoxes)
{
  TLASOcclusionFixture fixture{};
  fixture.occupancyQuery = true;
  fixture.occupancyQueryBoxes = queryBoxes;
  const std::vector<uint32_t> expected = expectedOccupancyInstances(manager, queryBoxes);
  const TLASReadback result = runTLASCulling(rhi, manager, raster, static_cast<uint32_t>(manager.runtimeInstances().size()), static_cast<uint32_t>(manager.tlas().nodes.size()), fixture);
  assert(result.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::None));
  if (result.counters.visibleInstanceCount != expected.size())
  {
    std::cerr << "Occupancy TLAS query mismatch: expected " << expected.size() << ", actual " << result.counters.visibleInstanceCount << ", boxes " << queryBoxes.size() << '\n';
    for (uint32_t queryIndex = 0u; queryIndex < queryBoxes.size(); ++queryIndex)
      std::cerr << "  box " << queryIndex << ": [" << queryBoxes[queryIndex].minimum[0] << ", " << queryBoxes[queryIndex].minimum[1] << ", " << queryBoxes[queryIndex].minimum[2] << "] -> ["
                << queryBoxes[queryIndex].maximum[0] << ", " << queryBoxes[queryIndex].maximum[1] << ", " << queryBoxes[queryIndex].maximum[2] << "]\n";
  }
  assert(result.counters.visibleInstanceCount == expected.size());
  for (uint32_t output = 0u; output < expected.size(); ++output)
  {
    assert(result.visible[output].instanceIndex == expected[output]);
    assert(result.seeds[output].instanceIndex == expected[output]);
  }
  for (uint32_t instance = 0u; instance < result.flags.size(); ++instance)
    assert((result.flags[instance] != 0u) == std::binary_search(expected.begin(), expected.end(), instance));
}

void testGPUOccupancyVolumeQuery(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelSceneManager manager;
  std::string error;
  assert(manager.setScene(makeCullingScene(), &error));
  const SparseVoxelRasterUniformsGPU raster = cullingCamera();

  SparseVoxelOccupancySettings settings{};
  settings.worldExtent = {16.0f, 16.0f, 16.0f};
  settings.logicalResolution = {16u, 16u, 16u};
  settings.memoryCeilingBytes = 1u << 20u;
  SparseVoxelOccupancyVolume volume;
  assert(volume.configure(settings, {}, &error));

  const std::vector<SparseVoxelAABB> noSources;
  assert(volume.update({0.0f, 0.0f, 5.0f}, 1u, noSources, &error));
  std::vector<SparseVoxelAABB> queryBoxes = sparseVoxelOccupancyQueryBoxes(volume.state());
  assert(queryBoxes.size() == 1u);
  const std::vector<uint32_t> fullExpected = expectedOccupancyInstances(manager, queryBoxes);
  assert(!fullExpected.empty());
  assert(
      std::any_of(
          fullExpected.begin(),
          fullExpected.end(),
          [&](const uint32_t instanceIndex)
          {
            return !cpuFrustumVisible(manager.runtimeInstances()[instanceIndex].gpu, raster);
          }));
  expectOccupancyTLASQuery(rhi, manager, raster, queryBoxes);

  assert(volume.update({1.0f, 0.0f, 5.0f}, 1u, noSources, &error));
  queryBoxes = sparseVoxelOccupancyQueryBoxes(volume.state());
  assert((queryBoxes.size() == 1u && volume.state().scrollDelta == std::array<int32_t, 3>{1, 0, 0}));
  const std::vector<uint32_t> xSlabExpected = expectedOccupancyInstances(manager, queryBoxes);
  assert(!xSlabExpected.empty() && xSlabExpected.size() < fullExpected.size());
  expectOccupancyTLASQuery(rhi, manager, raster, queryBoxes);

  assert(volume.update({2.0f, 1.0f, 6.0f}, 1u, noSources, &error));
  queryBoxes = sparseVoxelOccupancyQueryBoxes(volume.state());
  assert((queryBoxes.size() == 3u && volume.state().scrollDelta == std::array<int32_t, 3>{1, 1, 1}));
  expectOccupancyTLASQuery(rhi, manager, raster, queryBoxes);

  assert(volume.update({2.25f, 1.25f, 6.25f}, 1u, noSources, &error));
  queryBoxes = sparseVoxelOccupancyQueryBoxes(volume.state());
  assert(queryBoxes.empty());
  expectOccupancyTLASQuery(rhi, manager, raster, queryBoxes);
}

void testGPUFrustumAndStableCompaction(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelSceneManager manager;
  std::string error;
  assert(manager.setScene(makeCullingScene(), &error));
  const SparseVoxelRasterUniformsGPU raster = cullingCamera();
  std::vector<uint32_t> expected;
  for (const auto &instance : manager.runtimeInstances())
    if (cpuFrustumVisible(instance.gpu, raster))
      expected.push_back(instance.stableId);
  assert(expected.size() >= 6u && expected.size() < manager.runtimeInstances().size());

  const TLASReadback result = runTLASCulling(rhi, manager, raster, static_cast<uint32_t>(manager.runtimeInstances().size()), static_cast<uint32_t>(manager.tlas().nodes.size()));
  assert(result.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::None));
  assert(result.counters.visibleInstanceCount == expected.size());
  uint32_t rootOffset = 0u;
  for (uint32_t output = 0u; output < expected.size(); ++output)
  {
    assert(result.visible[output].instanceIndex == expected[output]);
    assert(result.visible[output].firstVisibleVoxel == 0u && result.visible[output].visibleVoxelCount == 0u);
    assert(result.seeds[output].instanceIndex == expected[output]);
    assert(result.seeds[output].firstRoot == manager.blasRootRanges()[0].firstRoot);
    assert(result.seeds[output].rootCount == manager.blasRootRanges()[0].rootCount);
    assert(result.seeds[output].firstQueueEntry == rootOffset);
    rootOffset += result.seeds[output].rootCount;
  }
  for (uint32_t instance = 0u; instance < result.flags.size(); ++instance)
    assert((result.flags[instance] != 0u) == std::binary_search(expected.begin(), expected.end(), instance));
  assert(result.dispatch[0].x == 1u);
  for (const auto &args : result.dispatch)
  {
    assert(args.y == 1u && args.z == 1u && args.reserved == 0u);
    assert(args.x <= (manager.tlas().nodes.size() + kSparseVoxelTLASWorkgroupSize - 1u) / kSparseVoxelTLASWorkgroupSize);
  }

  const TLASReadback repeat = runTLASCulling(rhi, manager, raster, static_cast<uint32_t>(manager.runtimeInstances().size()), static_cast<uint32_t>(manager.tlas().nodes.size()));
  assert(repeat.counters.visibleInstanceCount == result.counters.visibleInstanceCount);
  assert(std::memcmp(repeat.visible.data(), result.visible.data(), expected.size() * sizeof(VisibleInstanceGPU)) == 0);
  assert(std::memcmp(repeat.seeds.data(), result.seeds.data(), expected.size() * sizeof(SparseVoxelLODSeedGPU)) == 0);
}

void testGPUOverflowFailsClosed(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelSceneManager manager;
  std::string error;
  assert(manager.setScene(makeCullingScene(), &error));
  const auto raster = cullingCamera();
  const TLASReadback visibleOverflow = runTLASCulling(rhi, manager, raster, 1u, static_cast<uint32_t>(manager.tlas().nodes.size()));
  assert(visibleOverflow.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::VisibleInstances));
  assert(visibleOverflow.counters.visibleInstanceCount == 0u);
  assert(visibleOverflow.counters.requiredCount > 1u && visibleOverflow.counters.capacity == 1u);

  const TLASReadback queueOverflow = runTLASCulling(rhi, manager, raster, static_cast<uint32_t>(manager.runtimeInstances().size()), 1u);
  assert(queueOverflow.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::TLASQueue));
  assert(queueOverflow.counters.visibleInstanceCount == 0u);
  assert(queueOverflow.counters.requiredCount > 1u && queueOverflow.counters.capacity == 1u);
}

void testGPUMultiBlockStableCompaction(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelScene scene = makeExteriorShellScene(43u);
  scene.instances.clear();
  scene.tlasFanout = 8u;
  scene.tlasLeafSize = 4u;
  constexpr uint32_t instanceCount = 600u;
  for (uint32_t index = 0u; index < instanceCount; ++index)
  {
    const bool visible = (index % 5u) != 0u;
    const float x = visible ? static_cast<float>(static_cast<int32_t>(index % 9u) - 4) : 80.0f + static_cast<float>(index % 7u);
    const float y = static_cast<float>(static_cast<int32_t>((index / 9u) % 7u) - 3);
    const float z = 20.0f + static_cast<float>(index % 11u);
    scene.instances.push_back({0u, 0u, transform(x, y, z, 0.25f, 0.25f, 0.25f)});
  }
  SparseVoxelSceneManager manager;
  std::string error;
  assert(manager.setScene(scene, &error));
  const auto raster = cullingCamera();
  std::vector<uint32_t> expected;
  for (const auto &instance : manager.runtimeInstances())
    if (cpuFrustumVisible(instance.gpu, raster))
      expected.push_back(instance.stableId);
  assert(expected.size() > kSparseVoxelTLASWorkgroupSize);
  const TLASReadback result = runTLASCulling(rhi, manager, raster, instanceCount, static_cast<uint32_t>(manager.tlas().nodes.size()));
  assert(result.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::None));
  assert(result.counters.visibleInstanceCount == expected.size());
  for (uint32_t output = 0u; output < expected.size(); ++output)
    assert(result.visible[output].instanceIndex == expected[output]);
}

void testGPUResidentBLASNodeFrustum(::rendering::backend::vulkan::VulkanRHI &rhi, const bool testOcclusion = false)
{
  using namespace ::rendering;
  RenderGraph graph(&rhi);
  auto storage = [&](const char *name, const uint64_t size, const bool indirect = false)
  {
    return graph.createBuffer(
        {.name = name,
         .size = std::max<uint64_t>(size, sizeof(uint32_t)),
         .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc | (indirect ? BufferUsage::BufferUsage_Indirect : BufferUsage::BufferUsage_None)});
  };
  constexpr uint32_t nodeCount = 2u;
  const Buffer counters = storage("g11.blas.counters", sizeof(SparseVoxelFrameCountersGPU));
  const Buffer visibleInstances = storage("g11.blas.visibleInstances", sizeof(VisibleInstanceGPU));
  const Buffer visibleVoxels = storage("g11.blas.visibleVoxels", nodeCount * sizeof(VisibleVoxelGPU));
  const Buffer instances = storage("g11.blas.instances", sizeof(SparseVoxelInstanceGPU));
  const Buffer nodes = storage("g11.blas.nodes", nodeCount * sizeof(SparseVoxelLODNodeGPU));
  const Buffer centers = storage("g11.blas.centers", nodeCount * sizeof(SparseVoxelCenterLengthGPU));
  const Buffer queueA = storage("g11.blas.queueA", nodeCount * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer queueB = storage("g11.blas.queueB", nodeCount * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer queueCounts = storage("g11.blas.queueCounts", 8u * sizeof(uint32_t));
  const Buffer lodArgs = storage("g11.blas.lodArgs", 8u * sizeof(IndirectDispatchArgsGPU), true);
  const Buffer rasterArgs = storage("g11.blas.rasterArgs", kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU), true);
  const Buffer metadata = storage("g11.blas.metadata", nodeCount * sizeof(SparseVoxelResidentNodeMetadataGPU));
  const Buffer requestBits = storage("g11.blas.requestBits", sizeof(uint32_t));
  const Buffer requestPriorities = storage("g11.blas.requestPriorities", nodeCount * sizeof(uint32_t));
  const Buffer requestInstances = storage("g11.blas.requestInstances", nodeCount * sizeof(uint32_t));
  const Buffer seeds = storage("g11.blas.seeds", sizeof(SparseVoxelLODSeedGPU));
  const Buffer pageBounds = storage("g11.blas.pageBounds", nodeCount * sizeof(SparseVoxelPageBoundsGPU));
  const Buffer rejectedNodes = storage("g11.blas.rejectedNodes", nodeCount * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer recoveryState = storage("g11.blas.recoveryState", sizeof(SparseVoxelLODRecoveryStateGPU));
  const Buffer temporalVisibleVoxels = storage("g11.blas.temporalVisibleVoxels", nodeCount * sizeof(VisibleVoxelGPU));
  const Buffer preSubmittedFlags = storage("g11.blas.preSubmittedFlags", sizeof(uint32_t));
  const Buffer occupancySources = storage("g24.blas.occupancySources", nodeCount * sizeof(SparseVoxelOccupancyAABBGPU));
  const Buffer countersReadback = graph.createBuffer({.name = "g11.blas.counters.read", .size = sizeof(SparseVoxelFrameCountersGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer visibleReadback = graph.createBuffer({.name = "g11.blas.visible.read", .size = nodeCount * sizeof(VisibleVoxelGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer bitsReadback = graph.createBuffer({.name = "g11.blas.bits.read", .size = sizeof(uint32_t), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer argsReadback =
      graph.createBuffer({.name = "g11.blas.args.read", .size = kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer occupancySourceReadback =
      graph.createBuffer({.name = "g24.blas.occupancySources.read", .size = nodeCount * sizeof(SparseVoxelOccupancyAABBGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Texture hzb = graph.createTexture({
    .name = "g21.blas.hzb",
    .format = Format::Format_R32Float,
    .height = 8u,
    .width = 8u,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  SparseVoxelFrameCountersGPU zero{};
  graph.bufferWrite(counters, 0u, sizeof(zero), &zero);
  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = transform(0.0f, 0.0f, 0.0f);
  instance.worldToModel = instance.modelToWorld;
  instance.previousModelToWorld = instance.modelToWorld;
  graph.bufferWrite(instances, 0u, sizeof(instance), &instance);
  std::array<SparseVoxelLODNodeGPU, nodeCount> lodNodes{{
    {0u, 0u, 0u, 1u},
    {0u, 0u, 1u, 1u},
  }};
  std::array<SparseVoxelCenterLengthGPU, nodeCount> nodeBounds{{
    {{0.0f, 0.0f, 5.0f}, 1.0f},
    {{20.0f, 0.0f, 5.0f}, 1.0f},
  }};
  std::array<SparseVoxelResidentNodeMetadataGPU, nodeCount> resident{{
    {0u, 0u, 0u, 0u},
    {1u, 1u, 0u, 0u},
  }};
  std::array<SparseVoxelPageBoundsGPU, nodeCount> bounds{{
    {{{-0.5f, -0.5f, 4.5f, 0.0f}}, {{0.5f, 0.5f, 5.5f, 0.0f}}},
    {{{19.5f, -0.5f, 4.5f, 0.0f}}, {{20.5f, 0.5f, 5.5f, 0.0f}}},
  }};
  graph.bufferWrite(nodes, 0u, sizeof(lodNodes), lodNodes.data());
  graph.bufferWrite(centers, 0u, sizeof(nodeBounds), nodeBounds.data());
  graph.bufferWrite(metadata, 0u, sizeof(resident), resident.data());
  graph.bufferWrite(pageBounds, 0u, sizeof(bounds), bounds.data());

  SparseVoxelLODPassUniformsGPU lod{};
  lod.firstRoot = 0u;
  lod.rootCount = nodeCount;
  lod.nodeCapacity = nodeCount;
  lod.pageCount = nodeCount;
  lod.requestWordCount = 1u;
  lod.streamingEnabled = 1u;
  lod.cullingFlags = kSparseVoxelCullingFrustum;
  lod.seedCapacity = 1u;
  SparseVoxelRasterUniformsGPU raster = cullingCamera();
  if (testOcclusion)
  {
    lod.cullingFlags |= kSparseVoxelCullingOcclusion | kSparseVoxelCullingReverseZ;
    lod.hzbWidth = 8u;
    lod.hzbHeight = 8u;
    lod.hzbMipCount = 1u;
    lod.historyValid = 1u;
    raster.worldToClip = {};
    raster.worldToClip[0] = 1.0f;
    raster.worldToClip[5] = -1.0f;
    raster.worldToClip[11] = 1.0f;
    raster.worldToClip[14] = raster.cameraWorldPositionNear[3];
  }
  raster.visibleVoxelCapacity = nodeCount;
  raster.projectedVoxelCapacity = nodeCount;
  raster.lodEnabled = 0u;
  const svrRendering::SparseVoxelLODPass::Buffers passBuffers{
    counters, visibleInstances, visibleVoxels,     instances,        nodes, centers,    queueA,        queueB,        queueCounts,           lodArgs,           rasterArgs,
    metadata, requestBits,      requestPriorities, requestInstances, seeds, pageBounds, rejectedNodes, recoveryState, temporalVisibleVoxels, preSubmittedFlags,
  };
  uint32_t passIndex = 0u;
  const svrRendering::SparseVoxelLODPass::OcclusionResources occlusion{hzb};
  graph.registerPass<svrRendering::SparseVoxelLODPass>(
      "g11.blas.initialize", passIndex++, svrRendering::SparseVoxelLODPass::Kernel::Initialize, passBuffers, svrRendering::SparseVoxelLODPass::Settings{raster, lod}, occlusion);
  graph.registerPass<svrRendering::SparseVoxelLODPass>(
      "g11.blas.traverse", passIndex++, svrRendering::SparseVoxelLODPass::Kernel::Traverse, passBuffers, svrRendering::SparseVoxelLODPass::Settings{raster, lod}, occlusion);
  graph.registerPass<svrRendering::SparseVoxelLODPass>(
      "g11.blas.finalize", passIndex++, svrRendering::SparseVoxelLODPass::Kernel::FinalizeSelection, passBuffers, svrRendering::SparseVoxelLODPass::Settings{raster, lod}, occlusion);
  graph.registerPass<svrRendering::SparseVoxelOccupancySourcePass>(
      "g24.blas.transformSources",
      passIndex++,
      svrRendering::SparseVoxelOccupancySourcePass::Buffers{counters, visibleVoxels, instances, centers, occupancySources},
      SparseVoxelOccupancySourceGPU{nodeCount, 1u, nodeCount, 0u});
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("g11.blas.copyCounters", passIndex++, counters, 0u, sizeof(SparseVoxelFrameCountersGPU), countersReadback, 0u, sizeof(SparseVoxelFrameCountersGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("g11.blas.copyVisible", passIndex++, visibleVoxels, 0u, nodeCount * sizeof(VisibleVoxelGPU), visibleReadback, 0u, nodeCount * sizeof(VisibleVoxelGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("g11.blas.copyBits", passIndex++, requestBits, 0u, sizeof(uint32_t), bitsReadback, 0u, sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g11.blas.copyArgs", passIndex++, rasterArgs, 0u, kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU), argsReadback, 0u, kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g24.blas.copyOccupancySources", passIndex++, occupancySources, 0u, nodeCount * sizeof(SparseVoxelOccupancyAABBGPU), occupancySourceReadback, 0u, nodeCount * sizeof(SparseVoxelOccupancyAABBGPU));
  graph.compile();
  std::vector<float> hzbPixels(8u * 8u, 0.05f);
  graph.textureWrite(hzb, hzbPixels.size() * sizeof(float), hzbPixels.data());
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  SparseVoxelFrameCountersGPU resultCounters{};
  std::array<VisibleVoxelGPU, nodeCount> resultVisible{};
  std::array<IndirectDispatchArgsGPU, kSparseVoxelIndirectSlotCount> resultArgs{};
  std::array<SparseVoxelOccupancyAABBGPU, nodeCount> resultOccupancySources{};
  uint32_t resultBits = 0u;
  graph.bufferRead(
      countersReadback,
      0u,
      sizeof(resultCounters),
      [&](const void *data)
      {
        std::memcpy(&resultCounters, data, sizeof(resultCounters));
      });
  graph.bufferRead(
      visibleReadback,
      0u,
      sizeof(resultVisible),
      [&](const void *data)
      {
        std::memcpy(resultVisible.data(), data, sizeof(resultVisible));
      });
  graph.bufferRead(
      bitsReadback,
      0u,
      sizeof(resultBits),
      [&](const void *data)
      {
        std::memcpy(&resultBits, data, sizeof(resultBits));
      });
  graph.bufferRead(
      argsReadback,
      0u,
      sizeof(resultArgs),
      [&](const void *data)
      {
        std::memcpy(resultArgs.data(), data, sizeof(resultArgs));
      });
  graph.bufferRead(
      occupancySourceReadback,
      0u,
      sizeof(resultOccupancySources),
      [&](const void *data)
      {
        std::memcpy(resultOccupancySources.data(), data, sizeof(resultOccupancySources));
      });
  assert(resultCounters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::None));
  assert(resultCounters.visibleVoxelCount == (testOcclusion ? 0u : 1u));
  assert(resultCounters.occlusionCulledVoxelCount == (testOcclusion ? 1u : 0u));
  if (!testOcclusion)
  {
    assert(resultVisible[0].instanceIndex == 0u && resultVisible[0].voxelIndex == 0u);
    assert((resultOccupancySources[0].minimum == std::array<float, 4>{-0.5f, -0.5f, 4.5f, 0.0f}));
    assert((resultOccupancySources[0].maximum == std::array<float, 4>{0.5f, 0.5f, 5.5f, 0.0f}));
  }
  assert(resultBits == (testOcclusion ? 0u : 1u));
  assert(resultArgs[0].x == (testOcclusion ? 0u : 1u) && resultArgs[0].y == 1u && resultArgs[0].z == 1u);
}

void runG11()
{
  runG10();
  using namespace ::rendering::backend;
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 0u, .minimumComputeWorkGroupInvocations = 256u};
  const ::rendering::DeviceFeatures features = ::rendering::DeviceFeatures::DeviceFeatures_Compute;
  vulkan::VulkanRHI rhi(vulkan::Vulkan_1_2, limits, features, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testGPUFrustumAndStableCompaction(rhi);
  testGPUMultiBlockStableCompaction(rhi);
  testGPUOverflowFailsClosed(rhi);
  testGPUResidentBLASNodeFrustum(rhi);
}

SparseVoxelRasterUniformsGPU occlusionCamera(const bool reverseZ)
{
  SparseVoxelRasterUniformsGPU raster = cullingCamera();
  raster.worldToClip = {};
  raster.worldToClip[0] = 1.0f;
  raster.worldToClip[5] = -1.0f;
  raster.worldToClip[11] = 1.0f;
  if (reverseZ)
  {
    raster.worldToClip[14] = raster.cameraWorldPositionNear[3];
  }
  else
  {
    raster.worldToClip[10] = 1.0f;
    raster.worldToClip[14] = -raster.cameraWorldPositionNear[3];
  }
  return raster;
}

SparseVoxelSceneManager makeOcclusionManager()
{
  SparseVoxelScene scene = makeExteriorShellScene(59u);
  scene.instances.clear();
  scene.tlasFanout = 8u;
  scene.tlasLeafSize = 8u;
  // Keep the synthetic occluded bound within the exact footprint supported by
  // this fixture's single-mip HZB. Runtime HZBs provide coarser levels.
  scene.instances.push_back({0u, 0u, transform(0.0f, 0.0f, 5.0f, 0.1f, 0.1f, 0.1f)});    // definitely occluded
  scene.instances.push_back({0u, 0u, transform(0.0f, 0.0f, 1.0f, 0.25f, 0.25f, 0.25f)}); // in front
  scene.instances.push_back({0u, 0u, transform(4.8f, 0.0f, 5.0f, 0.25f, 0.25f, 0.25f)}); // viewport escape
  scene.instances.push_back({0u, 0u, transform(1.0f, 0.0f, 5.0f, 0.25f, 0.25f, 0.25f)}); // moves
  scene.instances.push_back({0u, 0u, transform(0.0f, 0.0f, 0.2f, 0.25f, 0.25f, 0.25f)}); // near-plane overlap
  SparseVoxelSceneManager manager;
  std::string error;
  assert(manager.setScene(scene, &error));
  assert(manager.setInstanceTransform(3u, transform(2.0f, 0.0f, 5.0f, 0.25f, 0.25f, 0.25f), &error));
  return manager;
}

void testGPUConservativeOcclusion(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelSceneManager manager = makeOcclusionManager();
  const uint32_t instanceCount = static_cast<uint32_t>(manager.runtimeInstances().size());
  const uint32_t queueCapacity = static_cast<uint32_t>(manager.tlas().nodes.size());

  TLASOcclusionFixture fixture{};
  fixture.enabled = true;
  fixture.reverseZ = true;
  fixture.historyValid = false;
  fixture.hzbDepth = 0.05f;
  TLASReadback result = runTLASCulling(rhi, manager, occlusionCamera(true), instanceCount, queueCapacity, fixture);
  assert(result.counters.visibleInstanceCount == instanceCount);
  assert(
      std::all_of(
          result.reasons.begin(),
          result.reasons.end(),
          [](const uint32_t reason)
          {
            return reason == static_cast<uint32_t>(SparseVoxelOcclusionReason::VisibleHistoryInvalid);
          }));

  fixture.historyValid = true;
  fixture.previousFrame = true;
  result = runTLASCulling(rhi, manager, occlusionCamera(true), instanceCount, queueCapacity, fixture);
  const TLASReadback repeatReverseZ =
      runTLASCulling(
          rhi, manager, occlusionCamera(true), instanceCount, queueCapacity,
          fixture);
  expectSameTLASReadback(result, repeatReverseZ);
  assert(result.flags[0] == 0u);
  assert(result.reasons[0] == static_cast<uint32_t>(SparseVoxelOcclusionReason::CulledOcclusion));
  assert(result.flags[1] == 1u);
  assert(result.flags[2] == 1u);
  assert(result.reasons[2] == static_cast<uint32_t>(SparseVoxelOcclusionReason::VisibleViewportEscape));
  assert(result.flags[3] == 1u);
  assert(result.reasons[3] == static_cast<uint32_t>(SparseVoxelOcclusionReason::VisibleMovingPreviousTransform));
  assert(result.flags[4] == 1u);
  assert(result.reasons[4] == static_cast<uint32_t>(SparseVoxelOcclusionReason::VisibleNearPlane));

  fixture.reverseZ = false;
  fixture.previousFrame = false;
  fixture.hzbDepth = 0.9f;
  result = runTLASCulling(rhi, manager, occlusionCamera(false), instanceCount, queueCapacity, fixture);
  const TLASReadback repeatForwardZ =
      runTLASCulling(
          rhi, manager, occlusionCamera(false), instanceCount, queueCapacity,
          fixture);
  expectSameTLASReadback(result, repeatForwardZ);
  assert(result.flags[0] == 0u);
  assert(result.flags[1] == 1u);
  assert(result.flags[2] == 1u);
  assert(result.flags[4] == 1u);

  // Recovery never resubmits an instance already emitted by pre-culling.
  fixture.reverseZ = true;
  fixture.hzbDepth = 0.0f;
  fixture.recovery = true;
  fixture.preSubmitted.assign(instanceCount, 0u);
  fixture.preSubmitted[1] = 1u;
  result = runTLASCulling(rhi, manager, occlusionCamera(true), instanceCount, queueCapacity, fixture);
  const TLASReadback repeatRecovery =
      runTLASCulling(
          rhi, manager, occlusionCamera(true), instanceCount, queueCapacity,
          fixture);
  expectSameTLASReadback(result, repeatRecovery);
  assert(result.flags[1] == 0u);
  assert(result.counters.visibleInstanceCount == instanceCount - 1u);

  // A complete final render guarantees prepass submissions instead of
  // testing them against the depth they generated themselves.
  fixture.recovery = false;
  fixture.prepassGuarantee = true;
  result = runTLASCulling(rhi, manager, occlusionCamera(true), instanceCount, queueCapacity, fixture);
  const TLASReadback repeatPrepassGuarantee =
      runTLASCulling(
          rhi, manager, occlusionCamera(true), instanceCount, queueCapacity,
          fixture);
  expectSameTLASReadback(result, repeatPrepassGuarantee);
  assert(result.flags[1] == 1u);
  assert(result.reasons[1] == static_cast<uint32_t>(SparseVoxelOcclusionReason::Visible));
  assert(result.counters.visibleInstanceCount == instanceCount);
}

struct BLASRecoveryReadback
{
  SparseVoxelFrameCountersGPU temporalCounters{};
  SparseVoxelFrameCountersGPU finalCounters{};
  SparseVoxelLODRecoveryStateGPU recovery{};
  std::array<VisibleVoxelGPU, 7> visible{};
};

BLASRecoveryReadback runGPUHierarchicalBLASRecovery(::rendering::backend::vulkan::VulkanRHI &rhi, const bool revealRejectedSubtree, const bool historyValid = true, const bool movingInstance = false)
{
  using namespace ::rendering;
  constexpr uint32_t nodeCount = 7u;
  constexpr uint32_t levelCount = 3u;
  RenderGraph graph(&rhi);
  auto storage = [&](const char *name, const uint64_t size, const bool indirect = false)
  {
    return graph.createBuffer({
      .name = name,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc | (indirect ? BufferUsage::BufferUsage_Indirect : BufferUsage::BufferUsage_None),
    });
  };

  const Buffer counters = storage("g21.recovery.counters", sizeof(SparseVoxelFrameCountersGPU));
  const Buffer visibleInstances = storage("g21.recovery.instances.visible", sizeof(VisibleInstanceGPU));
  const Buffer visibleVoxels = storage("g21.recovery.voxels.visible", nodeCount * sizeof(VisibleVoxelGPU));
  const Buffer instances = storage("g21.recovery.instances", sizeof(SparseVoxelInstanceGPU));
  const Buffer nodes = storage("g21.recovery.nodes", nodeCount * sizeof(SparseVoxelLODNodeGPU));
  const Buffer centers = storage("g21.recovery.centers", nodeCount * sizeof(SparseVoxelCenterLengthGPU));
  const Buffer queueA = storage("g21.recovery.queueA", nodeCount * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer queueB = storage("g21.recovery.queueB", nodeCount * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer queueCounts = storage("g21.recovery.queueCounts", levelCount * sizeof(uint32_t));
  const Buffer lodArgs = storage("g21.recovery.lodArgs", levelCount * sizeof(IndirectDispatchArgsGPU), true);
  const Buffer rasterArgs = storage("g21.recovery.rasterArgs", kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU), true);
  const Buffer metadata = storage("g21.recovery.metadata", nodeCount * sizeof(SparseVoxelResidentNodeMetadataGPU));
  const Buffer requestBits = storage("g21.recovery.requestBits", sizeof(uint32_t));
  const Buffer requestPriorities = storage("g21.recovery.requestPriorities", sizeof(uint32_t));
  const Buffer requestInstances = storage("g21.recovery.requestInstances", sizeof(uint32_t));
  const Buffer seeds = storage("g21.recovery.seeds", sizeof(SparseVoxelLODSeedGPU));
  const Buffer pageBounds = storage("g21.recovery.pageBounds", sizeof(SparseVoxelPageBoundsGPU));
  const Buffer rejectedNodes = storage("g21.recovery.rejectedNodes", nodeCount * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer recoveryState = storage("g21.recovery.state", sizeof(SparseVoxelLODRecoveryStateGPU));
  const Buffer temporalVisibleVoxels = storage("g21.recovery.temporalVisible", nodeCount * sizeof(VisibleVoxelGPU));
  const Buffer preSubmittedFlags = storage("g21.recovery.preSubmitted", sizeof(uint32_t));
  const Buffer readback = graph.createBuffer({
    .name = "g21.recovery.readback",
    .size = sizeof(BLASRecoveryReadback),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Texture previousHZB = graph.createTexture({
    .name = "g21.recovery.previousHZB",
    .format = Format::Format_R32Float,
    .height = 64u,
    .width = 64u,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });
  const Texture provisionalHZB = graph.createTexture({
    .name = "g21.recovery.provisionalHZB",
    .format = Format::Format_R32Float,
    .height = 64u,
    .width = 64u,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = transform(0.0f, 0.0f, 0.0f);
  instance.worldToModel = instance.modelToWorld;
  instance.previousModelToWorld = movingInstance ? transform(1.0f, 0.0f, 0.0f) : instance.modelToWorld;
  const std::array<SparseVoxelLODNodeGPU, nodeCount> hierarchy{{
    {1u, 2u, 0u, 0u},
    {3u, 2u, 1u, 1u},
    {5u, 2u, 2u, 1u},
    {0u, 0u, 3u, 2u},
    {0u, 0u, 4u, 2u},
    {0u, 0u, 5u, 2u},
    {0u, 0u, 6u, 2u},
  }};
  const std::array<SparseVoxelCenterLengthGPU, nodeCount> bounds{{
    {{0.0f, 0.0f, 5.0f}, 1.0f},
    {{-0.3f, 0.0f, 5.0f}, 0.2f},
    {{0.3f, 0.0f, 5.0f}, 0.2f},
    {{-0.35f, 0.0f, 5.0f}, 0.1f},
    {{-0.25f, 0.0f, 5.0f}, 0.1f},
    {{0.25f, 0.0f, 5.0f}, 0.1f},
    {{0.35f, 0.0f, 5.0f}, 0.1f},
  }};
  graph.bufferWrite(instances, 0u, sizeof(instance), &instance);
  graph.bufferWrite(nodes, 0u, sizeof(hierarchy), const_cast<SparseVoxelLODNodeGPU *>(hierarchy.data()));
  graph.bufferWrite(centers, 0u, sizeof(bounds), const_cast<SparseVoxelCenterLengthGPU *>(bounds.data()));
  const uint32_t preSubmitted = 1u;
  graph.bufferWrite(preSubmittedFlags, 0u, sizeof(preSubmitted), const_cast<uint32_t *>(&preSubmitted));

  SparseVoxelRasterUniformsGPU raster = occlusionCamera(true);
  raster.previousWorldToClip = raster.worldToClip;
  raster.visibleVoxelCapacity = nodeCount;
  raster.projectedVoxelCapacity = nodeCount;
  raster.lodEnabled = 0u;
  const svrRendering::SparseVoxelLODPass::Buffers buffers{
    counters, visibleInstances, visibleVoxels,     instances,        nodes, centers,    queueA,        queueB,        queueCounts,           lodArgs,           rasterArgs,
    metadata, requestBits,      requestPriorities, requestInstances, seeds, pageBounds, rejectedNodes, recoveryState, temporalVisibleVoxels, preSubmittedFlags,
  };
  SparseVoxelLODPassUniformsGPU temporal{};
  temporal.firstRoot = 0u;
  temporal.rootCount = 1u;
  temporal.nodeCapacity = nodeCount;
  temporal.levelCapacity = levelCount;
  temporal.seedCapacity = 1u;
  temporal.cullingFlags = kSparseVoxelCullingFrustum | kSparseVoxelCullingOcclusion | kSparseVoxelCullingPreviousFrame | kSparseVoxelCullingReverseZ;
  temporal.hzbWidth = 64u;
  temporal.hzbHeight = 64u;
  temporal.hzbMipCount = 1u;
  temporal.historyValid = historyValid ? 1u : 0u;
  temporal.rejectionCapacity = nodeCount;
  SparseVoxelLODPassUniformsGPU recovery = temporal;
  recovery.cullingFlags = kSparseVoxelCullingFrustum | kSparseVoxelCullingOcclusion | kSparseVoxelCullingRecovery | kSparseVoxelCullingReverseZ;
  recovery.historyValid = 1u;

  uint32_t passIndex = 0u;
  auto addLODPhase = [&](const char *prefix, SparseVoxelLODPassUniformsGPU settings, const Texture hzb, const bool preserve)
  {
    const svrRendering::SparseVoxelLODPass::OcclusionResources occlusion{hzb};
    settings.depth = 0u;
    graph.registerPass<svrRendering::SparseVoxelLODPass>(
        std::string(prefix) + ".initialize", passIndex++, svrRendering::SparseVoxelLODPass::Kernel::Initialize, buffers, svrRendering::SparseVoxelLODPass::Settings{raster, settings}, occlusion);
    for (uint32_t depth = 0u; depth < levelCount; ++depth)
    {
      settings.depth = depth;
      graph.registerPass<svrRendering::SparseVoxelLODPass>(
          std::string(prefix) + ".traverse." + std::to_string(depth), passIndex++, svrRendering::SparseVoxelLODPass::Kernel::Traverse, buffers, svrRendering::SparseVoxelLODPass::Settings{raster, settings}, occlusion);
      if (depth + 1u < levelCount)
        graph.registerPass<svrRendering::SparseVoxelLODPass>(
            std::string(prefix) + ".finalize." + std::to_string(depth),
            passIndex++,
            svrRendering::SparseVoxelLODPass::Kernel::FinalizeLevel,
            buffers,
            svrRendering::SparseVoxelLODPass::Settings{raster, settings},
            occlusion);
    }
    graph.registerPass<svrRendering::SparseVoxelLODPass>(
        std::string(prefix) + ".selection", passIndex++, svrRendering::SparseVoxelLODPass::Kernel::FinalizeSelection, buffers, svrRendering::SparseVoxelLODPass::Settings{raster, settings}, occlusion);
    if (preserve)
      graph.registerPass<svrRendering::SparseVoxelLODPass>(
          std::string(prefix) + ".preserve", passIndex++, svrRendering::SparseVoxelLODPass::Kernel::PreserveTemporalSelection, buffers, svrRendering::SparseVoxelLODPass::Settings{raster, settings}, occlusion);
  };

  addLODPhase("g21.recovery.temporal", temporal, previousHZB, true);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g21.recovery.copyTemporalCounters", passIndex++, counters, 0u, sizeof(SparseVoxelFrameCountersGPU), readback, offsetof(BLASRecoveryReadback, temporalCounters), sizeof(SparseVoxelFrameCountersGPU));
  graph.registerPass<svrRendering::SparseVoxelBootstrapPass>(
      "g21.recovery.clearFinal",
      passIndex++,
      svrRendering::SparseVoxelBootstrapPass::Kernel::ClearFrame,
      counters,
      visibleInstances,
      rasterArgs,
      svrRendering::SparseVoxelBootstrapPass::Settings{nodeCount, kPreprocessWorkgroupSize, 1u, nodeCount});
  addLODPhase("g21.recovery.final", recovery, provisionalHZB, false);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g21.recovery.copyFinalCounters", passIndex++, counters, 0u, sizeof(SparseVoxelFrameCountersGPU), readback, offsetof(BLASRecoveryReadback, finalCounters), sizeof(SparseVoxelFrameCountersGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g21.recovery.copyState", passIndex++, recoveryState, 0u, sizeof(SparseVoxelLODRecoveryStateGPU), readback, offsetof(BLASRecoveryReadback, recovery), sizeof(SparseVoxelLODRecoveryStateGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g21.recovery.copyVisible", passIndex++, visibleVoxels, 0u, nodeCount * sizeof(VisibleVoxelGPU), readback, offsetof(BLASRecoveryReadback, visible), nodeCount * sizeof(VisibleVoxelGPU));
  graph.compile();

  std::vector<float> previousDepth(64u * 64u, 0.0f);
  for (uint32_t y = 0u; y < 64u; ++y)
    for (uint32_t x = 0u; x < 32u; ++x)
      previousDepth[y * 64u + x] = 0.05f;
  std::vector<float> provisionalDepth(64u * 64u, revealRejectedSubtree ? 0.0f : 0.05f);
  graph.textureWrite(previousHZB, previousDepth.size() * sizeof(float), previousDepth.data());
  graph.textureWrite(provisionalHZB, provisionalDepth.size() * sizeof(float), provisionalDepth.data());
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
  BLASRecoveryReadback result{};
  graph.bufferRead(
      readback,
      0u,
      sizeof(result),
      [&](const void *data)
      {
        std::memcpy(&result, data, sizeof(result));
      });
  return result;
}

void testGPUHierarchicalBLASRecovery(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  BLASRecoveryReadback result = runGPUHierarchicalBLASRecovery(rhi, true);
  if (result.temporalCounters.visibleVoxelCount != 2u)
    std::fprintf(
        stderr,
        "hierarchical recovery mismatch: temporal=%u rejected=%u seeds=%u recovered=%u final=%u temporalCull=%u finalCull=%u\n",
        result.temporalCounters.visibleVoxelCount,
        result.recovery.rejectedNodeCount,
        result.recovery.recoverySeedCount,
        result.recovery.recoveredVisibleVoxelCount,
        result.finalCounters.visibleVoxelCount,
        result.temporalCounters.occlusionCulledVoxelCount,
        result.finalCounters.occlusionCulledVoxelCount);
  assert(result.temporalCounters.visibleVoxelCount == 2u);
  assert(result.temporalCounters.occlusionCulledVoxelCount == 1u);
  assert(result.recovery.rejectedNodeCount == 1u);
  assert(result.recovery.recoverySeedCount == 1u);
  assert(result.recovery.temporalVisibleVoxelCount == 2u);
  assert(result.recovery.recoveredVisibleVoxelCount == 2u);
  assert(result.finalCounters.visibleVoxelCount == 4u);
  std::array<uint32_t, 4> selected{
    result.visible[0].voxelIndex,
    result.visible[1].voxelIndex,
    result.visible[2].voxelIndex,
    result.visible[3].voxelIndex,
  };
  std::sort(selected.begin(), selected.end());
  assert((selected == std::array<uint32_t, 4>{3u, 4u, 5u, 6u}));

  result = runGPUHierarchicalBLASRecovery(rhi, false);
  assert(result.temporalCounters.visibleVoxelCount == 2u);
  assert(result.recovery.rejectedNodeCount == 1u);
  assert(result.recovery.recoverySeedCount == 1u);
  assert(result.recovery.recoveredVisibleVoxelCount == 0u);
  assert(result.finalCounters.visibleVoxelCount == 2u);
  assert(result.finalCounters.occlusionCulledVoxelCount == 1u);
  std::array<uint32_t, 2> preserved{
    result.visible[0].voxelIndex,
    result.visible[1].voxelIndex,
  };
  std::sort(preserved.begin(), preserved.end());
  assert((preserved == std::array<uint32_t, 2>{5u, 6u}));

  // Invalid history and moving instances both fail open. The temporal pass
  // selects the complete hierarchy and the final pass preserves it without
  // seeding any self-occluding recovery work.
  for (const BLASRecoveryReadback failOpen : {
         runGPUHierarchicalBLASRecovery(rhi, false, false, false),
         runGPUHierarchicalBLASRecovery(rhi, false, true, true),
       })
  {
    assert(failOpen.temporalCounters.visibleVoxelCount == 4u);
    assert(failOpen.temporalCounters.occlusionCulledVoxelCount == 0u);
    assert(failOpen.recovery.rejectedNodeCount == 0u);
    assert(failOpen.recovery.recoverySeedCount == 0u);
    assert(failOpen.recovery.temporalVisibleVoxelCount == 4u);
    assert(failOpen.recovery.recoveredVisibleVoxelCount == 0u);
    assert(failOpen.finalCounters.visibleVoxelCount == 4u);
  }
}

void runG21()
{
  using namespace ::rendering::backend;
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 0u, .minimumComputeWorkGroupInvocations = 256u};
  vulkan::VulkanRHI rhi(vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testGPUConservativeOcclusion(rhi);
  testGPUResidentBLASNodeFrustum(rhi, true);
  testGPUHierarchicalBLASRecovery(rhi);
}

void runG24()
{
  using namespace ::rendering::backend;
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 0u, .minimumComputeWorkGroupInvocations = 256u};
  vulkan::VulkanRHI rhi(vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testGPUOccupancyVolumeQuery(rhi);
  testGPUResidentBLASNodeFrustum(rhi);
}

} // namespace

int main(int argc, char **argv)
{
  os::Logger::start();
  const std::string gate = argc > 1 ? argv[1] : "--g10";
  if (gate == "--g10")
  {
    runG10();
    std::cout << "Sparse voxel G10 scene-manager/TLAS tests passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (gate == "--g11")
  {
    runG11();
    std::cout << "Sparse voxel G11 GPU TLAS/frustum tests passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (gate == "--g21")
  {
    runG21();
    std::cout << "Sparse voxel G21 conservative HZB occlusion tests passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (gate == "--g24")
  {
    runG24();
    std::cout << "Sparse voxel G24 occupancy TLAS-query tests passed\n";
    os::Logger::shutdown();
    return 0;
  }
  os::Logger::shutdown();
  std::cerr << "usage: SparseVoxelSceneCullingTests --g10|--g11|--g21|--g24\n";
  return 2;
}
