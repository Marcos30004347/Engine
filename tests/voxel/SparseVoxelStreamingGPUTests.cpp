#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/rendering/SparseVoxelBrickPasses.hpp"
#include "voxel/svraster/rendering/SparseVoxelLODPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelStreamingPass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#undef assert
#define assert(condition)                                                                                                                                        \
  do                                                                                                                                                             \
  {                                                                                                                                                              \
    if (!(condition))                                                                                                                                            \
    {                                                                                                                                                            \
      std::fprintf(stderr, "test assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__);                                                           \
      std::abort();                                                                                                                                              \
    }                                                                                                                                                            \
  } while (false)

namespace
{

using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

std::array<float, 16> identityMatrix()
{
  return {1.0f, 0.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 0.0f, 1.0f};
}

void testClearAndPhysicalRewritePass(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  using namespace ::rendering;
  RenderGraph graph(&rhi);
  auto storage = [&](const char *name, const uint64_t size)
  {
    return graph.createBuffer(
        {.name = name, .size = size, .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc});
  };
  constexpr uint32_t pageCount = 70u;
  constexpr uint32_t wordCount = 3u;
  constexpr uint32_t nodeCount = 4u;
  const Buffer bits = storage("g16.gpu.bits", wordCount * sizeof(uint32_t));
  const Buffer priorities = storage("g16.gpu.priorities", pageCount * sizeof(uint32_t));
  const Buffer instances = storage("g16.gpu.instances", pageCount * sizeof(uint32_t));
  const Buffer rewrites = storage("g16.gpu.rewrites", 2u * sizeof(SparseVoxelPhysicalPageRewriteGPU));
  const Buffer nodes = storage("g16.gpu.nodes", nodeCount * sizeof(SparseVoxelLODNodeGPU));
  const Buffer bitsReadback = graph.createBuffer(
      {.name = "g16.gpu.bitsReadback", .size = wordCount * sizeof(uint32_t), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer prioritiesReadback = graph.createBuffer(
      {.name = "g16.gpu.prioritiesReadback", .size = pageCount * sizeof(uint32_t), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer instancesReadback = graph.createBuffer(
      {.name = "g16.gpu.instancesReadback", .size = pageCount * sizeof(uint32_t), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer nodesReadback = graph.createBuffer(
      {.name = "g16.gpu.nodesReadback", .size = nodeCount * sizeof(SparseVoxelLODNodeGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});

  std::vector<uint32_t> poisonBits(wordCount, 0xffffffffu);
  std::vector<uint32_t> poisonPriorities(pageCount, 0x12345678u);
  std::vector<uint32_t> poisonInstances(pageCount, 17u);
  std::vector<SparseVoxelLODNodeGPU> inputNodes(nodeCount);
  for (uint32_t index = 0u; index < nodeCount; ++index)
    inputNodes[index] = {99u, 7u, index, index};
  const std::array<SparseVoxelPhysicalPageRewriteGPU, 2> inputRewrites{{{0u, 2u, 2u, 0u}, {3u, 0u, 0u, 0u}}};
  graph.bufferWrite(bits, 0u, poisonBits.size() * sizeof(uint32_t), poisonBits.data());
  graph.bufferWrite(priorities, 0u, poisonPriorities.size() * sizeof(uint32_t), poisonPriorities.data());
  graph.bufferWrite(instances, 0u, poisonInstances.size() * sizeof(uint32_t), poisonInstances.data());
  graph.bufferWrite(nodes, 0u, inputNodes.size() * sizeof(inputNodes[0]), inputNodes.data());
  graph.bufferWrite(rewrites, 0u, sizeof(inputRewrites), const_cast<SparseVoxelPhysicalPageRewriteGPU *>(inputRewrites.data()));

  const svrRendering::SparseVoxelStreamingPass::Buffers buffers{bits, priorities, instances, rewrites, nodes};
  uint32_t passIndex = 0u;
  graph.registerPass<svrRendering::SparseVoxelStreamingPass>(
      "g16.gpu.clear", passIndex++, svrRendering::SparseVoxelStreamingPass::Kernel::ClearPageRequests, buffers,
      SparseVoxelStreamingPassUniformsGPU{pageCount, wordCount, 0u, nodeCount});
  graph.registerPass<svrRendering::SparseVoxelStreamingPass>(
      "g16.gpu.rewrite", passIndex++, svrRendering::SparseVoxelStreamingPass::Kernel::ApplyPhysicalPageRewrites, buffers,
      SparseVoxelStreamingPassUniformsGPU{pageCount, wordCount, 2u, nodeCount});
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>("g16.gpu.copyBits", passIndex++, bits, 0u, wordCount * sizeof(uint32_t), bitsReadback, 0u, wordCount * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g16.gpu.copyPriorities", passIndex++, priorities, 0u, pageCount * sizeof(uint32_t), prioritiesReadback, 0u, pageCount * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g16.gpu.copyInstances", passIndex++, instances, 0u, pageCount * sizeof(uint32_t), instancesReadback, 0u, pageCount * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g16.gpu.copyNodes", passIndex++, nodes, 0u, nodeCount * sizeof(SparseVoxelLODNodeGPU), nodesReadback, 0u, nodeCount * sizeof(SparseVoxelLODNodeGPU));
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  graph.bufferRead(bitsReadback, 0u, poisonBits.size() * sizeof(uint32_t), [&](const void *data) { std::memcpy(poisonBits.data(), data, poisonBits.size() * sizeof(uint32_t)); });
  graph.bufferRead(prioritiesReadback, 0u, poisonPriorities.size() * sizeof(uint32_t), [&](const void *data) { std::memcpy(poisonPriorities.data(), data, poisonPriorities.size() * sizeof(uint32_t)); });
  graph.bufferRead(instancesReadback, 0u, poisonInstances.size() * sizeof(uint32_t), [&](const void *data) { std::memcpy(poisonInstances.data(), data, poisonInstances.size() * sizeof(uint32_t)); });
  graph.bufferRead(nodesReadback, 0u, inputNodes.size() * sizeof(inputNodes[0]), [&](const void *data) { std::memcpy(inputNodes.data(), data, inputNodes.size() * sizeof(inputNodes[0])); });
  assert(std::all_of(poisonBits.begin(), poisonBits.end(), [](const uint32_t value) { return value == 0u; }));
  assert(std::all_of(poisonPriorities.begin(), poisonPriorities.end(), [](const uint32_t value) { return value == 0u; }));
  assert(std::all_of(poisonInstances.begin(), poisonInstances.end(), [](const uint32_t value) { return value == kInvalidSparseVoxelIndex; }));
  assert(inputNodes[0].firstChild == 2u && inputNodes[0].childCount == 2u);
  assert(inputNodes[3].firstChild == 0u && inputNodes[3].childCount == 0u);
  assert(inputNodes[1].firstChild == 99u && inputNodes[1].childCount == 7u);
}

struct LODRequestReadback
{
  uint32_t bits = 0u;
  std::array<uint32_t, 4> priorities{};
  std::array<uint32_t, 4> instances{};
  SparseVoxelFrameCountersGPU counters{};
  std::array<VisibleVoxelGPU, 2> visible{};
};

LODRequestReadback runLODRequests(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const float threshold,
    const bool validRefinement,
    const bool installedChildren,
    const bool sharedInstances = false)
{
  using namespace ::rendering;
  RenderGraph graph(&rhi);
  auto storage = [&](const char *name, const uint64_t size, const bool indirect = false)
  {
    return graph.createBuffer(
        {.name = name,
         .size = size,
         .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc |
                  (indirect ? BufferUsage::BufferUsage_Indirect : BufferUsage::BufferUsage_None)});
  };
  constexpr uint32_t capacity = 2u;
  const Buffer counters = storage("g16.lod.counters", sizeof(SparseVoxelFrameCountersGPU));
  const Buffer visibleInstances = storage("g16.lod.visibleInstances", (sharedInstances ? 2u : 1u) * sizeof(VisibleInstanceGPU));
  const Buffer visibleVoxels = storage("g16.lod.visibleVoxels", capacity * sizeof(VisibleVoxelGPU));
  const Buffer instancesBuffer = storage("g16.lod.sceneInstances", (sharedInstances ? 2u : 1u) * sizeof(SparseVoxelInstanceGPU));
  const Buffer nodesBuffer = storage("g16.lod.nodes", capacity * sizeof(SparseVoxelLODNodeGPU));
  const Buffer centersBuffer = storage("g16.lod.centers", capacity * sizeof(SparseVoxelCenterLengthGPU));
  const Buffer queueA = storage("g16.lod.queueA", capacity * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer queueB = storage("g16.lod.queueB", capacity * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer queueCounts = storage("g16.lod.queueCounts", 8u * sizeof(uint32_t));
  const Buffer lodArgs = storage("g16.lod.args", 8u * sizeof(IndirectDispatchArgsGPU), true);
  const Buffer rasterArgs = storage("g16.lod.rasterArgs", kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU), true);
  const Buffer metadataBuffer = storage("g16.lod.metadata", capacity * sizeof(SparseVoxelResidentNodeMetadataGPU));
  const Buffer bits = storage("g16.lod.bits", sizeof(uint32_t));
  const Buffer priorities = storage("g16.lod.priorities", 4u * sizeof(uint32_t));
  const Buffer requestInstances = storage("g16.lod.requestInstances", 4u * sizeof(uint32_t));
  const Buffer lodSeeds = storage("g16.lod.seeds", (sharedInstances ? 2u : 1u) * sizeof(SparseVoxelLODSeedGPU));
  const Buffer pageBoundsBuffer = storage("g16.lod.pageBounds", 4u * sizeof(SparseVoxelPageBoundsGPU));
  const Buffer rejectedNodes = storage("g16.lod.rejectedNodes", capacity * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer recoveryState = storage("g16.lod.recoveryState", sizeof(SparseVoxelLODRecoveryStateGPU));
  const Buffer temporalVisibleVoxels = storage("g16.lod.temporalVisibleVoxels", capacity * sizeof(VisibleVoxelGPU));
  const Buffer preSubmittedFlags = storage("g16.lod.preSubmittedFlags", (sharedInstances ? 2u : 1u) * sizeof(uint32_t));
  const Buffer readback = graph.createBuffer(
      {.name = "g16.lod.readback", .size = sizeof(LODRequestReadback), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});

  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = identityMatrix();
  instance.worldToModel = identityMatrix();
  instance.previousModelToWorld = identityMatrix();
  std::array<SparseVoxelInstanceGPU, 2> sceneInstances{instance, instance};
  if (sharedInstances)
  {
    sceneInstances[0].modelToWorld[14] = 32.0f; // the higher instance ID has the higher projected priority
    sceneInstances[0].previousModelToWorld[14] = 32.0f;
  }
  std::array<SparseVoxelLODNodeGPU, capacity> nodes{{{installedChildren ? 1u : 0u, installedChildren ? 1u : 0u, 0u, 1u}, {0u, 0u, 1u, 1u}}};
  std::array<SparseVoxelCenterLengthGPU, capacity> centers{{{{0.0f, 0.0f, 8.0f}, 2.0f}, {{0.0f, 0.0f, 8.0f}, 1.0f}}};
  std::array<SparseVoxelResidentNodeMetadataGPU, capacity> metadata{};
  metadata[0] = {0u, validRefinement ? 3u : kInvalidSparseVoxelIndex, 0u, 0u};
  metadata[1] = {1u, validRefinement ? 3u : kInvalidSparseVoxelIndex, 0u, 0u};
  const SparseVoxelFrameCountersGPU zeroCounters{};
  graph.bufferWrite(counters, 0u, sizeof(zeroCounters), const_cast<SparseVoxelFrameCountersGPU *>(&zeroCounters));
  graph.bufferWrite(instancesBuffer, 0u, (sharedInstances ? 2u : 1u) * sizeof(SparseVoxelInstanceGPU), sceneInstances.data());
  graph.bufferWrite(nodesBuffer, 0u, sizeof(nodes), nodes.data());
  graph.bufferWrite(centersBuffer, 0u, sizeof(centers), centers.data());
  graph.bufferWrite(metadataBuffer, 0u, sizeof(metadata), metadata.data());
  std::array<SparseVoxelLODSeedGPU, 2> seeds{{{0u, 0u, 1u, 0u}, {1u, 0u, 1u, 1u}}};
  if (sharedInstances)
    graph.bufferWrite(lodSeeds, 0u, sizeof(seeds), seeds.data());
  std::array<SparseVoxelPageBoundsGPU, 4> gpuPageBounds{};
  gpuPageBounds[3].localBoundsMin = {-1.0f, -1.0f, 7.0f, 0.0f};
  gpuPageBounds[3].localBoundsMax = {1.0f, 1.0f, 9.0f, 0.0f};
  graph.bufferWrite(pageBoundsBuffer, 0u, sizeof(gpuPageBounds), gpuPageBounds.data());

  SparseVoxelRasterUniformsGPU raster{};
  raster.worldToCamera = identityMatrix();
  raster.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.001f};
  raster.focalPrincipal = {800.0f, 800.0f, 0.0f, 0.0f};
  raster.visibleVoxelCapacity = capacity;
  raster.projectedVoxelCapacity = capacity;
  raster.lodEnabled = 1u;
  raster.lodPixelThreshold = threshold;
  const svrRendering::SparseVoxelLODPass::Buffers buffers{
    counters, visibleInstances, visibleVoxels, instancesBuffer, nodesBuffer, centersBuffer, queueA, queueB, queueCounts, lodArgs, rasterArgs,
    metadataBuffer, bits, priorities, requestInstances,
    lodSeeds,
    pageBoundsBuffer,
    rejectedNodes,
    recoveryState,
    temporalVisibleVoxels,
    preSubmittedFlags,
  };
  const uint32_t rootCount = installedChildren ? 1u : 2u;
  const uint32_t seedCount = sharedInstances ? 2u : 0u;
  const uint32_t seedCapacity = sharedInstances ? 2u : 0u;
  uint32_t passIndex = 0u;
  graph.registerPass<svrRendering::SparseVoxelLODPass>(
      "g16.lod.initialize", passIndex++, svrRendering::SparseVoxelLODPass::Kernel::Initialize, buffers,
      svrRendering::SparseVoxelLODPass::Settings{raster, {0u, rootCount, capacity, 0u, 4u, 1u, 1u, 0u, seedCount, seedCapacity, 0u, 0u}});
  graph.registerPass<svrRendering::SparseVoxelLODPass>(
      "g16.lod.traverse", passIndex++, svrRendering::SparseVoxelLODPass::Kernel::Traverse, buffers,
      svrRendering::SparseVoxelLODPass::Settings{raster, {0u, rootCount, capacity, 0u, 4u, 1u, 1u, 0u, seedCount, seedCapacity, 0u, 0u}});
  graph.registerPass<svrRendering::SparseVoxelLODPass>(
      "g16.lod.finalize", passIndex++, svrRendering::SparseVoxelLODPass::Kernel::FinalizeSelection, buffers,
      svrRendering::SparseVoxelLODPass::Settings{raster, {0u, rootCount, capacity, 0u, 4u, 1u, 1u, 0u, seedCount, seedCapacity, 0u, 0u}});
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g16.lod.copyBits", passIndex++, bits, 0u, sizeof(uint32_t), readback, offsetof(LODRequestReadback, bits), sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g16.lod.copyPriorities", passIndex++, priorities, 0u, 4u * sizeof(uint32_t), readback, offsetof(LODRequestReadback, priorities), 4u * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g16.lod.copyInstances", passIndex++, requestInstances, 0u, 4u * sizeof(uint32_t), readback, offsetof(LODRequestReadback, instances), 4u * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g16.lod.copyCounters", passIndex++, counters, 0u, sizeof(SparseVoxelFrameCountersGPU), readback, offsetof(LODRequestReadback, counters), sizeof(SparseVoxelFrameCountersGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g16.lod.copyVisible", passIndex++, visibleVoxels, 0u, capacity * sizeof(VisibleVoxelGPU), readback, offsetof(LODRequestReadback, visible), capacity * sizeof(VisibleVoxelGPU));
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
  LODRequestReadback result{};
  graph.bufferRead(readback, 0u, sizeof(result), [&](const void *data) { std::memcpy(&result, data, sizeof(result)); });
  return result;
}

void testLODRequestEmission(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const LODRequestReadback requested = runLODRequests(rhi, 1.0f, true, false);
  assert(requested.bits == (1u << 3u));
  for (uint32_t page = 0u; page < 3u; ++page)
  {
    assert(requested.priorities[page] == 0u);
    assert(requested.instances[page] == kInvalidSparseVoxelIndex);
  }
  VirtualSparseVoxelFilePageRecord largeNode{};
  largeNode.localBoundsMin = {-1.0f, -1.0f, 7.0f};
  largeNode.localBoundsMax = {1.0f, 1.0f, 9.0f};
  SparseVoxelPagePriorityView view{};
  view.modelToWorld = identityMatrix();
  view.worldToCamera = identityMatrix();
  view.focalPixels = {800.0f, 800.0f};
  view.cameraWorldPosition = {0.0f, 0.0f, 0.0f};
  view.nearPlane = 0.001f;
  assert(requested.priorities[3] == scoreSparseVoxelPagePriority(largeNode, view));
  assert(requested.instances[3] == 0u);
  assert(requested.counters.visibleVoxelCount == 2u); // missing refinement keeps the coarse nodes renderable
  assert(requested.visible[0].voxelIndex < 2u && requested.visible[1].voxelIndex < 2u);

  const LODRequestReadback belowThreshold = runLODRequests(rhi, 100000.0f, true, false);
  assert(belowThreshold.bits == 0u);
  assert(belowThreshold.counters.visibleVoxelCount == 2u);
  const LODRequestReadback terminal = runLODRequests(rhi, 1.0f, false, false);
  assert(terminal.bits == 0u);
  const LODRequestReadback installed = runLODRequests(rhi, 1.0f, true, true);
  assert(installed.bits == 0u);

  const LODRequestReadback shared = runLODRequests(rhi, 1.0f, true, false, true);
  assert(shared.bits == (1u << 3u));
  assert(shared.priorities[3] == scoreSparseVoxelPagePriority(largeNode, view));
  assert(shared.counters.visibleVoxelCount == 2u);
  assert(shared.counters.visibleInstanceCount == 2u);
  assert(shared.visible[0].instanceIndex != shared.visible[1].instanceIndex);
}

struct BrickLODReadback
{
  uint32_t visibleCount = 0u;
  uint32_t visibleBrick = kSparseVoxelBrickInvalidIndex;
  std::array<uint32_t, 2> history{};
  uint32_t requestBits = 0u;
  uint32_t requestPriority = 0u;
  uint32_t requestGeneration = 0u;
  uint32_t requestDesiredLOD = 0xffffffffu;
};

struct BrickVirtualCutReadback
{
  std::array<uint32_t, 16> counters{};
  std::array<uint32_t, 18> visible{};
  std::array<uint32_t, 18> history{};
};

uint32_t brickLODSelectionHashCPU(
    const uint32_t index,
    const SparseVoxelBrickNodeGPU &node)
{
  uint32_t value = index ^ ((node.level & 0xffffu) * 0x9e3779b9u);
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return value;
}

float brickProjectedMetricPixelsCPU(
    const SparseVoxelBrickNodeGPU &node,
    const float error,
    const float focalPixels)
{
  const float side = node.minimumVoxelSize[3] *
      static_cast<float>(kSparseVoxelBrickSide);
  float closest[3]{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float minimum = node.minimumVoxelSize[axis];
    const float maximum = minimum + side;
    closest[axis] = std::max(minimum, std::min(0.0f, maximum));
  }
  const float distance = std::max(
      std::sqrt(
          closest[0] * closest[0] +
          closest[1] * closest[1] +
          closest[2] * closest[2]),
      1.0e-4f);
  return error * focalPixels / distance;
}

float brickStableLODThresholdCPU(
    const uint32_t history,
    const float lodPixelThreshold)
{
  if (!(lodPixelThreshold > 0.0f))
    return 0.0f;
  if ((history & kSparseVoxelBrickHistoryLODInitialized) == 0u)
    return lodPixelThreshold;
  if ((history & kSparseVoxelBrickHistoryPreviousLOD) != 0u)
    return lodPixelThreshold * 1.25f;
  return lodPixelThreshold * 0.75f;
}

bool brickGroupSelectsRepresentativeCPU(
    const uint32_t groupIndex,
    const std::vector<SparseVoxelBrickNodeGPU> &nodes,
    const std::vector<SparseVoxelBrickLODGroupGPU> &groups,
    const std::vector<uint32_t> &history,
    const float focalPixels,
    const float lodPixelThreshold)
{
  if (groupIndex >= groups.size())
    return false;
  const SparseVoxelBrickLODGroupGPU &group = groups[groupIndex];
  const uint32_t representativeIndex = group.representativeNode;
  if (representativeIndex == kSparseVoxelBrickLODGroupInvalidIndex ||
      representativeIndex >= nodes.size())
    return false;
  const SparseVoxelBrickNodeGPU &representative = nodes[representativeIndex];
  if (representative.occupiedCount == 0u ||
      !(representative.minimumVoxelSize[3] > 0.0f) ||
      (representative.level & kSparseVoxelBrickForceLeafRaster) != 0u)
    return false;
  if (representative.reservedTailPadding != 0u)
  {
    const bool certifiedSurfaceRepresentative =
        (group.flags &
         kSparseVoxelBrickLODGroupSurfaceRepresentativeCertified) != 0u;
    if (representative.childCount != 8u ||
        (!certifiedSurfaceRepresentative &&
         ((group.flags & kSparseVoxelBrickLODGroupBoundaryMask) != 0u ||
          representative.occupiedCount != kSparseVoxelBrickVoxelCount)))
      return false;
  }

  const float groupMetric = group.sharedError > 0.0f
      ? group.sharedError
      : representative.minimumVoxelSize[3];
  const float groupPixels =
      brickProjectedMetricPixelsCPU(representative, groupMetric, focalPixels);
  const float threshold = brickStableLODThresholdCPU(
      history[representativeIndex], lodPixelThreshold);
  return groupPixels <= threshold;
}

std::vector<uint32_t> cpuOracleVirtualBrickCut(
    const std::vector<SparseVoxelBrickNodeGPU> &nodes,
    const std::vector<SparseVoxelBrickLODGroupGPU> &groups,
    const std::vector<uint32_t> &nodeGroups,
    const std::vector<uint32_t> &representativeGroups,
    const std::vector<uint32_t> &history,
    const float focalPixels,
    const float lodPixelThreshold)
{
  std::vector<uint32_t> selected;
  for (uint32_t index = 0u; index < nodes.size(); ++index)
  {
    const SparseVoxelBrickNodeGPU &node = nodes[index];
    bool overrideEnabled = false;
    bool overrideSelected = false;
    bool bypassParentLOD = false;

    if (index < nodeGroups.size())
    {
      const uint32_t groupIndex = nodeGroups[index];
      if (groupIndex != kSparseVoxelBrickLODGroupInvalidIndex &&
          groupIndex < groups.size() &&
          groups[groupIndex].representativeNode !=
              kSparseVoxelBrickLODGroupInvalidIndex)
      {
        if (brickGroupSelectsRepresentativeCPU(
                groupIndex, nodes, groups, history, focalPixels,
                lodPixelThreshold))
        {
          overrideEnabled = true;
          overrideSelected = false;
        }
        else
        {
          bypassParentLOD = true;
        }
      }
    }

    if (index < representativeGroups.size())
    {
      const uint32_t groupIndex = representativeGroups[index];
      if (groupIndex != kSparseVoxelBrickLODGroupInvalidIndex &&
          groupIndex < groups.size())
      {
        overrideEnabled = true;
        overrideSelected = brickGroupSelectsRepresentativeCPU(
            groupIndex, nodes, groups, history, focalPixels,
            lodPixelThreshold);
      }
    }

    bool nodeSelected = false;
    if (overrideEnabled)
    {
      nodeSelected = overrideSelected;
    }
    else
    {
      const float voxelSize = node.minimumVoxelSize[3];
      const float selfMetric =
          node.selfError > 0.0f ? node.selfError : voxelSize;
      const float selfPixels =
          brickProjectedMetricPixelsCPU(node, selfMetric, focalPixels);
      const float selfThreshold =
          brickStableLODThresholdCPU(history[index], lodPixelThreshold);
      const bool forceLeafRaster =
          (node.level & kSparseVoxelBrickForceLeafRaster) != 0u;
      const bool fineEnough = node.childCount == 0u ||
          (!forceLeafRaster && selfPixels <= selfThreshold);
      if (fineEnough)
      {
        nodeSelected = true;
        if (!bypassParentLOD && !forceLeafRaster &&
            node.parentIndex != kSparseVoxelBrickInvalidIndex &&
            node.parentIndex < nodes.size())
        {
          const SparseVoxelBrickNodeGPU &parent = nodes[node.parentIndex];
          const float parentMetric =
              node.parentError > 0.0f && node.selfError > 0.0f
              ? node.parentError
              : parent.minimumVoxelSize[3];
          const float parentPixels = brickProjectedMetricPixelsCPU(
              parent, parentMetric, focalPixels);
          const float parentThreshold = brickStableLODThresholdCPU(
              history[node.parentIndex], lodPixelThreshold);
          if (parentPixels <= parentThreshold)
            nodeSelected = false;
        }
      }
    }

    if (nodeSelected)
      selected.push_back(index);
  }
  return selected;
}

BrickVirtualCutReadback runBrickVirtualGeometryCut(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const std::vector<SparseVoxelBrickNodeGPU> &nodes,
    const std::vector<SparseVoxelBrickLODGroupGPU> &groups,
    const std::vector<uint32_t> &nodeGroups,
    const std::vector<uint32_t> &representativeGroups,
    const std::vector<uint32_t> &initialHistory,
    const std::array<SparseVoxelTerrainPageStateGPU, 2> &pageStates,
    const float focalPixels,
    const float lodPixelThreshold)
{
  using namespace ::rendering;
  RenderGraph graph(&rhi);
  auto storage = [&](const char *name, const uint64_t size,
                     const bool indirect = false)
  {
    return graph.createBuffer({
        .name = name,
        .size = size,
        .usage = BufferUsage::BufferUsage_Storage |
            BufferUsage::BufferUsage_Push |
            BufferUsage::BufferUsage_CopySrc |
            (indirect ? BufferUsage::BufferUsage_Indirect
                      : BufferUsage::BufferUsage_None),
    });
  };

  const uint32_t nodeCount = static_cast<uint32_t>(nodes.size());
  const Buffer nodeBuffer = storage(
      "brick.virtualCut.nodes",
      std::max<uint64_t>(nodeCount, 1u) *
          sizeof(SparseVoxelBrickNodeGPU));
  const Buffer visible = storage(
      "brick.virtualCut.visible",
      std::max<uint64_t>(nodeCount, 1u) * sizeof(uint32_t));
  const Buffer counters = storage(
      "brick.virtualCut.counters", 16u * sizeof(uint32_t));
  const Buffer drawArgs = storage(
      "brick.virtualCut.drawArgs",
      sizeof(SparseVoxelBrickDrawIndirectGPU), true);
  const Buffer history = storage(
      "brick.virtualCut.history",
      std::max<uint64_t>(nodeCount, 1u) * sizeof(uint32_t));
  const Buffer microCounters = storage(
      "brick.virtualCut.microCounters", sizeof(SparseVoxelMicroCountersGPU));
  const Buffer requestBits = storage(
      "brick.virtualCut.requestBits", sizeof(uint32_t));
  const Buffer requestPriorities = storage(
      "brick.virtualCut.requestPriorities", 2u * sizeof(uint32_t));
  const Buffer pageStatesBuffer = storage(
      "brick.virtualCut.pageStates",
      pageStates.size() * sizeof(SparseVoxelTerrainPageStateGPU));
  const Buffer requestGenerations = storage(
      "brick.virtualCut.requestGenerations", 2u * sizeof(uint32_t));
  const Buffer requestDesiredLODs = storage(
      "brick.virtualCut.requestDesiredLODs", 2u * sizeof(uint32_t));
  const Buffer lodGroups = storage(
      "brick.virtualCut.lodGroups",
      std::max<uint64_t>(groups.size(), 1u) *
          sizeof(SparseVoxelBrickLODGroupGPU));
  const Buffer nodeLODGroupIndices = storage(
      "brick.virtualCut.nodeLODGroupIndices",
      std::max<uint64_t>(nodeGroups.size(), 1u) * sizeof(uint32_t));
  const Buffer representativeLODGroupIndices = storage(
      "brick.virtualCut.representativeLODGroupIndices",
      std::max<uint64_t>(representativeGroups.size(), 1u) *
          sizeof(uint32_t));
  const Buffer readback = graph.createBuffer({
      .name = "brick.virtualCut.readback",
      .size = sizeof(BrickVirtualCutReadback),
      .usage = BufferUsage::BufferUsage_Pull |
          BufferUsage::BufferUsage_CopyDst,
  });

  graph.bufferWrite(
      nodeBuffer, 0u, nodes.size() * sizeof(nodes[0]),
      const_cast<SparseVoxelBrickNodeGPU *>(nodes.data()));
  graph.bufferWrite(
      history, 0u, initialHistory.size() * sizeof(initialHistory[0]),
      const_cast<uint32_t *>(initialHistory.data()));
  graph.bufferWrite(
      pageStatesBuffer, 0u, sizeof(pageStates),
      const_cast<SparseVoxelTerrainPageStateGPU *>(pageStates.data()));
  if (!groups.empty())
    graph.bufferWrite(
        lodGroups, 0u, groups.size() * sizeof(groups[0]),
        const_cast<SparseVoxelBrickLODGroupGPU *>(groups.data()));
  graph.bufferWrite(
      nodeLODGroupIndices, 0u,
      nodeGroups.size() * sizeof(nodeGroups[0]),
      const_cast<uint32_t *>(nodeGroups.data()));
  graph.bufferWrite(
      representativeLODGroupIndices, 0u,
      representativeGroups.size() * sizeof(representativeGroups[0]),
      const_cast<uint32_t *>(representativeGroups.data()));

  const Texture hzb = graph.createTexture({
      .name = "brick.virtualCut.hzb",
      .format = Format::Format_R32Float,
      .width = 1u,
      .height = 1u,
      .usage = ImageUsage::ImageUsage_Sampled,
      .mipLevels = 1u,
  });
  const svrRendering::SparseVoxelBrickCullPass::Buffers buffers{
      nodeBuffer, visible, counters, drawArgs, history, microCounters,
      requestBits, requestPriorities, pageStatesBuffer, requestGenerations,
      requestDesiredLODs, lodGroups, nodeLODGroupIndices,
      representativeLODGroupIndices};
  SparseVoxelBrickCullUniformsGPU uniforms{};
  uniforms.cameraWorldPosition = {0.0f, 0.0f, 0.0f, 1.0f};
  uniforms.focalPrincipal = {focalPixels, focalPixels, 0.0f, 0.0f};
  uniforms.viewport = {128u, 128u, 1u, 1u};
  uniforms.rootCount = 2u;
  uniforms.nodeCount = nodeCount;
  uniforms.visibleCapacity = nodeCount;
  uniforms.lodGroupCount = static_cast<uint32_t>(groups.size());
  uniforms.lodPixelThreshold = lodPixelThreshold;
  uniforms.cullingFlags = kSparseVoxelBrickCullLODDiagnostics;
  uniforms.hzbMipCount = 1u;
  uniforms.terrainPageRequestCapacity =
      static_cast<uint32_t>(pageStates.size());
  uniforms.terrainSplitErrorPixels = 4.0f;
  uniforms.terrainMergeErrorPixels = 2.5f;
  uniforms.terrainOutputPixelScale = 1.0f;
  uniforms.terrainMinimumResidentFrames = 0u;
  uniforms.terrainFrameIndex = 100u;

  uint32_t passIndex = 0u;
  const auto addPass = [&](const char *name,
                           const svrRendering::SparseVoxelBrickCullPass::Kernel kernel,
                           const uint32_t phase)
  {
    SparseVoxelBrickCullUniformsGPU passUniforms = uniforms;
    passUniforms.phase = phase;
    graph.registerPass<svrRendering::SparseVoxelBrickCullPass>(
        name, passIndex++, kernel, buffers,
        svrRendering::SparseVoxelBrickCullPass::Settings{passUniforms}, hzb);
  };
  addPass("brick.virtualCut.reset",
      svrRendering::SparseVoxelBrickCullPass::Kernel::Reset, 0u);
  addPass("brick.virtualCut.phase0",
      svrRendering::SparseVoxelBrickCullPass::Kernel::Cull, 0u);
  addPass("brick.virtualCut.phase1",
      svrRendering::SparseVoxelBrickCullPass::Kernel::Cull, 1u);
  addPass("brick.virtualCut.commit",
      svrRendering::SparseVoxelBrickCullPass::Kernel::CommitLODHistory, 1u);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.virtualCut.copyCounters", passIndex++, counters, 0u,
      16u * sizeof(uint32_t), readback,
      offsetof(BrickVirtualCutReadback, counters),
      16u * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.virtualCut.copyVisible", passIndex++, visible, 0u,
      nodeCount * sizeof(uint32_t), readback,
      offsetof(BrickVirtualCutReadback, visible),
      nodeCount * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.virtualCut.copyHistory", passIndex++, history, 0u,
      nodeCount * sizeof(uint32_t), readback,
      offsetof(BrickVirtualCutReadback, history),
      nodeCount * sizeof(uint32_t));
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  BrickVirtualCutReadback result{};
  graph.bufferRead(readback, 0u, sizeof(result),
      [&](const void *data) { std::memcpy(&result, data, sizeof(result)); });
  return result;
}

void testVirtualBrickCutOracleMatchesGPUInterChunkMixedCut(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t nodeCount = 18u;
  constexpr uint32_t leftParent = 0u;
  constexpr uint32_t rightParent = 1u;
  constexpr uint32_t leftFirstChild = 2u;
  constexpr uint32_t rightFirstChild = 10u;
  constexpr float focalPixels = 100.0f;
  constexpr float lodPixelThreshold = 10.0f;
  constexpr uint32_t generation = 77u;
  std::vector<SparseVoxelBrickNodeGPU> nodes(nodeCount);
  const auto initNode =
      [&](const uint32_t index,
          const float minimumX,
          const float minimumY,
          const float minimumZ,
          const float voxelSize,
          const uint32_t terrainSlot)
  {
    auto &node = nodes[index];
    node.minimumVoxelSize = {minimumX, minimumY, minimumZ, voxelSize};
    node.packedOccupiedBounds = 0u;
    node.level = 0u;
    node.occupiedCount = 1u;
    node.instanceIndex = terrainSlot;
    node.selfError = 1.0f;
    node.parentError = std::numeric_limits<float>::max();
    node.reservedTailPadding = generation;
  };
  initNode(leftParent, -16.0f, 0.0f, 100.0f, 2.0f, 0u);
  initNode(rightParent, 0.0f, 0.0f, 100.0f, 2.0f, 1u);
  nodes[leftParent].childCount = 8u;
  nodes[leftParent].firstChild = leftFirstChild;
  nodes[leftParent].occupiedCount = kSparseVoxelBrickVoxelCount - 8u;
  nodes[rightParent].childCount = 8u;
  nodes[rightParent].firstChild = rightFirstChild;
  nodes[rightParent].occupiedCount = kSparseVoxelBrickVoxelCount;

  for (uint32_t child = 0u; child < 8u; ++child)
  {
    const uint32_t x = child & 1u;
    const uint32_t y = (child >> 1u) & 1u;
    const uint32_t z = (child >> 2u) & 1u;
    initNode(
        leftFirstChild + child,
        -16.0f + static_cast<float>(x * 8u),
        static_cast<float>(y * 8u),
        100.0f + static_cast<float>(z * 8u),
        1.0f,
        0u);
    nodes[leftFirstChild + child].parentIndex = leftParent;
    nodes[leftFirstChild + child].parentError = nodes[leftParent].selfError;
    initNode(
        rightFirstChild + child,
        static_cast<float>(x * 8u),
        static_cast<float>(y * 8u),
        100.0f + static_cast<float>(z * 8u),
        1.0f,
        1u);
    nodes[rightFirstChild + child].parentIndex = rightParent;
    nodes[rightFirstChild + child].parentError =
        nodes[rightParent].selfError;
  }

  std::vector<SparseVoxelBrickLODGroupGPU> groups(2u);
  groups[0].nodeCount = 8u;
  groups[0].level = 0u;
  groups[0].sharedError = 1.0f;
  groups[0].minimumError = 1.0f;
  groups[0].maximumError = 1.0f;
  groups[0].flags = kSparseVoxelBrickLODGroupBoundaryMinusX |
      kSparseVoxelBrickLODGroupBoundaryPlusX;
  groups[0].representativeNode = leftParent;
  groups[1].nodeCount = 8u;
  groups[1].level = 0u;
  groups[1].sharedError = 20.0f;
  groups[1].minimumError = 1.0f;
  groups[1].maximumError = 20.0f;
  groups[1].representativeNode = rightParent;

  std::vector<uint32_t> nodeGroups(
      nodeCount, kSparseVoxelBrickLODGroupInvalidIndex);
  std::vector<uint32_t> representativeGroups(
      nodeCount, kSparseVoxelBrickLODGroupInvalidIndex);
  representativeGroups[leftParent] = 0u;
  representativeGroups[rightParent] = 1u;
  for (uint32_t child = 0u; child < 8u; ++child)
  {
    nodeGroups[leftFirstChild + child] = 0u;
    nodeGroups[rightFirstChild + child] = 1u;
  }
  std::vector<uint32_t> history(
      nodeCount, kSparseVoxelBrickHistoryLODInitialized);
  const std::array<SparseVoxelTerrainPageStateGPU, 2> pageStates{{
      {generation, 0u, 0u,
       kSparseVoxelTerrainPageStateProgressiveGenerated},
      {generation, 0u, 0u,
       kSparseVoxelTerrainPageStateProgressiveGenerated},
  }};

  const auto runAndAssertGPUEqualsOracle =
      [&](const std::vector<SparseVoxelBrickLODGroupGPU> &testGroups,
          const std::vector<uint32_t> &oracle)
  {
    uint32_t expectedHashXor = 0u;
    uint32_t expectedHashSum = 0u;
    for (const uint32_t index : oracle)
    {
      const uint32_t hash = brickLODSelectionHashCPU(index, nodes[index]);
      expectedHashXor ^= hash;
      expectedHashSum += hash;
    }

    const BrickVirtualCutReadback gpu = runBrickVirtualGeometryCut(
        rhi, nodes, testGroups, nodeGroups, representativeGroups, history,
        pageStates, focalPixels, lodPixelThreshold);
    assert(gpu.counters[12] == oracle.size());
    assert(gpu.counters[13] == expectedHashXor);
    assert(gpu.counters[14] == expectedHashSum);
    // All oracle-selected bricks transition from initialized/unselected history
    // on this first cut.
    assert(gpu.counters[15] == oracle.size());
    assert(gpu.counters[0] == oracle.size());
    std::vector<uint32_t> gpuVisible(
        gpu.visible.begin(), gpu.visible.begin() + oracle.size());
    std::sort(gpuVisible.begin(), gpuVisible.end());
    assert(gpuVisible == oracle);
    for (const uint32_t index : oracle)
      assert((gpu.history[index] & kSparseVoxelBrickHistoryPreviousLOD) != 0u);
    assert((gpu.history[rightParent] &
            kSparseVoxelBrickHistoryPreviousLOD) == 0u);
  };

  // Uncertified generated surface/boundary representatives remain conservative:
  // the left parent has a good error metric, but it is a non-full terrain
  // surface touching a page edge, so all children own the cut.
  const std::vector<uint32_t> uncertifiedOracle = cpuOracleVirtualBrickCut(
      nodes, groups, nodeGroups, representativeGroups, history,
      focalPixels, lodPixelThreshold);
  assert(uncertifiedOracle.size() == 16u);
  for (uint32_t child = 0u; child < 16u; ++child)
    assert(uncertifiedOracle[child] == leftFirstChild + child);
  runAndAssertGPUEqualsOracle(groups, uncertifiedOracle);

  std::vector<SparseVoxelBrickLODGroupGPU> certifiedGroups = groups;
  certifiedGroups[0].flags |=
      kSparseVoxelBrickLODGroupSurfaceRepresentativeCertified;
  const std::vector<uint32_t> oracle = cpuOracleVirtualBrickCut(
      nodes, certifiedGroups, nodeGroups, representativeGroups, history,
      focalPixels, lodPixelThreshold);
  assert(oracle.size() == 9u);
  assert(oracle[0] == leftParent);
  for (uint32_t child = 0u; child < 8u; ++child)
    assert(oracle[child + 1u] == rightFirstChild + child);
  assert(std::find(oracle.begin(), oracle.end(), rightParent) ==
      oracle.end());
  assert(std::none_of(
      oracle.begin(), oracle.end(),
      [](const uint32_t index)
      {
        return index >= leftFirstChild && index < rightFirstChild;
      }));
  runAndAssertGPUEqualsOracle(certifiedGroups, oracle);
}

struct BrickHZBReadback
{
  std::array<uint32_t, 16> counters{};
  uint32_t visibleBrick = kSparseVoxelBrickInvalidIndex;
};

BrickHZBReadback runBrickHZBCull(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const float occluderDepth,
    const bool farDepthOutsideTightFootprint,
    const bool farDepthInsideTightFootprint,
    const float dilationVoxels)
{
  using namespace ::rendering;
  constexpr uint32_t width = 64u;
  constexpr uint32_t height = 64u;
  RenderGraph graph(&rhi);
  auto storage = [&](const char *name, const uint64_t size,
                     const bool indirect = false)
  {
    return graph.createBuffer({
        .name = name,
        .size = size,
        .usage = BufferUsage::BufferUsage_Storage |
            BufferUsage::BufferUsage_Push |
            BufferUsage::BufferUsage_CopySrc |
            (indirect ? BufferUsage::BufferUsage_Indirect
                      : BufferUsage::BufferUsage_None),
    });
  };

  SparseVoxelBrickNodeGPU node{};
  // One occupied voxel centred on the view axis. The allocated brick is much
  // larger, but packedOccupiedBounds=0 makes the submitted proxy exactly this
  // one-cell box.
  node.minimumVoxelSize = {-0.5f, -0.5f, -10.0f, 1.0f};
  node.occupiedCount = 1u;
  const Buffer nodes = storage("brick.hzb.nodes", sizeof(node));
  const Buffer visible = storage("brick.hzb.visible", sizeof(uint32_t));
  const Buffer counters = storage(
      "brick.hzb.counters", 16u * sizeof(uint32_t));
  const Buffer drawArgs = storage(
      "brick.hzb.drawArgs", sizeof(SparseVoxelBrickDrawIndirectGPU), true);
  const Buffer history = storage("brick.hzb.history", sizeof(uint32_t));
  const Buffer microCounters = storage(
      "brick.hzb.microCounters", sizeof(SparseVoxelMicroCountersGPU));
  const Buffer requestBits = storage("brick.hzb.requestBits", sizeof(uint32_t));
  const Buffer requestPriorities = storage(
      "brick.hzb.requestPriorities", sizeof(uint32_t));
  const Buffer pageStates = storage(
      "brick.hzb.pageStates", sizeof(SparseVoxelTerrainPageStateGPU));
  const Buffer requestGenerations = storage(
      "brick.hzb.requestGenerations", sizeof(uint32_t));
  const Buffer requestDesiredLODs = storage(
      "brick.hzb.requestDesiredLODs", sizeof(uint32_t));
  const Buffer lodGroups = storage(
      "brick.hzb.lodGroups", sizeof(SparseVoxelBrickLODGroupGPU));
  const Buffer nodeLODGroupIndices = storage(
      "brick.hzb.nodeLODGroupIndices", sizeof(uint32_t));
  const Buffer representativeLODGroupIndices = storage(
      "brick.hzb.representativeLODGroupIndices", sizeof(uint32_t));
  const Buffer readback = graph.createBuffer({
      .name = "brick.hzb.readback",
      .size = sizeof(BrickHZBReadback),
      .usage = BufferUsage::BufferUsage_Pull |
          BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(nodes, 0u, sizeof(node), &node);
  uint32_t hiddenHistory =
      kSparseVoxelBrickHistoryLODInitialized |
      kSparseVoxelBrickHistoryPreviousLOD | 2u;
  graph.bufferWrite(
      history, 0u, sizeof(hiddenHistory), &hiddenHistory);

  const Texture hzb = graph.createTexture({
      .name = "brick.hzb.texture",
      .format = Format::Format_R32Float,
      .width = width,
      .height = height,
      .mipLevels = 1u,
      .usage = ImageUsage::ImageUsage_Storage |
          ImageUsage::ImageUsage_Sampled,
  });
  std::vector<float> hzbPixels(width * height, occluderDepth);
  if (farDepthOutsideTightFootprint)
  {
    // The tight proxy covers approximately texels 29..35. These zero-depth
    // columns are reached only by the one-voxel safety dilation. Sampling them
    // would make a fully hidden brick look visible even though its raster
    // proxy cannot touch them.
    for (uint32_t y = 0u; y < height; ++y)
      for (uint32_t x = 0u; x < width; ++x)
        if (x <= 27u || x >= 37u || y <= 27u || y >= 37u)
          hzbPixels[y * width + x] = 0.0f;
  }
  if (farDepthInsideTightFootprint)
    hzbPixels[32u * width + 32u] = 0.0f;

  // 90-degree, aspect-one, finite reverse-Z projection. Column-major layout
  // matches SparseVoxelBrickCullUniformsGPU and WGSL mat4x4.
  constexpr float nearPlane = 0.1f;
  constexpr float farPlane = 100.0f;
  std::array<float, 16> projection{};
  projection[0] = 1.0f;
  projection[5] = 1.0f;
  projection[10] = nearPlane / (farPlane - nearPlane);
  projection[11] = -1.0f;
  projection[14] = farPlane * nearPlane / (farPlane - nearPlane);

  SparseVoxelBrickCullUniformsGPU uniforms{};
  uniforms.worldToClip = projection;
  uniforms.cameraWorldPosition = {0.0f, 0.0f, 0.0f, nearPlane};
  uniforms.focalPrincipal = {32.0f, 32.0f, 0.0f, 0.0f};
  uniforms.viewport = {width, height, 1u, 1u};
  uniforms.rootCount = 1u;
  uniforms.nodeCount = 1u;
  uniforms.visibleCapacity = 1u;
  uniforms.lodPixelThreshold = 1.0f;
  uniforms.cullingFlags = kSparseVoxelBrickCullOcclusion |
      kSparseVoxelBrickCullReverseZ |
      kSparseVoxelBrickCullMeasureArea;
  uniforms.hzbMipCount = 1u;
  uniforms.hzbFirstValidMip = 0u;
  uniforms.phase = 1u;
  uniforms.occlusionDilationVoxels = dilationVoxels;

  const svrRendering::SparseVoxelBrickCullPass::Buffers buffers{
      nodes, visible, counters, drawArgs, history, microCounters,
      requestBits, requestPriorities, pageStates, requestGenerations,
      requestDesiredLODs, lodGroups, nodeLODGroupIndices,
      representativeLODGroupIndices};
  uint32_t passIndex = 0u;
  graph.registerPass<svrRendering::SparseVoxelBrickCullPass>(
      "brick.hzb.reset", passIndex++,
      svrRendering::SparseVoxelBrickCullPass::Kernel::Reset, buffers,
      svrRendering::SparseVoxelBrickCullPass::Settings{uniforms}, hzb);
  graph.registerPass<svrRendering::SparseVoxelBrickCullPass>(
      "brick.hzb.cull", passIndex++,
      svrRendering::SparseVoxelBrickCullPass::Kernel::Cull, buffers,
      svrRendering::SparseVoxelBrickCullPass::Settings{uniforms}, hzb);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.hzb.copyCounters", passIndex++, counters, 0u,
      16u * sizeof(uint32_t), readback,
      offsetof(BrickHZBReadback, counters), 16u * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.hzb.copyVisible", passIndex++, visible, 0u, sizeof(uint32_t),
      readback, offsetof(BrickHZBReadback, visibleBrick), sizeof(uint32_t));
  graph.compile();
  graph.textureWrite(
      hzb, hzbPixels.size() * sizeof(float), hzbPixels.data());
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  BrickHZBReadback result{};
  graph.bufferRead(
      readback, 0u, sizeof(result),
      [&](const void *data) { std::memcpy(&result, data, sizeof(result)); });
  return result;
}

void testBrickHZBUsesTightFootprintAndConservativeDepth(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  // Empty HZB data outside the submitted proxy must not keep a hidden brick.
  const BrickHZBReadback tight = runBrickHZBCull(
      rhi, 0.1f, true, false, 1.0f);
  assert(tight.counters[0] == 0u);
  assert(tight.counters[1] == 1u);
  assert(tight.counters[4] == 1u);
  assert(tight.counters[10] == 1u);

  // A real hole inside the tight footprint still keeps the brick visible.
  const BrickHZBReadback hole = runBrickHZBCull(
      rhi, 0.1f, false, true, 1.0f);
  assert(hole.counters[0] == 1u);
  assert(hole.counters[1] == 0u);
  assert(hole.visibleBrick == 0u);

  // The screen footprint is tight, but the nearest-depth safety margin must
  // remain conservative. At this depth, the undilated box is hidden while the
  // one-voxel depth bound reaches in front of the occluder and stays visible.
  const BrickHZBReadback undilated = runBrickHZBCull(
      rhi, 0.0115f, false, false, 0.0f);
  const BrickHZBReadback dilated = runBrickHZBCull(
      rhi, 0.0115f, false, false, 1.0f);
  assert(undilated.counters[10] == 1u);
  assert(dilated.counters[0] == 1u);
  assert(dilated.counters[10] == 0u);
}

BrickLODReadback runBrickLODCut(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const float focalPixels,
    const std::array<uint32_t, 2> &initialHistory,
    const SparseVoxelTerrainPageStateGPU pageState = {},
    const uint32_t feedbackFrame = 0u,
    const bool feedbackEnabled = false,
    const float distanceErrorGrowthPerKilometre = 0.0f,
    const float distanceErrorExponent = 1.0f,
    const bool useRepresentativeLODGroup = false)
{
  using namespace ::rendering;
  RenderGraph graph(&rhi);
  auto storage = [&](const char *name, const uint64_t size,
                     const bool indirect = false)
  {
    return graph.createBuffer({
        .name = name,
        .size = size,
        .usage = BufferUsage::BufferUsage_Storage |
            BufferUsage::BufferUsage_Push |
            BufferUsage::BufferUsage_CopySrc |
            (indirect ? BufferUsage::BufferUsage_Indirect
                      : BufferUsage::BufferUsage_None),
    });
  };

  std::array<SparseVoxelBrickNodeGPU, 2> nodes{};
  nodes[0].minimumVoxelSize = {0.0f, 0.0f, 10.0f, 2.0f};
  nodes[0].childCount = 1u;
  nodes[0].occupiedCount = 1u;
  nodes[0].firstChild = 1u;
  nodes[1].minimumVoxelSize = {0.0f, 0.0f, 10.0f, 1.0f};
  nodes[1].parentIndex = 0u;
  nodes[1].occupiedCount = 1u;
  nodes[1].level = pageState.currentLOD << 8u;
  nodes[1].instanceIndex = 0u;

  const Buffer nodeBuffer = storage(
      "brick.lod.nodes", sizeof(nodes));
  const Buffer visible = storage(
      "brick.lod.visible", 2u * sizeof(uint32_t));
  const Buffer counters = storage(
      "brick.lod.counters", 12u * sizeof(uint32_t));
  const Buffer drawArgs = storage(
      "brick.lod.drawArgs", sizeof(SparseVoxelBrickDrawIndirectGPU), true);
  const Buffer history = storage(
      "brick.lod.history", sizeof(initialHistory));
  const Buffer microCounters = storage(
      "brick.lod.microCounters", sizeof(SparseVoxelMicroCountersGPU));
  const Buffer requestBits = storage(
      "brick.lod.requestBits", sizeof(uint32_t));
  const Buffer requestPriorities = storage(
      "brick.lod.requestPriorities", sizeof(uint32_t));
  const Buffer pageStates = storage(
      "brick.lod.pageStates", sizeof(SparseVoxelTerrainPageStateGPU));
  const Buffer requestGenerations = storage(
      "brick.lod.requestGenerations", sizeof(uint32_t));
  const Buffer requestDesiredLODs = storage(
      "brick.lod.requestDesiredLODs", sizeof(uint32_t));
  const Buffer lodGroups = storage(
      "brick.lod.lodGroups", sizeof(SparseVoxelBrickLODGroupGPU));
  const Buffer nodeLODGroupIndices = storage(
      "brick.lod.nodeLODGroupIndices", 2u * sizeof(uint32_t));
  const Buffer representativeLODGroupIndices = storage(
      "brick.lod.representativeLODGroupIndices", 2u * sizeof(uint32_t));
  const Buffer readback = graph.createBuffer({
      .name = "brick.lod.readback",
      .size = sizeof(BrickLODReadback),
      .usage = BufferUsage::BufferUsage_Pull |
          BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(nodeBuffer, 0u, sizeof(nodes), nodes.data());
  graph.bufferWrite(history, 0u, sizeof(initialHistory),
      const_cast<uint32_t *>(initialHistory.data()));
  graph.bufferWrite(pageStates, 0u, sizeof(pageState),
      const_cast<SparseVoxelTerrainPageStateGPU *>(&pageState));
  if (useRepresentativeLODGroup)
  {
    SparseVoxelBrickLODGroupGPU group{};
    group.firstNode = 0u;
    group.nodeCount = 1u;
    group.level = 0u;
    group.sharedError = 1.0f;
    group.minimumError = 1.0f;
    group.maximumError = 1.0f;
    group.representativeNode = 0u;
    const std::array<uint32_t, 2> nodeGroups{
        kSparseVoxelBrickLODGroupInvalidIndex, 0u};
    const std::array<uint32_t, 2> representativeGroups{
        0u, kSparseVoxelBrickLODGroupInvalidIndex};
    graph.bufferWrite(lodGroups, 0u, sizeof(group), &group);
    graph.bufferWrite(
        nodeLODGroupIndices, 0u, sizeof(nodeGroups),
        const_cast<uint32_t *>(nodeGroups.data()));
    graph.bufferWrite(
        representativeLODGroupIndices, 0u, sizeof(representativeGroups),
        const_cast<uint32_t *>(representativeGroups.data()));
  }

  const Texture hzb = graph.createTexture({
      .name = "brick.lod.hzb",
      .format = Format::Format_R32Float,
      .width = 1u,
      .height = 1u,
      .usage = ImageUsage::ImageUsage_Sampled,
      .mipLevels = 1u,
  });
  const svrRendering::SparseVoxelBrickCullPass::Buffers buffers{
      nodeBuffer, visible, counters, drawArgs, history, microCounters,
      requestBits, requestPriorities, pageStates, requestGenerations,
      requestDesiredLODs, lodGroups, nodeLODGroupIndices,
      representativeLODGroupIndices};
  SparseVoxelBrickCullUniformsGPU uniforms{};
  uniforms.cameraWorldPosition = {0.0f, 0.0f, 0.0f, 1.0f};
  uniforms.focalPrincipal = {focalPixels, focalPixels, 0.0f, 0.0f};
  uniforms.viewport = {64u, 64u, 1u, 1u};
  uniforms.rootCount = 1u;
  uniforms.nodeCount = 2u;
  uniforms.visibleCapacity = 2u;
  uniforms.lodGroupCount = useRepresentativeLODGroup ? 1u : 0u;
  uniforms.lodPixelThreshold = 10.0f;
  uniforms.hzbMipCount = 1u;
  uniforms.terrainPageRequestCapacity = feedbackEnabled ? 1u : 0u;
  uniforms.terrainSplitErrorPixels = 4.0f;
  uniforms.terrainMergeErrorPixels = 2.5f;
  uniforms.terrainOutputPixelScale = 1.0f;
  uniforms.terrainMinimumResidentFrames = 4u;
  uniforms.terrainFrameIndex = feedbackFrame;
  uniforms.terrainFeedbackEnabled = feedbackEnabled ? 1u : 0u;
  uniforms.terrainDistanceErrorGrowthPerKilometre =
      distanceErrorGrowthPerKilometre;
  uniforms.terrainDistanceErrorExponent = distanceErrorExponent;

  uint32_t passIndex = 0u;
  const auto addPass = [&](const char *name,
                           const svrRendering::SparseVoxelBrickCullPass::Kernel kernel,
                           const uint32_t phase)
  {
    SparseVoxelBrickCullUniformsGPU passUniforms = uniforms;
    passUniforms.phase = phase;
    graph.registerPass<svrRendering::SparseVoxelBrickCullPass>(
        name, passIndex++, kernel, buffers,
        svrRendering::SparseVoxelBrickCullPass::Settings{passUniforms}, hzb);
  };
  addPass("brick.lod.reset", svrRendering::SparseVoxelBrickCullPass::Kernel::Reset, 0u);
  addPass("brick.lod.phase0", svrRendering::SparseVoxelBrickCullPass::Kernel::Cull, 0u);
  addPass("brick.lod.phase1", svrRendering::SparseVoxelBrickCullPass::Kernel::Cull, 1u);
  addPass("brick.lod.commit", svrRendering::SparseVoxelBrickCullPass::Kernel::CommitLODHistory, 1u);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.lod.copyCount", passIndex++, counters, 0u, sizeof(uint32_t),
      readback, offsetof(BrickLODReadback, visibleCount), sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.lod.copyVisible", passIndex++, visible, 0u, sizeof(uint32_t),
      readback, offsetof(BrickLODReadback, visibleBrick), sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.lod.copyHistory", passIndex++, history, 0u, sizeof(initialHistory),
      readback, offsetof(BrickLODReadback, history), sizeof(initialHistory));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.lod.copyRequestBits", passIndex++, requestBits, 0u,
      sizeof(uint32_t), readback, offsetof(BrickLODReadback, requestBits),
      sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.lod.copyRequestPriority", passIndex++, requestPriorities, 0u,
      sizeof(uint32_t), readback,
      offsetof(BrickLODReadback, requestPriority), sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.lod.copyRequestGeneration", passIndex++, requestGenerations, 0u,
      sizeof(uint32_t), readback,
      offsetof(BrickLODReadback, requestGeneration), sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "brick.lod.copyRequestDesiredLOD", passIndex++, requestDesiredLODs, 0u,
      sizeof(uint32_t), readback,
      offsetof(BrickLODReadback, requestDesiredLOD), sizeof(uint32_t));
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  BrickLODReadback result{};
  graph.bufferRead(readback, 0u, sizeof(result),
      [&](const void *data) { std::memcpy(&result, data, sizeof(result)); });
  return result;
}

void testBrickLODTemporalHysteresis(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t initialized =
      kSparseVoxelBrickHistoryLODInitialized;
  constexpr uint32_t selected =
      kSparseVoxelBrickHistoryPreviousLOD;

  // Parent footprint is 10.5 px: above the nominal 10 px split but inside
  // the 11.5 px refinement edge, so a previously selected parent must stay.
  const BrickLODReadback refining = runBrickLODCut(
      rhi, 52.5f, {initialized | selected, initialized});
  assert(refining.visibleCount == 1u);
  assert(refining.visibleBrick == 0u);
  assert((refining.history[0] & selected) != 0u);
  assert((refining.history[1] & selected) == 0u);
  assert((refining.history[0] & kSparseVoxelBrickHistoryPendingLOD) == 0u);

  // Parent footprint is 9.5 px: below the nominal split but above the 8.5 px
  // coarsening edge, so the previously selected child must stay.
  const BrickLODReadback coarsening = runBrickLODCut(
      rhi, 47.5f, {initialized, initialized | selected});
  assert(coarsening.visibleCount == 1u);
  assert(coarsening.visibleBrick == 1u);
  assert((coarsening.history[0] & selected) == 0u);
  assert((coarsening.history[1] & selected) != 0u);

  const BrickLODReadback crossedRefine = runBrickLODCut(
      rhi, 65.0f, {initialized | selected, initialized});
  assert(crossedRefine.visibleCount == 1u);
  assert(crossedRefine.visibleBrick == 1u);
  assert((crossedRefine.history[0] & selected) == 0u);
  assert((crossedRefine.history[1] & selected) != 0u);

  const BrickLODReadback crossedCoarsen = runBrickLODCut(
      rhi, 35.0f, {initialized, initialized | selected});
  assert(crossedCoarsen.visibleCount == 1u);
  assert(crossedCoarsen.visibleBrick == 0u);
  assert((crossedCoarsen.history[0] & selected) != 0u);
  assert((crossedCoarsen.history[1] & selected) == 0u);

  // Fresh or rewritten slots have no committed cut and use the nominal
  // threshold for their first frame instead of being biased finer.
  const BrickLODReadback firstFrame = runBrickLODCut(rhi, 47.5f, {1u, 1u});
  assert(firstFrame.visibleCount == 1u);
  assert(firstFrame.visibleBrick == 0u);
  assert((firstFrame.history[0] & (initialized | selected)) ==
      (initialized | selected));
}

void testBrickLODGroupSelectsRepresentativeAsOnePatch(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t initialized =
      kSparseVoxelBrickHistoryLODInitialized;
  constexpr uint32_t selected =
      kSparseVoxelBrickHistoryPreviousLOD;

  // Without group metadata, this camera projects the parent's voxel/error
  // above the hysteresis edge, so the child owns the cut.
  const BrickLODReadback legacy = runBrickLODCut(
      rhi, 65.0f, {initialized | selected, initialized});
  assert(legacy.visibleCount == 1u);
  assert(legacy.visibleBrick == 1u);

  // With a parent-backed group, the representative carries the shared
  // simplification error. That error is below the threshold, so the parent wins
  // and the child is suppressed by the group decision. This is the virtual
  // geometry cluster-group rule in miniature: parent patch or all children,
  // never an independent child sliver beside its own simplified parent.
  const BrickLODReadback grouped = runBrickLODCut(
      rhi, 65.0f, {initialized | selected, initialized}, {}, 0u, false,
      0.0f, 1.0f, true);
  assert(grouped.visibleCount == 1u);
  assert(grouped.visibleBrick == 0u);
  assert((grouped.history[0] & selected) != 0u);
  assert((grouped.history[1] & selected) == 0u);
}

void testGeneratedWorldDesiredLODFeedback(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t initialized =
      kSparseVoxelBrickHistoryLODInitialized;
  const SparseVoxelTerrainPageStateGPU resident{
    37u, 2u, 10u,
    kSparseVoxelTerrainPageStateProgressiveGenerated};

  const BrickLODReadback requested = runBrickLODCut(
      rhi, 60.0f, {initialized, initialized}, resident, 14u, true);
  assert(requested.requestBits == 1u);
  assert(requested.requestPriority != 0u);
  assert(requested.requestGeneration == 37u);
  assert(requested.requestDesiredLOD == 1u);

  // A newly published chunk cannot immediately request another level.
  const BrickLODReadback tooYoung = runBrickLODCut(
      rhi, 60.0f, {initialized, initialized}, resident, 13u, true);
  assert(tooYoung.requestBits == 0u);
  // Visible terminals always publish their generation-locked maximum error;
  // the residency-age gate suppresses only the refine bit/desired LOD. CPU
  // coarsening policy needs these observations during the dead band.
  assert(tooYoung.requestPriority != 0u);
  assert(tooYoung.requestGeneration == 37u);
  assert(tooYoung.requestDesiredLOD == 0xffffffffu);

  // The same distance-adjusted metric drives the hierarchy cut, page split,
  // merge observation, and request priority. At 10 m a growth of 10/km makes
  // the multiplier 1.1, so this leaf reports 6 / 1.1 adjusted pixels.
  const BrickLODReadback distanceAdjusted = runBrickLODCut(
      rhi, 60.0f, {initialized, initialized}, resident, 14u, true,
      10.0f, 1.0f);
  const uint32_t adjustedPriority = static_cast<uint32_t>(
      (6.0f / 1.1f) * 65536.0f);
  assert(distanceAdjusted.requestBits == 1u);
  assert(distanceAdjusted.requestPriority >= adjustedPriority - 32u);
  assert(distanceAdjusted.requestPriority <= adjustedPriority + 32u);
  assert(distanceAdjusted.requestGeneration == 37u);

  // At growth 100/km the parent itself satisfies the ordinary hierarchy
  // threshold. Traversal stops there, so its descendant cannot independently
  // request a finer physical page.
  const BrickLODReadback distanceCoarsened = runBrickLODCut(
      rhi, 60.0f, {initialized, initialized}, resident, 14u, true,
      100.0f, 1.0f);
  assert(distanceCoarsened.visibleBrick == 0u);
  assert(distanceCoarsened.requestBits == 0u);
  assert(distanceCoarsened.requestPriority == 0u);
  assert(distanceCoarsened.requestGeneration == 0u);

  // A satisfied LOD-zero chunk and an ordinary non-progressive page never
  // emit through the generated-world feedback path.
  const SparseVoxelTerrainPageStateGPU satisfied{
    37u, 0u, 0u, kSparseVoxelTerrainPageStateProgressiveGenerated};
  assert(runBrickLODCut(
      rhi, 60.0f, {initialized, initialized}, satisfied, 20u, true)
             .requestBits == 0u);
  const SparseVoxelTerrainPageStateGPU ordinary{37u, 2u, 0u, 0u};
  assert(runBrickLODCut(
      rhi, 60.0f, {initialized, initialized}, ordinary, 20u, true)
             .requestBits == 0u);
}

} // namespace

int main()
{
  os::Logger::start();
  using namespace ::rendering::backend;
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 0u, .minimumComputeWorkGroupInvocations = 64u};
  const ::rendering::DeviceFeatures features = ::rendering::DeviceFeatures::DeviceFeatures_Compute;
  vulkan::VulkanRHI rhi(vulkan::Vulkan_1_2, limits, features, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testClearAndPhysicalRewritePass(rhi);
  testLODRequestEmission(rhi);
  testBrickHZBUsesTightFootprintAndConservativeDepth(rhi);
  testBrickLODTemporalHysteresis(rhi);
  testBrickLODGroupSelectsRepresentativeAsOnePatch(rhi);
  testVirtualBrickCutOracleMatchesGPUInterChunkMixedCut(rhi);
  testGeneratedWorldDesiredLODFeedback(rhi);
  os::Logger::shutdown();
  return 0;
}
