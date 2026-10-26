#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64Dynamic.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64DynamicTracePass.hpp"

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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

std::array<float, 16> inverseTranslation(const float x)
{
  return {
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    -x,
    0.0f,
    0.0f,
    1.0f,
  };
}

SparseVoxelTree64DynamicModelSource model()
{
  return {
    7u,
    256u,
    {
      {0.0f, 0.0f, 0.0f},
      {4.0f, 4.0f, 4.0f},
    },
    {
      {
        {1.0f, 1.0f, 1.0f},
        {2.0f, 2.0f, 2.0f},
      },
    },
  };
}

SparseVoxelTree64DynamicInstanceSource translated(const uint32_t id, const float x)
{
  return {
    id,
    7u,
    inverseTranslation(x),
    {
      {x, 0.0f, 0.0f},
      {x + 4.0f, 4.0f, 4.0f},
    },
  };
}

SparseVoxelTree64Ray ray(const std::array<double, 3> origin, const std::array<double, 3> direction)
{
  return {origin, direction, 64.0};
}

SparseVoxelOccupancyDDARayGPU gpuRay(const SparseVoxelTree64Ray &value)
{
  return {
    {
      static_cast<float>(value.origin[0]),
      static_cast<float>(value.origin[1]),
      static_cast<float>(value.origin[2]),
      static_cast<float>(value.maximumDistance),
    },
    {
      static_cast<float>(value.direction[0]),
      static_cast<float>(value.direction[1]),
      static_cast<float>(value.direction[2]),
      0.0f,
    },
  };
}

struct Box
{
  std::array<double, 3> minimum{};
  std::array<double, 3> maximum{};
};

double intersect(const SparseVoxelTree64Ray &value, const Box &box)
{
  double entry = 0.0;
  double exit = value.maximumDistance;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (value.direction[axis] == 0.0)
    {
      if (value.origin[axis] < box.minimum[axis] || value.origin[axis] >= box.maximum[axis])
        return std::numeric_limits<double>::infinity();
      continue;
    }
    const double first = (box.minimum[axis] - value.origin[axis]) / value.direction[axis];
    const double last = (box.maximum[axis] - value.origin[axis]) / value.direction[axis];
    entry = std::max(entry, std::min(first, last));
    exit = std::min(exit, std::max(first, last));
    if (!(exit > entry))
      return std::numeric_limits<double>::infinity();
  }
  return entry;
}

void testDynamicTrace(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const svrRendering::SparseVoxelTree64DynamicTracePass::Traversal
        traversal)
{
  constexpr uint32_t side = 16u;
  SparseVoxelTree64StateGPU state{};
  state.logicalChunkResolution = {2u, 1u, 1u};
  state.chunkSide = side;
  state.originWorldChunk = {0, 0, 0};
  state.valid = 1u;
  state.contentGeneration = 19u;
  state.originCell = {0, 0, 0};
  state.logicalCellResolution = {32u, 16u, 16u};
  state.cellSize = {1.0f, 1.0f, 1.0f};
  state.worldExtent = {32.0f, 16.0f, 16.0f};

  const uint32_t slotCapacity = sparseVoxelTree64MaximumNodeCount(side);
  std::vector<SparseVoxelTree64NodeGPU> staticNodes(2u * slotCapacity);
  std::vector<uint8_t> cells(uint64_t{side} * side * side, 0u);
  cells[12u + side * (1u + side * 1u)] = 1u;
  SparseVoxelTree64Chunk staticTree;
  std::string error;
  assert(buildSparseVoxelTree64(side, cells, staticTree, &error));
  std::copy(staticTree.nodes.begin(), staticTree.nodes.end(), staticNodes.begin());
  std::vector<SparseVoxelTree64ChunkGPU> directory{
    {
      {0, 0, 0},
      0u,
      0u,
      static_cast<uint32_t>(staticTree.nodes.size()),
      state.contentGeneration,
      kSparseVoxelTree64ChunkValid | kSparseVoxelTree64ChunkOccupied,
    },
    {
      {1, 0, 0},
      1u,
      0u,
      0u,
      state.contentGeneration,
      kSparseVoxelTree64ChunkValid,
    },
  };

  auto reflected = translated(2u, 0.0f);
  reflected.worldToModel = {
    -0.5f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    2.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f / 3.0f,
    0.0f,
    5.0f,
    0.0f,
    0.0f,
    1.0f,
  };
  reflected.worldBounds = {
    {2.0f, 0.0f, 0.0f},
    {10.0f, 2.0f, 12.0f},
  };
  auto terrainReceiver = translated(0u, 2.0f);
  terrainReceiver.flags = kSparseVoxelTree64DynamicSkipReceiverCell;
  // Model one reduced Tree64 leaf containing four 25 cm raster receivers.
  // The escape must use the raster cell, not discard the complete tree leaf.
  terrainReceiver.receiverCellSizeMetres = 0.25f;
  terrainReceiver.terrainSlot = 0u;
  terrainReceiver.terrainGeneration = 7u;
  terrainReceiver.regionMappingId = 0x2468ace0u;
  const std::vector dynamicInstances{
    terrainReceiver,
    translated(1u, 20.0f),
    reflected,
  };
  SparseVoxelTree64DynamicSnapshot dynamic;
  assert(sparseVoxelTree64BuildDynamicSnapshot(
      state,
      {
        1u,
        3u,
        2u,
        8u,
        16u,
        8u,
      },
      5u,
      31u,
      {model()},
      dynamicInstances,
      dynamic,
      &error));
  assert(!dynamic.conservativeFallback);
  assert(dynamic.models.size() == 1u);
  auto replacementModelSource = model();
  replacementModelSource.localLeafBounds[0] = {
    {2.0f, 1.0f, 1.0f},
    {3.0f, 2.0f, 2.0f},
  };
  SparseVoxelTree64DynamicSnapshot replacementDynamic;
  assert(sparseVoxelTree64BuildDynamicSnapshot(
      state,
      {
        1u,
        3u,
        2u,
        8u,
        16u,
        8u,
      },
      5u,
      32u,
      {replacementModelSource},
      dynamicInstances,
      replacementDynamic,
      &error));
  assert(!replacementDynamic.conservativeFallback);
  assert(replacementDynamic.models.size() == 1u);
  // Stable terrain instances occupy physical slots.  Keep one deliberately
  // absent slot in the traced capacity: it must behave as empty space, not
  // invalidate every global any-hit shadow ray while the async window fills.
  auto gpuInstances = dynamic.instances;
  gpuInstances.resize(dynamic.instances.size() + 1u);

  std::vector rays{
    ray({0.0, 1.5, 1.5}, {1.0, 0.0, 0.0}),
    ray({10.0, 1.5, 1.5}, {1.0, 0.0, 0.0}),
    ray({15.0, 1.5, 1.5}, {1.0, 0.0, 0.0}),
    ray({11.0, 0.75, 4.5}, {-1.0, 0.0, 0.0}),
    ray({0.0, 8.0, 8.0}, {1.0, 0.0, 0.0}),
    ray({-2.0, 1.5, 1.5}, {1.0, 0.0, 0.0}),
    // Starts in the last occupied leaf of the flagged terrain instance. The
    // containing receiver cell must be skipped, after which the static cell at
    // x=12 remains the nearest real occluder.
    ray({3.99, 1.5, 1.5}, {1.0, 0.0, 0.0}),
    // Starts halfway through the same occupied Tree64 leaf, exactly on a
    // 25 cm raster boundary. Exiting one raster receiver reaches x=3.75 and
    // must see the remaining conservative leaf immediately; skipping the
    // complete one-metre leaf would incorrectly report the distant x=12 box.
    ray({3.50, 1.5, 1.5}, {1.0, 0.0, 0.0}),
    // Same canonical receiver, but compaction advanced the traversal origin
    // across the 25 cm receiver-cell boundary. direction.w carries that
    // advance. The remaining conservative leaf begins at the advanced origin;
    // it must be hit immediately rather than skipped as a different receiver.
    ray({3.50, 1.5, 1.5}, {1.0, 0.0, 0.0}),
  };
  std::mt19937 random(0x7164u);
  std::uniform_real_distribution<double> lateral(0.0, 15.99);
  for (uint32_t index = 0u; index < 128u; ++index)
    rays.push_back((index & 1u) == 0u ? ray({-2.0, lateral(random), lateral(random)}, {1.0, 0.0, 0.0}) : ray({34.0, lateral(random), lateral(random)}, {-1.0, 0.0, 0.0}));
  const std::vector<Box> sourceOracle{
    {{12.0, 1.0, 1.0}, {13.0, 2.0, 2.0}},
    {{3.0, 1.0, 1.0}, {4.0, 2.0, 2.0}},
    {{21.0, 1.0, 1.0}, {22.0, 2.0, 2.0}},
    {{6.0, 0.5, 3.0}, {8.0, 1.0, 6.0}},
  };
  std::vector<double> expectedDistance;
  std::vector<double> replacementExpectedDistance;
  std::vector<SparseVoxelOccupancyDDARayGPU> gpuRays;
  for (size_t rayIndex = 0u; rayIndex < rays.size(); ++rayIndex)
  {
    const auto &value = rays[rayIndex];
    double nearest = std::numeric_limits<double>::infinity();
    for (size_t boxIndex = 0u; boxIndex < sourceOracle.size(); ++boxIndex)
    {
      // The terrain receiver box is the representation whose containing leaf
      // is deliberately ignored for the dedicated ray above.
      if (rayIndex == 6u && boxIndex == 1u)
        continue;
      nearest = std::min(nearest, intersect(value, sourceOracle[boxIndex]));
    }
    if (rayIndex == 7u)
      nearest = 0.25;
    if (rayIndex == 8u)
      nearest = 0.0;
    expectedDistance.push_back(nearest);
    double replacementNearest = std::numeric_limits<double>::infinity();
    const std::array<Box, 4u> replacementOracle{
      sourceOracle[0],
      Box{{4.0, 1.0, 1.0}, {5.0, 2.0, 2.0}},
      sourceOracle[2],
      sourceOracle[3],
    };
    for (const auto &box : replacementOracle)
      replacementNearest = std::min(
          replacementNearest, intersect(value, box));
    replacementExpectedDistance.push_back(replacementNearest);
    gpuRays.push_back(gpuRay(value));
  }
  gpuRays[8].originMaximumDistance[0] += 0.30f;
  gpuRays[8].direction[3] = 0.30f;

  RenderGraph graph(&rhi);
  const auto storage = [&](const std::string &name, const uint64_t size)
  {
    return graph.createBuffer({
      .name = name,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
    });
  };
  const Buffer stateBuffer = storage("Tree64Dynamic.State", sizeof(state));
  const Buffer chunksBuffer = storage("Tree64Dynamic.Chunks", directory.size() * sizeof(directory[0]));
  const Buffer staticNodesBuffer = storage("Tree64Dynamic.StaticNodes", staticNodes.size() * sizeof(staticNodes[0]));
  const Buffer raysBuffer = storage("Tree64Dynamic.Rays", gpuRays.size() * sizeof(gpuRays[0]));
  const Buffer membershipsBuffer = storage("Tree64Dynamic.Memberships", dynamic.memberships.size() * sizeof(dynamic.memberships[0]));
  const Buffer referencesBuffer = storage("Tree64Dynamic.References", dynamic.references.size() * sizeof(dynamic.references[0]));
  const Buffer instancesBuffer = storage("Tree64Dynamic.Instances", gpuInstances.size() * sizeof(gpuInstances[0]));
  const Buffer modelsBuffer = storage("Tree64Dynamic.Models", dynamic.models.size() * sizeof(dynamic.models[0]));
  const Buffer dynamicNodesBuffer = storage("Tree64Dynamic.DynamicNodes", dynamic.nodes.size() * sizeof(dynamic.nodes[0]));
  const Buffer terrainPageStatesBuffer = storage(
      "Tree64Dynamic.TerrainPageStates",
      sizeof(SparseVoxelTerrainPageStateGPU));
  constexpr uint32_t terrainPhysicalPageCount = 2u;
  constexpr uint32_t terrainNodesPerPage = 16u;
  constexpr uint32_t terrainMaterialCellsPerPage = 1u;
  std::array<SparseVoxelTerrainTree64DescriptorGPU, 1>
      terrainDescriptors{};
  std::array<uint32_t, terrainPhysicalPageCount> nodePageGenerations{};
  std::array<uint32_t, terrainPhysicalPageCount>
      materialPageGenerations{};
  std::vector<SparseVoxelTree64NodeGPU> terrainPageNodes(
      terrainPhysicalPageCount * terrainNodesPerPage);
  std::array<SparseVoxelTree64MaterialCellGPU,
             terrainPhysicalPageCount * terrainMaterialCellsPerPage>
      terrainPageMaterials{};
  constexpr uint32_t physicalPageSlot = 1u;
  constexpr uint32_t physicalPageGeneration = 11u;
  const auto &terrainModel = dynamic.models[0];
  uint32_t terrainCellSizeBits = 0u;
  std::memcpy(
      &terrainCellSizeBits, &terrainModel.localCellSize[0],
      sizeof(terrainCellSizeBits));
  assert(terrainModel.nodeCount <= terrainNodesPerPage);
  std::copy_n(
      dynamic.nodes.begin() + terrainModel.nodeOffset,
      terrainModel.nodeCount,
      terrainPageNodes.begin() +
          physicalPageSlot * terrainNodesPerPage);
  terrainDescriptors[0] = {
    terrainReceiver.terrainSlot,
    terrainReceiver.terrainGeneration,
    0u,
    kSparseVoxelTree64TerrainPageValid,
    physicalPageSlot,
    physicalPageGeneration,
    terrainModel.rootNodeOffset,
    terrainModel.nodeCount,
    physicalPageSlot,
    physicalPageGeneration,
    0u,
    0u,
    31u,
    0u,
    terrainModel.side,
    terrainCellSizeBits,
  };
  nodePageGenerations[physicalPageSlot] = physicalPageGeneration;
  materialPageGenerations[physicalPageSlot] = physicalPageGeneration;
  const Buffer terrainDescriptorsBuffer = storage(
      "Tree64Dynamic.TerrainDescriptors", sizeof(terrainDescriptors));
  const Buffer terrainNodePageGenerationsBuffer = storage(
      "Tree64Dynamic.TerrainNodePageGenerations",
      sizeof(nodePageGenerations));
  const Buffer terrainPageNodesBuffer = storage(
      "Tree64Dynamic.TerrainPageNodes",
      terrainPageNodes.size() * sizeof(terrainPageNodes[0]));
  const Buffer terrainMaterialPageGenerationsBuffer = storage(
      "Tree64Dynamic.TerrainMaterialPageGenerations",
      sizeof(materialPageGenerations));
  const Buffer terrainPageMaterialsBuffer = storage(
      "Tree64Dynamic.TerrainPageMaterials",
      sizeof(terrainPageMaterials));
  const Buffer resultsBuffer = graph.createBuffer({
    .name = "Tree64Dynamic.Results",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer readback = graph.createBuffer({
    .name = "Tree64Dynamic.Readback",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(stateBuffer, 0u, sizeof(state), &state);
  graph.bufferWrite(chunksBuffer, 0u, directory.size() * sizeof(directory[0]), directory.data());
  graph.bufferWrite(staticNodesBuffer, 0u, staticNodes.size() * sizeof(staticNodes[0]), staticNodes.data());
  graph.bufferWrite(raysBuffer, 0u, gpuRays.size() * sizeof(gpuRays[0]), gpuRays.data());
  graph.bufferWrite(membershipsBuffer, 0u, dynamic.memberships.size() * sizeof(dynamic.memberships[0]), dynamic.memberships.data());
  graph.bufferWrite(referencesBuffer, 0u, dynamic.references.size() * sizeof(dynamic.references[0]), dynamic.references.data());
  graph.bufferWrite(instancesBuffer, 0u, gpuInstances.size() * sizeof(gpuInstances[0]), gpuInstances.data());
  graph.bufferWrite(modelsBuffer, 0u, dynamic.models.size() * sizeof(dynamic.models[0]), dynamic.models.data());
  graph.bufferWrite(dynamicNodesBuffer, 0u, dynamic.nodes.size() * sizeof(dynamic.nodes[0]), dynamic.nodes.data());
  SparseVoxelTerrainPageStateGPU terrainPageState{};
  terrainPageState.generation = terrainReceiver.terrainGeneration;
  graph.bufferWrite(
      terrainPageStatesBuffer, 0u, sizeof(terrainPageState),
      &terrainPageState);
  graph.bufferWrite(
      terrainDescriptorsBuffer, 0u, sizeof(terrainDescriptors),
      terrainDescriptors.data());
  graph.bufferWrite(
      terrainNodePageGenerationsBuffer, 0u,
      sizeof(nodePageGenerations), nodePageGenerations.data());
  graph.bufferWrite(
      terrainPageNodesBuffer, 0u,
      terrainPageNodes.size() * sizeof(terrainPageNodes[0]),
      terrainPageNodes.data());
  graph.bufferWrite(
      terrainMaterialPageGenerationsBuffer, 0u,
      sizeof(materialPageGenerations), materialPageGenerations.data());
  graph.bufferWrite(
      terrainPageMaterialsBuffer, 0u, sizeof(terrainPageMaterials),
      terrainPageMaterials.data());

  uint32_t passIndex = 0u;
  svrRendering::SparseVoxelTree64DynamicTracePass::Buffers traceBuffers{
    stateBuffer,
    chunksBuffer,
    staticNodesBuffer,
    raysBuffer,
    resultsBuffer,
    membershipsBuffer,
    referencesBuffer,
    instancesBuffer,
    modelsBuffer,
    dynamicNodesBuffer,
  };
  traceBuffers.terrainPageStates = terrainPageStatesBuffer;
  traceBuffers.terrainDescriptors = terrainDescriptorsBuffer;
  traceBuffers.terrainNodePageGenerations =
      terrainNodePageGenerationsBuffer;
  traceBuffers.terrainPageNodes = terrainPageNodesBuffer;
  traceBuffers.terrainMaterialPageGenerations =
      terrainMaterialPageGenerationsBuffer;
  traceBuffers.terrainPageMaterialCells = terrainPageMaterialsBuffer;
  svrRendering::SparseVoxelTree64DynamicTracePass::Capacities
      traceCapacities{
        2u,
        static_cast<uint32_t>(staticNodes.size()),
        slotCapacity,
        static_cast<uint32_t>(gpuRays.size()),
        2u,
        8u,
        static_cast<uint32_t>(gpuInstances.size()),
        1u,
        16u,
        8u,
      };
  traceCapacities.terrainPageStateCount = 1u;
  traceCapacities.terrainDescriptorCount = 1u;
  traceCapacities.terrainPhysicalPageCount = terrainPhysicalPageCount;
  traceCapacities.terrainNodesPerPage = terrainNodesPerPage;
  traceCapacities.terrainMaterialCellsPerPage =
      terrainMaterialCellsPerPage;
  auto *trace = graph.registerPass<svrRendering::SparseVoxelTree64DynamicTracePass>(
      "Tree64Dynamic.Trace",
      passIndex++,
      traceBuffers,
      traceCapacities,
      traversal);
  assert(trace->update(
      static_cast<uint32_t>(gpuRays.size()),
      4096u,
      static_cast<uint32_t>(dynamic.models.size()),
      static_cast<uint32_t>(gpuInstances.size()),
      static_cast<uint32_t>(dynamic.references.size()),
      static_cast<uint32_t>(dynamic.nodes.size()),
      dynamic.contentGeneration));
  auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "Tree64Dynamic.Copy", passIndex++, resultsBuffer, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU), readback, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
  graph.compile();

  const auto execute = [&]()
  {
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    std::vector<SparseVoxelOccupancyDDAResultGPU> result(rays.size());
    graph.bufferRead(
        readback,
        0u,
        result.size() * sizeof(result[0]),
        [&](const void *data)
        {
          std::memcpy(result.data(), data, result.size() * sizeof(result[0]));
        });
    return result;
  };

  const auto actual = execute();
  for (uint32_t index = 0u; index < rays.size(); ++index)
  {
    const bool expectedHit = std::isfinite(expectedDistance[index]);
    if (actual[index].status != static_cast<uint32_t>(
            expectedHit ? SparseVoxelTree64TraceStatus::Hit
                        : SparseVoxelTree64TraceStatus::Miss))
      std::cerr << "Tree64 descriptor parity mismatch traversal="
                << static_cast<uint32_t>(traversal) << " ray=" << index
                << " expectedHit=" << expectedHit
                << " status=" << actual[index].status
                << " distance=" << actual[index].distance
                << " queries=" << actual[index].queryCount << '\n';
    assert(actual[index].status == static_cast<uint32_t>(expectedHit ? SparseVoxelTree64TraceStatus::Hit : SparseVoxelTree64TraceStatus::Miss));
    if (expectedHit)
    {
      if (std::abs(actual[index].distance - expectedDistance[index]) >=
          2.0e-4)
        std::cerr << "Tree64 descriptor distance mismatch traversal="
                  << static_cast<uint32_t>(traversal) << " ray=" << index
                  << " expected=" << expectedDistance[index]
                  << " actual=" << actual[index].distance << '\n';
      assert(std::abs(actual[index].distance - expectedDistance[index]) <
          2.0e-4);
    }
  }

  // Internal refinement/coarsening keeps the stable terrain owner and swaps
  // only its descriptor identity. Until Phase 5 performs the swap atomically,
  // exercise both directions explicitly while the mirrored payload is equal.
  for (const uint32_t internalLOD : {2u, 0u})
  {
    terrainPageState.currentLOD = internalLOD;
    terrainDescriptors[0].internalLOD = internalLOD;
    graph.bufferWrite(
        terrainPageStatesBuffer, 0u, sizeof(terrainPageState),
        &terrainPageState);
    graph.bufferWrite(
        terrainDescriptorsBuffer, 0u, sizeof(terrainDescriptors),
        terrainDescriptors.data());
    const auto rewritten = execute();
    for (uint32_t index = 0u; index < rays.size(); ++index)
    {
      assert(rewritten[index].status == actual[index].status);
      if (rewritten[index].status == static_cast<uint32_t>(
              SparseVoxelTree64TraceStatus::Hit))
        assert(std::abs(
            rewritten[index].distance - actual[index].distance) < 2.0e-4);
    }
  }

  // Repeatedly install a genuinely different immutable terrain BLAS into the
  // inactive physical page, prove it cannot affect rays before the descriptor
  // rewrite, publish it, and then corrupt the retired page. This is the full
  // installed-geometry contract the shadow path depends on: the descriptor,
  // page generation and physical node payload must select one coherent raster
  // geometry over time, even after every physical slot has been recycled many
  // times.
  const auto &replacementModel = replacementDynamic.models[0];
  assert(replacementModel.side == terrainModel.side);
  assert(replacementModel.nodeCount <= terrainNodesPerPage);
  const auto assertInstalledParity = [&] (
      const std::vector<SparseVoxelOccupancyDDAResultGPU> &result,
      const std::vector<double> &expected,
      const uint32_t rewrite,
      const char *stage)
  {
    for (uint32_t index = 0u; index < rays.size(); ++index)
    {
      // Rays 6-7 are the dedicated receiver-cell scale cases. Their expected
      // exclusion is intentionally representation-dependent and is already
      // covered above; the rewrite sequence compares ordinary rays.
      if (index == 6u || index == 7u || index == 8u)
        continue;
      const bool expectedHit = std::isfinite(expected[index]);
      const uint32_t expectedStatus = static_cast<uint32_t>(
          expectedHit ? SparseVoxelTree64TraceStatus::Hit
                      : SparseVoxelTree64TraceStatus::Miss);
      if (result[index].status != expectedStatus ||
          (expectedHit && std::abs(
              result[index].distance - expected[index]) >= 2.0e-4))
        std::cerr << "Tree64 installed rewrite parity mismatch traversal="
                  << static_cast<uint32_t>(traversal)
                  << " rewrite=" << rewrite << " stage=" << stage
                  << " ray=" << index << " expectedStatus="
                  << expectedStatus << " actualStatus="
                  << result[index].status << " expectedDistance="
                  << expected[index] << " actualDistance="
                  << result[index].distance << '\n';
      assert(result[index].status == expectedStatus);
      if (expectedHit)
        assert(std::abs(result[index].distance - expected[index]) < 2.0e-4);
    }
  };
  constexpr uint32_t rewriteCount = 64u;
  bool replacementActive = false;
  uint32_t activePhysicalSlot = physicalPageSlot;
  for (uint32_t rewrite = 0u; rewrite < rewriteCount; ++rewrite)
  {
    const bool installReplacement = !replacementActive;
    const auto &installModel = installReplacement
        ? replacementModel : terrainModel;
    const auto &installNodes = installReplacement
        ? replacementDynamic.nodes : dynamic.nodes;
    const uint32_t inactivePhysicalSlot = activePhysicalSlot ^ 1u;
    const uint32_t installedGeneration = 1000u + rewrite;
    std::fill(
        terrainPageNodes.begin() +
            inactivePhysicalSlot * terrainNodesPerPage,
        terrainPageNodes.begin() +
            (inactivePhysicalSlot + 1u) * terrainNodesPerPage,
        SparseVoxelTree64NodeGPU{});
    std::copy_n(
        installNodes.begin() + installModel.nodeOffset,
        installModel.nodeCount,
        terrainPageNodes.begin() +
            inactivePhysicalSlot * terrainNodesPerPage);
    nodePageGenerations[inactivePhysicalSlot] = installedGeneration;
    materialPageGenerations[inactivePhysicalSlot] = installedGeneration;
    graph.bufferWrite(
        terrainPageNodesBuffer, 0u,
        terrainPageNodes.size() * sizeof(terrainPageNodes[0]),
        terrainPageNodes.data());
    graph.bufferWrite(
        terrainNodePageGenerationsBuffer, 0u,
        sizeof(nodePageGenerations), nodePageGenerations.data());
    graph.bufferWrite(
        terrainMaterialPageGenerationsBuffer, 0u,
        sizeof(materialPageGenerations), materialPageGenerations.data());

    const auto beforePublication = execute();
    assertInstalledParity(
        beforePublication,
        replacementActive ? replacementExpectedDistance : expectedDistance,
        rewrite, "before-publication");

    terrainDescriptors[0].nodePageSlot = inactivePhysicalSlot;
    terrainDescriptors[0].nodePageGeneration = installedGeneration;
    terrainDescriptors[0].rootNodeOffset = installModel.rootNodeOffset;
    terrainDescriptors[0].nodeCount = installModel.nodeCount;
    terrainDescriptors[0].materialPageSlot = inactivePhysicalSlot;
    terrainDescriptors[0].materialPageGeneration = installedGeneration;
    terrainDescriptors[0].geometryRevisionLo = 0x90000000u + rewrite;
    graph.bufferWrite(
        terrainDescriptorsBuffer, 0u, sizeof(terrainDescriptors),
        terrainDescriptors.data());
    replacementActive = installReplacement;
    const auto afterPublication = execute();
    assertInstalledParity(
        afterPublication,
        replacementActive ? replacementExpectedDistance : expectedDistance,
        rewrite, "after-publication");

    const uint32_t retiredPhysicalSlot = activePhysicalSlot;
    std::fill(
        terrainPageNodes.begin() +
            retiredPhysicalSlot * terrainNodesPerPage,
        terrainPageNodes.begin() +
            (retiredPhysicalSlot + 1u) * terrainNodesPerPage,
        SparseVoxelTree64NodeGPU{});
    ++nodePageGenerations[retiredPhysicalSlot];
    graph.bufferWrite(
        terrainPageNodesBuffer, 0u,
        terrainPageNodes.size() * sizeof(terrainPageNodes[0]),
        terrainPageNodes.data());
    graph.bufferWrite(
        terrainNodePageGenerationsBuffer, 0u,
        sizeof(nodePageGenerations), nodePageGenerations.data());
    const auto afterRetiredCorruption = execute();
    assertInstalledParity(
        afterRetiredCorruption,
        replacementActive ? replacementExpectedDistance : expectedDistance,
        rewrite, "after-retired-corruption");
    activePhysicalSlot = inactivePhysicalSlot;
  }

  // Leave the original page identity installed for the stale-owner checks
  // below, which intentionally mutate one invariant at a time.
  terrainPageState.currentLOD = 0u;
  terrainDescriptors[0] = {
    terrainReceiver.terrainSlot,
    terrainReceiver.terrainGeneration,
    0u,
    kSparseVoxelTree64TerrainPageValid,
    physicalPageSlot,
    physicalPageGeneration,
    terrainModel.rootNodeOffset,
    terrainModel.nodeCount,
    physicalPageSlot,
    physicalPageGeneration,
    0u,
    0u,
    31u,
    0u,
    terrainModel.side,
    terrainCellSizeBits,
  };
  std::fill(terrainPageNodes.begin(), terrainPageNodes.end(),
      SparseVoxelTree64NodeGPU{});
  std::copy_n(
      dynamic.nodes.begin() + terrainModel.nodeOffset,
      terrainModel.nodeCount,
      terrainPageNodes.begin() + physicalPageSlot * terrainNodesPerPage);
  nodePageGenerations = {};
  materialPageGenerations = {};
  nodePageGenerations[physicalPageSlot] = physicalPageGeneration;
  materialPageGenerations[physicalPageSlot] = physicalPageGeneration;
  graph.bufferWrite(
      terrainPageStatesBuffer, 0u, sizeof(terrainPageState),
      &terrainPageState);
  graph.bufferWrite(
      terrainDescriptorsBuffer, 0u, sizeof(terrainDescriptors),
      terrainDescriptors.data());
  graph.bufferWrite(
      terrainPageNodesBuffer, 0u,
      terrainPageNodes.size() * sizeof(terrainPageNodes[0]),
      terrainPageNodes.data());
  graph.bufferWrite(
      terrainNodePageGenerationsBuffer, 0u,
      sizeof(nodePageGenerations), nodePageGenerations.data());
  graph.bufferWrite(
      terrainMaterialPageGenerationsBuffer, 0u,
      sizeof(materialPageGenerations), materialPageGenerations.data());

  auto staleMemberships = dynamic.memberships;
  staleMemberships[0].contentGeneration -= 1u;
  graph.bufferWrite(membershipsBuffer, 0u, staleMemberships.size() * sizeof(staleMemberships[0]), staleMemberships.data());
  const auto stale = execute();
  assert(stale[0].status == static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Invalid));
  assert(stale[0].distance == 0.0f);

  graph.bufferWrite(membershipsBuffer, 0u, dynamic.memberships.size() * sizeof(dynamic.memberships[0]), dynamic.memberships.data());
  auto staleReferences = dynamic.references;
  staleReferences[0] = 999u;
  graph.bufferWrite(referencesBuffer, 0u, staleReferences.size() * sizeof(staleReferences[0]), staleReferences.data());
  const auto badReference = execute();
  assert(badReference[0].status == static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Invalid));
  assert(badReference[0].distance == 0.0f);

  graph.bufferWrite(referencesBuffer, 0u, dynamic.references.size() * sizeof(dynamic.references[0]), dynamic.references.data());
  ++terrainPageState.generation;
  graph.bufferWrite(
      terrainPageStatesBuffer, 0u, sizeof(terrainPageState),
      &terrainPageState);
  const auto mismatchedTerrainOwner = execute();
  // A stale stable terrain owner means this physical slot is retired or has
  // moved to a newer cut.  Under the streaming contract it must behave like
  // empty space for lighting instead of invalidating the whole any-hit ray;
  // the global trace should continue to the next valid Tree64 occluder.
  assert(mismatchedTerrainOwner[0].status ==
      static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Hit));
  assert(std::abs(mismatchedTerrainOwner[0].distance - 12.0f) < 2.0e-4);
  terrainPageState.generation = terrainReceiver.terrainGeneration;
  graph.bufferWrite(
      terrainPageStatesBuffer, 0u, sizeof(terrainPageState),
      &terrainPageState);

  ++nodePageGenerations[physicalPageSlot];
  graph.bufferWrite(
      terrainNodePageGenerationsBuffer, 0u,
      sizeof(nodePageGenerations), nodePageGenerations.data());
  const auto stalePhysicalPage = execute();
  assert(stalePhysicalPage[0].status ==
      static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Invalid));
  nodePageGenerations[physicalPageSlot] = physicalPageGeneration;
  graph.bufferWrite(
      terrainNodePageGenerationsBuffer, 0u,
      sizeof(nodePageGenerations), nodePageGenerations.data());

  ++materialPageGenerations[physicalPageSlot];
  graph.bufferWrite(
      terrainMaterialPageGenerationsBuffer, 0u,
      sizeof(materialPageGenerations), materialPageGenerations.data());
  const auto staleMaterialPage = execute();
  assert(staleMaterialPage[0].status ==
      static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Invalid));
  materialPageGenerations[physicalPageSlot] = physicalPageGeneration;
  graph.bufferWrite(
      terrainMaterialPageGenerationsBuffer, 0u,
      sizeof(materialPageGenerations), materialPageGenerations.data());

  terrainDescriptors[0].nodeCount = terrainNodesPerPage + 1u;
  graph.bufferWrite(
      terrainDescriptorsBuffer, 0u, sizeof(terrainDescriptors),
      terrainDescriptors.data());
  const auto overflowingPhysicalPage = execute();
  assert(overflowingPhysicalPage[0].status ==
      static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Invalid));
  terrainDescriptors[0].nodeCount = terrainModel.nodeCount;
  graph.bufferWrite(
      terrainDescriptorsBuffer, 0u, sizeof(terrainDescriptors),
      terrainDescriptors.data());

  terrainDescriptors[0].materialCellCount =
      terrainMaterialCellsPerPage + 1u;
  graph.bufferWrite(
      terrainDescriptorsBuffer, 0u, sizeof(terrainDescriptors),
      terrainDescriptors.data());
  const auto overflowingMaterialPage = execute();
  assert(overflowingMaterialPage[0].status ==
      static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Invalid));
  terrainDescriptors[0].materialCellCount = 0u;
  graph.bufferWrite(
      terrainDescriptorsBuffer, 0u, sizeof(terrainDescriptors),
      terrainDescriptors.data());

  constexpr uint32_t iterations = 32u;
  const auto begin = std::chrono::steady_clock::now();
  for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
    static_cast<void>(execute());
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
  std::cout << "TREE64_T7_GPU traversal="
            << (traversal ==
                        svrRendering::SparseVoxelTree64DynamicTracePass::
                            Traversal::RegionMarcher
                    ? "region"
                    : "mirrored-ancestor")
            << " rays=" << rays.size() << " instances=" << dynamic.instances.size() << " shared_models=" << dynamic.models.size() << " references=" << dynamic.references.size()
            << " iterations=" << iterations << " submit_wait_readback_us=" << elapsed << '\n';

  delete copy;
  delete trace;
  graph.deleteBuffer(readback);
  graph.deleteBuffer(resultsBuffer);
  graph.deleteBuffer(terrainPageMaterialsBuffer);
  graph.deleteBuffer(terrainMaterialPageGenerationsBuffer);
  graph.deleteBuffer(terrainPageNodesBuffer);
  graph.deleteBuffer(terrainNodePageGenerationsBuffer);
  graph.deleteBuffer(terrainDescriptorsBuffer);
  graph.deleteBuffer(terrainPageStatesBuffer);
  graph.deleteBuffer(dynamicNodesBuffer);
  graph.deleteBuffer(modelsBuffer);
  graph.deleteBuffer(instancesBuffer);
  graph.deleteBuffer(referencesBuffer);
  graph.deleteBuffer(membershipsBuffer);
  graph.deleteBuffer(raysBuffer);
  graph.deleteBuffer(staticNodesBuffer);
  graph.deleteBuffer(chunksBuffer);
  graph.deleteBuffer(stateBuffer);
}

} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 256u,
  };
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testDynamicTrace(
      rhi,
      svrRendering::SparseVoxelTree64DynamicTracePass::Traversal::
          RegionMarcher);
  testDynamicTrace(
      rhi,
      svrRendering::SparseVoxelTree64DynamicTracePass::Traversal::
          MirroredAncestor);
  std::cout << "Sparse voxel Tree64 T7 dynamic GPU tests passed\n";
  os::Logger::shutdown();
  return 0;
}
