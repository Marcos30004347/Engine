#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelOccupancyDDA.hpp"
#include "voxel/svraster/SparseVoxelOccupancyVolume.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64Reference.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyDDAPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyMipChain.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64TracePass.hpp"

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
#include <string>
#include <vector>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

constexpr uint32_t kChunkSide = 16u;
constexpr uint32_t kChunkAxis = 2u;
constexpr uint32_t kVolumeSide = kChunkSide * kChunkAxis;
constexpr int32_t kOriginCell = -16;

size_t cellIndex(const uint32_t side, const uint32_t x, const uint32_t y, const uint32_t z)
{
  return static_cast<size_t>(x) + static_cast<size_t>(side) * (static_cast<size_t>(y) + static_cast<size_t>(side) * z);
}

bool fixtureOccupied(const std::vector<uint8_t> &cells, const std::array<int32_t, 3> &cell)
{
  const std::array<int32_t, 3> logical{
    cell[0] - kOriginCell,
    cell[1] - kOriginCell,
    cell[2] - kOriginCell,
  };
  if (logical[0] < 0 || logical[1] < 0 || logical[2] < 0 || logical[0] >= static_cast<int32_t>(kVolumeSide) || logical[1] >= static_cast<int32_t>(kVolumeSide) || logical[2] >= static_cast<int32_t>(kVolumeSide))
    return false;
  return cells[cellIndex(kVolumeSide, static_cast<uint32_t>(logical[0]), static_cast<uint32_t>(logical[1]), static_cast<uint32_t>(logical[2]))] != 0u;
}

struct ReferenceResult
{
  SparseVoxelTree64TraceStatus status = SparseVoxelTree64TraceStatus::Invalid;
  std::array<int32_t, 3> cell{};
  double distance = 0.0;
};

ReferenceResult traceFixture(const std::vector<uint8_t> &cells, const SparseVoxelTree64Ray &ray)
{
  ReferenceResult result{};
  if (ray.maximumDistance < 0.0 || !std::isfinite(ray.maximumDistance))
    return result;
  bool hasDirection = false;
  double entry = 0.0;
  double exit = ray.maximumDistance;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(ray.origin[axis]) || !std::isfinite(ray.direction[axis]))
      return result;
    hasDirection = hasDirection || ray.direction[axis] != 0.0;
    if (ray.direction[axis] == 0.0)
    {
      if (ray.origin[axis] < kOriginCell || ray.origin[axis] >= kOriginCell + static_cast<int32_t>(kVolumeSide))
      {
        result.status = SparseVoxelTree64TraceStatus::Miss;
        return result;
      }
      continue;
    }
    const double first = (static_cast<double>(kOriginCell) - ray.origin[axis]) / ray.direction[axis];
    const double last = (static_cast<double>(kOriginCell + kVolumeSide) - ray.origin[axis]) / ray.direction[axis];
    entry = std::max(entry, std::min(first, last));
    exit = std::min(exit, std::max(first, last));
  }
  if (!hasDirection)
    return result;
  if (!(exit > entry))
  {
    result.status = SparseVoxelTree64TraceStatus::Miss;
    return result;
  }

  const double sample = std::nextafter(entry, exit);
  std::array<int32_t, 3> cell{};
  std::array<int32_t, 3> step{};
  std::array<double, 3> nextBoundary{};
  std::array<double, 3> delta{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    double coordinate = ray.origin[axis] + sample * ray.direction[axis];
    if (ray.direction[axis] != 0.0)
      coordinate = std::nextafter(coordinate, ray.direction[axis] > 0.0 ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity());
    cell[axis] = std::clamp(static_cast<int32_t>(std::floor(coordinate)), kOriginCell, kOriginCell + static_cast<int32_t>(kVolumeSide) - 1);
    if (ray.direction[axis] > 0.0)
    {
      step[axis] = 1;
      nextBoundary[axis] = (static_cast<double>(cell[axis] + 1) - ray.origin[axis]) / ray.direction[axis];
      delta[axis] = 1.0 / ray.direction[axis];
    }
    else if (ray.direction[axis] < 0.0)
    {
      step[axis] = -1;
      nextBoundary[axis] = (static_cast<double>(cell[axis]) - ray.origin[axis]) / ray.direction[axis];
      delta[axis] = -1.0 / ray.direction[axis];
    }
    else
    {
      nextBoundary[axis] = std::numeric_limits<double>::infinity();
      delta[axis] = std::numeric_limits<double>::infinity();
    }
    while (nextBoundary[axis] <= entry)
      nextBoundary[axis] += delta[axis];
  }

  double current = entry;
  while (current < exit)
  {
    if (fixtureOccupied(cells, cell))
    {
      result.status = SparseVoxelTree64TraceStatus::Hit;
      result.cell = cell;
      result.distance = current;
      return result;
    }
    const double next = std::min(nextBoundary[0], std::min(nextBoundary[1], nextBoundary[2]));
    if (!(next < exit))
      break;
    current = next;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      if (nextBoundary[axis] == next)
      {
        cell[axis] += step[axis];
        nextBoundary[axis] += delta[axis];
      }
  }
  result.status = SparseVoxelTree64TraceStatus::Miss;
  result.distance = exit;
  return result;
}

SparseVoxelOccupancyDDARayGPU gpuRay(const SparseVoxelTree64Ray &ray)
{
  return {
    {static_cast<float>(ray.origin[0]), static_cast<float>(ray.origin[1]), static_cast<float>(ray.origin[2]), static_cast<float>(ray.maximumDistance)},
    {static_cast<float>(ray.direction[0]), static_cast<float>(ray.direction[1]), static_cast<float>(ray.direction[2]), 0.0f},
  };
}

void testGPUStaticTraversal(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  std::vector<uint8_t> occupied(uint64_t{kVolumeSide} * kVolumeSide * kVolumeSide, 0u);
  const auto setOccupied = [&](const int32_t x, const int32_t y, const int32_t z)
  {
    occupied[cellIndex(kVolumeSide, static_cast<uint32_t>(x - kOriginCell), static_cast<uint32_t>(y - kOriginCell), static_cast<uint32_t>(z - kOriginCell))] = 1u;
  };
  for (int32_t z = -10; z <= 10; ++z)
    for (int32_t y = -10; y <= 10; ++y)
      if (((y + 2 * z) & 3) != 0)
        setOccupied(0, y, z);
  for (int32_t x = -14; x <= 14; ++x)
    setOccupied(x, 12, -7);
  setOccupied(-16, -16, -16);
  setOccupied(-1, -1, -1);
  setOccupied(15, 15, 15);

  std::vector<SparseVoxelAABB> packedSources;
  for (int32_t z = kOriginCell; z < kOriginCell + static_cast<int32_t>(kVolumeSide); ++z)
    for (int32_t y = kOriginCell; y < kOriginCell + static_cast<int32_t>(kVolumeSide); ++y)
      for (int32_t x = kOriginCell; x < kOriginCell + static_cast<int32_t>(kVolumeSide); ++x)
        if (fixtureOccupied(occupied, {x, y, z}))
          packedSources.push_back({
            {static_cast<float>(x) + 0.25f, static_cast<float>(y) + 0.25f, static_cast<float>(z) + 0.25f},
            {static_cast<float>(x) + 0.75f, static_cast<float>(y) + 0.75f, static_cast<float>(z) + 0.75f},
          });
  SparseVoxelOccupancySettings packedSettings{};
  packedSettings.worldExtent = {
    static_cast<float>(kVolumeSide),
    static_cast<float>(kVolumeSide),
    static_cast<float>(kVolumeSide),
  };
  packedSettings.logicalResolution = {kVolumeSide, kVolumeSide, kVolumeSide};
  packedSettings.memoryCeilingBytes = 4u << 20u;
  SparseVoxelOccupancyVolume packedVolume;
  std::string packedError;
  assert(packedVolume.configure(packedSettings, {}, &packedError));
  assert(packedVolume.update({0.0f, 0.0f, 0.0f}, 17u, packedSources, &packedError));
  assert((packedVolume.state().originCell == std::array<int32_t, 3>{kOriginCell, kOriginCell, kOriginCell}));

  SparseVoxelTree64StateGPU state{};
  state.logicalChunkResolution = {kChunkAxis, kChunkAxis, kChunkAxis};
  state.chunkSide = kChunkSide;
  state.originWorldChunk = {-1, -1, -1};
  state.valid = 1u;
  state.wrapOffset = {1u, 0u, 1u};
  state.contentGeneration = 17u;
  state.originCell = {kOriginCell, kOriginCell, kOriginCell};
  state.logicalCellResolution = {kVolumeSide, kVolumeSide, kVolumeSide};
  state.cellSize = {1.0f, 1.0f, 1.0f};
  state.worldExtent = {static_cast<float>(kVolumeSide), static_cast<float>(kVolumeSide), static_cast<float>(kVolumeSide)};

  constexpr uint32_t chunkCount = kChunkAxis * kChunkAxis * kChunkAxis;
  const uint32_t slotCapacity = sparseVoxelTree64MaximumNodeCount(kChunkSide);
  std::vector<SparseVoxelTree64ChunkGPU> directory(chunkCount);
  std::vector<SparseVoxelTree64NodeGPU> nodePool(uint64_t{chunkCount} * slotCapacity);
  uint32_t slot = 0u;
  for (int32_t z = -1; z <= 0; ++z)
    for (int32_t y = -1; y <= 0; ++y)
      for (int32_t x = -1; x <= 0; ++x)
      {
        std::vector<uint8_t> chunkCells(uint64_t{kChunkSide} * kChunkSide * kChunkSide, 0u);
        for (uint32_t localZ = 0u; localZ < kChunkSide; ++localZ)
          for (uint32_t localY = 0u; localY < kChunkSide; ++localY)
            for (uint32_t localX = 0u; localX < kChunkSide; ++localX)
            {
              const std::array<int32_t, 3> globalCell{
                x * static_cast<int32_t>(kChunkSide) + static_cast<int32_t>(localX),
                y * static_cast<int32_t>(kChunkSide) + static_cast<int32_t>(localY),
                z * static_cast<int32_t>(kChunkSide) + static_cast<int32_t>(localZ),
              };
              chunkCells[cellIndex(kChunkSide, localX, localY, localZ)] = fixtureOccupied(occupied, globalCell) ? 1u : 0u;
            }
        SparseVoxelTree64Chunk tree;
        std::string error;
        assert(buildSparseVoxelTree64(kChunkSide, chunkCells, tree, &error));
        assert(tree.nodes.size() <= slotCapacity);

        std::array<uint32_t, 3> physical{};
        assert(sparseVoxelTree64DirectorySlot({x, y, z}, state.originWorldChunk, state.logicalChunkResolution, state.wrapOffset, physical));
        const uint32_t directoryIndex = physical[0] + kChunkAxis * (physical[1] + kChunkAxis * physical[2]);
        auto &descriptor = directory[directoryIndex];
        descriptor.worldChunk = {x, y, z};
        descriptor.nodeSlot = slot;
        descriptor.rootNodeOffset = 0u;
        descriptor.nodeCount = static_cast<uint32_t>(tree.nodes.size());
        // Retained toroidal chunks keep their own older build generation when
        // the directory publication generation advances.
        descriptor.contentGeneration = x == -1 ? state.contentGeneration - 1u : state.contentGeneration;
        descriptor.flags = kSparseVoxelTree64ChunkValid;
        if (sparseVoxelTree64PopulationMask(tree.nodes.front()) != 0u)
          descriptor.flags |= kSparseVoxelTree64ChunkOccupied;
        std::copy(tree.nodes.begin(), tree.nodes.end(), nodePool.begin() + uint64_t{slot} * slotCapacity);
        ++slot;
      }

  std::vector<SparseVoxelTree64Ray> rays{
    {{-40.0, 0.5, 0.5}, {1.0, 0.0, 0.0}, 100.0},
    {{40.0, 0.5, 0.5}, {-1.0, 0.0, 0.0}, 100.0},
    {{-40.0, 15.5, 15.5}, {1.0, 0.0, 0.0}, 100.0},
    {{-40.0, 14.5, -14.5}, {1.0, 0.0, 0.0}, 100.0},
    {{0.25, 0.25, 0.25}, {1.0, 0.0, 0.0}, 100.0},
    {{0.0, 0.0, 8.0}, {-0.001, -0.001, -1.0}, 100.0},
    {{-40.0, 0.5, 0.5}, {1.0, 0.0, 0.0}, 10.0},
    {{16.0, 15.5, 15.5}, {-1.0, 0.0, 0.0}, 100.0},
    {{-16.0, -16.0, -16.0}, {1.0, 1.0, 1.0}, 100.0},
    {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, 100.0},
    {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, -1.0},
  };
  std::mt19937 random(0x7642u);
  std::uniform_real_distribution<double> originDistribution(-48.0, 48.0);
  std::uniform_real_distribution<double> directionDistribution(-1.0, 1.0);
  for (uint32_t rayIndex = 0u; rayIndex < 512u; ++rayIndex)
  {
    std::array<double, 3> direction{
      directionDistribution(random),
      directionDistribution(random),
      directionDistribution(random),
    };
    const double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (length < 1.0e-8)
      direction = {1.0, 0.0, 0.0};
    else
      for (double &component : direction)
        component /= length;
    rays.push_back({
      {originDistribution(random), originDistribution(random), originDistribution(random)},
      direction,
      160.0,
    });
  }
  std::vector<SparseVoxelOccupancyDDARayGPU> gpuRays;
  gpuRays.reserve(rays.size());
  for (const auto &ray : rays)
    gpuRays.push_back(gpuRay(ray));

  RenderGraph graph(&rhi);
  const auto makeUploadBuffer = [&](const std::string &name, const uint64_t size, const BufferUsage usage)
  {
    return graph.createBuffer({
      .name = name,
      .size = size,
      .usage = usage | BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
  };
  const Buffer stateBuffer = makeUploadBuffer("Tree64.State", sizeof(state), BufferUsage::BufferUsage_Storage);
  const Buffer chunkBuffer = makeUploadBuffer("Tree64.Chunks", directory.size() * sizeof(SparseVoxelTree64ChunkGPU), BufferUsage::BufferUsage_Storage);
  const Buffer nodeBuffer = makeUploadBuffer("Tree64.Nodes", nodePool.size() * sizeof(SparseVoxelTree64NodeGPU), BufferUsage::BufferUsage_Storage);
  const Buffer rayBuffer = makeUploadBuffer("Tree64.Rays", gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU), BufferUsage::BufferUsage_Storage);
  const Buffer resultBuffer = graph.createBuffer({
    .name = "Tree64.Results",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer readback = graph.createBuffer({
    .name = "Tree64.Readback",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  graph.bufferWrite(stateBuffer, 0u, sizeof(state), &state);
  graph.bufferWrite(chunkBuffer, 0u, directory.size() * sizeof(SparseVoxelTree64ChunkGPU), directory.data());
  graph.bufferWrite(nodeBuffer, 0u, nodePool.size() * sizeof(SparseVoxelTree64NodeGPU), nodePool.data());
  graph.bufferWrite(rayBuffer, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU), gpuRays.data());

  uint32_t passIndex = 0u;
  auto *trace = graph.registerPass<svrRendering::SparseVoxelTree64TracePass>(
      "Tree64.Trace",
      passIndex++,
      svrRendering::SparseVoxelTree64TracePass::Buffers{
        stateBuffer,
        chunkBuffer,
        nodeBuffer,
        rayBuffer,
        resultBuffer,
      },
      svrRendering::SparseVoxelTree64TracePass::Capacities{
        static_cast<uint32_t>(directory.size()),
        static_cast<uint32_t>(nodePool.size()),
        slotCapacity,
        static_cast<uint32_t>(gpuRays.size()),
      });
  assert(trace->update(static_cast<uint32_t>(gpuRays.size()), 4096u));
  auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "Tree64.Copy", passIndex++, resultBuffer, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU), readback, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
  graph.compile();

  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
  std::vector<SparseVoxelOccupancyDDAResultGPU> actual(rays.size());
  graph.bufferRead(
      readback,
      0u,
      actual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
      [&](const void *data)
      {
        std::memcpy(actual.data(), data, actual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
      });

  for (uint32_t rayIndex = 0u; rayIndex < rays.size(); ++rayIndex)
  {
    const ReferenceResult expected = traceFixture(occupied, rays[rayIndex]);
    const SparseVoxelOccupancyTraceResult packedExpected = traceSparseVoxelOccupancyMip0(
        packedVolume,
        SparseVoxelOccupancyRay{
          {static_cast<float>(rays[rayIndex].origin[0]), static_cast<float>(rays[rayIndex].origin[1]), static_cast<float>(rays[rayIndex].origin[2])},
          {static_cast<float>(rays[rayIndex].direction[0]), static_cast<float>(rays[rayIndex].direction[1]), static_cast<float>(rays[rayIndex].direction[2])},
          rays[rayIndex].maximumDistance,
        });
    assert(static_cast<uint32_t>(packedExpected.status) == static_cast<uint32_t>(expected.status));
    if (expected.status == SparseVoxelTree64TraceStatus::Hit)
      assert(packedExpected.globalCell == expected.cell);
    if (actual[rayIndex].status != static_cast<uint32_t>(expected.status) || (expected.status == SparseVoxelTree64TraceStatus::Hit && actual[rayIndex].globalCell != expected.cell))
    {
      std::cerr << "Tree64 GPU mismatch ray " << rayIndex << ": expected status " << static_cast<uint32_t>(expected.status) << " cell (" << expected.cell[0] << ", " << expected.cell[1] << ", " << expected.cell[2]
                << "), actual status " << actual[rayIndex].status << " cell (" << actual[rayIndex].globalCell[0] << ", " << actual[rayIndex].globalCell[1] << ", " << actual[rayIndex].globalCell[2] << "), distance "
                << actual[rayIndex].distance << ", queries " << actual[rayIndex].queryCount << ", chunk steps " << actual[rayIndex].reserved0 << ", stack " << actual[rayIndex].reserved1 << '\n';
    }
    assert(actual[rayIndex].status == static_cast<uint32_t>(expected.status));
    assert(actual[rayIndex].contentGeneration == state.contentGeneration);
    assert(actual[rayIndex].status != static_cast<uint32_t>(SparseVoxelTree64TraceStatus::QueryLimit));
    assert(actual[rayIndex].reserved1 <= 64u);
    if (expected.status == SparseVoxelTree64TraceStatus::Hit)
    {
      assert(actual[rayIndex].globalCell == expected.cell);
      assert(std::abs(static_cast<double>(actual[rayIndex].distance) - expected.distance) < 2.0e-4);
    }
  }

  constexpr uint32_t benchmarkFrames = 32u;
  const auto benchmarkGraph = [&](RenderGraph &benchmarkGraph)
  {
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < benchmarkFrames; ++iteration)
    {
      RenderGraph::Frame benchmarkFrame{};
      benchmarkGraph.run(benchmarkFrame, overrides);
      benchmarkGraph.waitFrame(benchmarkFrame);
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
  };
  const double packedMilliseconds = benchmarkGraph(graph);
  delete copy;
  delete trace;
  graph.deleteBuffer(readback);
  graph.deleteBuffer(resultBuffer);
  graph.deleteBuffer(rayBuffer);
  graph.deleteBuffer(nodeBuffer);
  graph.deleteBuffer(chunkBuffer);
  graph.deleteBuffer(stateBuffer);

  std::vector<SparseVoxelTree64NodePaddedGPU> paddedNodePool;
  paddedNodePool.reserve(nodePool.size());
  for (const auto &node : nodePool)
    paddedNodePool.push_back({
      node.childBaseAndFlags,
      node.populationMaskLo,
      node.populationMaskHi,
      0u,
    });
  RenderGraph paddedGraph(&rhi);
  const auto makePaddedUploadBuffer = [&](const std::string &name, const uint64_t size, const BufferUsage usage)
  {
    return paddedGraph.createBuffer({
      .name = name,
      .size = size,
      .usage = usage | BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
  };
  const Buffer paddedStateBuffer = makePaddedUploadBuffer("Tree64Padded.State", sizeof(state), BufferUsage::BufferUsage_Storage);
  const Buffer paddedChunkBuffer = makePaddedUploadBuffer("Tree64Padded.Chunks", directory.size() * sizeof(SparseVoxelTree64ChunkGPU), BufferUsage::BufferUsage_Storage);
  const Buffer paddedNodeBuffer = makePaddedUploadBuffer("Tree64Padded.Nodes", paddedNodePool.size() * sizeof(SparseVoxelTree64NodePaddedGPU), BufferUsage::BufferUsage_Storage);
  const Buffer paddedRayBuffer = makePaddedUploadBuffer("Tree64Padded.Rays", gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU), BufferUsage::BufferUsage_Storage);
  const Buffer paddedResultBuffer = paddedGraph.createBuffer({
    .name = "Tree64Padded.Results",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer paddedReadback = paddedGraph.createBuffer({
    .name = "Tree64Padded.Readback",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  paddedGraph.bufferWrite(paddedStateBuffer, 0u, sizeof(state), &state);
  paddedGraph.bufferWrite(paddedChunkBuffer, 0u, directory.size() * sizeof(SparseVoxelTree64ChunkGPU), directory.data());
  paddedGraph.bufferWrite(paddedNodeBuffer, 0u, paddedNodePool.size() * sizeof(SparseVoxelTree64NodePaddedGPU), paddedNodePool.data());
  paddedGraph.bufferWrite(paddedRayBuffer, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU), gpuRays.data());
  uint32_t paddedPassIndex = 0u;
  auto *paddedTrace = paddedGraph.registerPass<svrRendering::SparseVoxelTree64TracePass>(
      "Tree64Padded.Trace",
      paddedPassIndex++,
      svrRendering::SparseVoxelTree64TracePass::Buffers{
        paddedStateBuffer,
        paddedChunkBuffer,
        paddedNodeBuffer,
        paddedRayBuffer,
        paddedResultBuffer,
      },
      svrRendering::SparseVoxelTree64TracePass::Capacities{
        static_cast<uint32_t>(directory.size()),
        static_cast<uint32_t>(paddedNodePool.size()),
        slotCapacity,
        static_cast<uint32_t>(gpuRays.size()),
      },
      svrRendering::SparseVoxelTree64TracePass::NodeLayout::Padded16);
  assert(paddedTrace->update(static_cast<uint32_t>(gpuRays.size()), 4096u));
  auto *paddedCopy = paddedGraph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "Tree64Padded.Copy", paddedPassIndex++, paddedResultBuffer, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU), paddedReadback, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
  paddedGraph.compile();
  RenderGraph::Frame paddedFrame{};
  paddedGraph.run(paddedFrame, overrides);
  paddedGraph.waitFrame(paddedFrame);
  std::vector<SparseVoxelOccupancyDDAResultGPU> paddedActual(rays.size());
  paddedGraph.bufferRead(
      paddedReadback,
      0u,
      paddedActual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
      [&](const void *data)
      {
        std::memcpy(paddedActual.data(), data, paddedActual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
      });
  assert(std::memcmp(actual.data(), paddedActual.data(), actual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU)) == 0);

  const double paddedMilliseconds = benchmarkGraph(paddedGraph);
  delete paddedCopy;
  delete paddedTrace;
  paddedGraph.deleteBuffer(paddedReadback);
  paddedGraph.deleteBuffer(paddedResultBuffer);
  paddedGraph.deleteBuffer(paddedRayBuffer);
  paddedGraph.deleteBuffer(paddedNodeBuffer);
  paddedGraph.deleteBuffer(paddedChunkBuffer);
  paddedGraph.deleteBuffer(paddedStateBuffer);

  RenderGraph sharedGraph(&rhi);
  const auto makeSharedUploadBuffer = [&](const std::string &name, const uint64_t size, const BufferUsage usage)
  {
    return sharedGraph.createBuffer({
      .name = name,
      .size = size,
      .usage = usage | BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
  };
  const Buffer sharedStateBuffer = makeSharedUploadBuffer("Tree64Shared.State", sizeof(state), BufferUsage::BufferUsage_Storage);
  const Buffer sharedChunkBuffer = makeSharedUploadBuffer("Tree64Shared.Chunks", directory.size() * sizeof(SparseVoxelTree64ChunkGPU), BufferUsage::BufferUsage_Storage);
  const Buffer sharedNodeBuffer = makeSharedUploadBuffer("Tree64Shared.Nodes", nodePool.size() * sizeof(SparseVoxelTree64NodeGPU), BufferUsage::BufferUsage_Storage);
  const Buffer sharedRayBuffer = makeSharedUploadBuffer("Tree64Shared.Rays", gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU), BufferUsage::BufferUsage_Storage);
  const Buffer sharedResultBuffer = sharedGraph.createBuffer({
    .name = "Tree64Shared.Results",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer sharedReadback = sharedGraph.createBuffer({
    .name = "Tree64Shared.Readback",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  sharedGraph.bufferWrite(sharedStateBuffer, 0u, sizeof(state), &state);
  sharedGraph.bufferWrite(sharedChunkBuffer, 0u, directory.size() * sizeof(SparseVoxelTree64ChunkGPU), directory.data());
  sharedGraph.bufferWrite(sharedNodeBuffer, 0u, nodePool.size() * sizeof(SparseVoxelTree64NodeGPU), nodePool.data());
  sharedGraph.bufferWrite(sharedRayBuffer, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU), gpuRays.data());
  uint32_t sharedPassIndex = 0u;
  auto *sharedTrace = sharedGraph.registerPass<svrRendering::SparseVoxelTree64TracePass>(
      "Tree64Shared.Trace",
      sharedPassIndex++,
      svrRendering::SparseVoxelTree64TracePass::Buffers{
        sharedStateBuffer,
        sharedChunkBuffer,
        sharedNodeBuffer,
        sharedRayBuffer,
        sharedResultBuffer,
      },
      svrRendering::SparseVoxelTree64TracePass::Capacities{
        static_cast<uint32_t>(directory.size()),
        static_cast<uint32_t>(nodePool.size()),
        slotCapacity,
        static_cast<uint32_t>(gpuRays.size()),
      },
      svrRendering::SparseVoxelTree64TracePass::NodeLayout::Packed12,
      svrRendering::SparseVoxelTree64TracePass::Traversal::MirroredAncestor);
  assert(sharedTrace->update(static_cast<uint32_t>(gpuRays.size()), 4096u));
  auto *sharedCopy = sharedGraph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "Tree64Shared.Copy", sharedPassIndex++, sharedResultBuffer, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU), sharedReadback, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
  sharedGraph.compile();
  RenderGraph::Frame sharedFrame{};
  sharedGraph.run(sharedFrame, overrides);
  sharedGraph.waitFrame(sharedFrame);
  std::vector<SparseVoxelOccupancyDDAResultGPU> sharedActual(rays.size());
  sharedGraph.bufferRead(
      sharedReadback,
      0u,
      sharedActual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
      [&](const void *data)
      {
        std::memcpy(sharedActual.data(), data, sharedActual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
      });
  for (uint32_t rayIndex = 0u; rayIndex < sharedActual.size(); ++rayIndex)
  {
    const auto &reference = actual[rayIndex];
    const auto &candidate = sharedActual[rayIndex];
    if (candidate.status != reference.status ||
        (reference.status == static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Hit) &&
         (candidate.globalCell != reference.globalCell ||
          std::abs(candidate.distance - reference.distance) >= 2.0e-4f)))
    {
      std::cerr << "Tree64 optimized traversal mismatch ray " << rayIndex
                << ": reference status " << reference.status << " cell ("
                << reference.globalCell[0] << ", " << reference.globalCell[1] << ", "
                << reference.globalCell[2] << ") distance " << reference.distance
                << ", candidate status " << candidate.status << " cell ("
                << candidate.globalCell[0] << ", " << candidate.globalCell[1] << ", "
                << candidate.globalCell[2] << ") distance " << candidate.distance
                << ", queries " << candidate.queryCount << ", node visits "
                << candidate.baseCellSteps << ", stack " << candidate.reserved1 << '\n';
    }
    assert(candidate.status == reference.status);
    assert(candidate.contentGeneration == reference.contentGeneration);
    if (reference.status == static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Hit))
    {
      assert(candidate.globalCell == reference.globalCell);
      assert(std::abs(candidate.distance - reference.distance) < 2.0e-4f);
    }
  }
  const double sharedMilliseconds = benchmarkGraph(sharedGraph);
  std::cout << "Tree64 static GPU benchmark: packed12/region " << packedMilliseconds << " ms, padded16/region " << paddedMilliseconds << " ms, packed12/mirrored-ancestor " << sharedMilliseconds << " ms for "
            << rays.size() * benchmarkFrames << " rays including submit/wait\n";

  delete sharedCopy;
  delete sharedTrace;
  sharedGraph.deleteBuffer(sharedReadback);
  sharedGraph.deleteBuffer(sharedResultBuffer);
  sharedGraph.deleteBuffer(sharedRayBuffer);
  sharedGraph.deleteBuffer(sharedNodeBuffer);
  sharedGraph.deleteBuffer(sharedChunkBuffer);
  sharedGraph.deleteBuffer(sharedStateBuffer);

  RenderGraph ddaGraph(&rhi);
  uint32_t ddaPassIndex = 0u;
  svrRendering::SparseVoxelOccupancyMipChain ddaChain(ddaGraph, ddaPassIndex, packedVolume, packedSources, static_cast<uint32_t>(packedSources.size()), "Tree64PackedParity");
  const Buffer ddaRayBuffer = ddaGraph.createBuffer({
    .name = "Tree64PackedParity.Rays",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer ddaResultBuffer = ddaGraph.createBuffer({
    .name = "Tree64PackedParity.Results",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer ddaReadback = ddaGraph.createBuffer({
    .name = "Tree64PackedParity.Readback",
    .size = gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  ddaGraph.bufferWrite(ddaRayBuffer, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDARayGPU), gpuRays.data());
  auto *ddaTrace = ddaGraph.registerPass<svrRendering::SparseVoxelOccupancyDDAPass>(
      "Tree64PackedParity.Trace",
      ddaPassIndex++,
      ddaChain.occupancyTexture(),
      svrRendering::SparseVoxelOccupancyDDAPass::Buffers{
        ddaChain.baseState(),
        ddaChain.pyramidState(),
        ddaChain.levelDescriptors(),
        ddaChain.mipWords(),
        ddaRayBuffer,
        ddaResultBuffer,
      },
      svrRendering::SparseVoxelOccupancyDDAPass::Capacities{
        static_cast<uint32_t>(rays.size()),
        static_cast<uint32_t>(packedVolume.mipLevels().size()),
        packedVolume.pyramidLayout().wordCount,
      });
  assert(ddaTrace->update(static_cast<uint32_t>(rays.size()), 4096u));
  auto *ddaCopy = ddaGraph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "Tree64PackedParity.Copy", ddaPassIndex++, ddaResultBuffer, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU), ddaReadback, 0u, gpuRays.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
  ddaGraph.compile();
  RenderGraph::Frame ddaFrame{};
  ddaGraph.run(ddaFrame, overrides);
  ddaGraph.waitFrame(ddaFrame);
  std::vector<SparseVoxelOccupancyDDAResultGPU> ddaActual(rays.size());
  ddaGraph.bufferRead(
      ddaReadback,
      0u,
      ddaActual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU),
      [&](const void *data)
      {
        std::memcpy(ddaActual.data(), data, ddaActual.size() * sizeof(SparseVoxelOccupancyDDAResultGPU));
      });
  for (uint32_t rayIndex = 0u; rayIndex < rays.size(); ++rayIndex)
  {
    assert(ddaActual[rayIndex].status == actual[rayIndex].status);
    if (actual[rayIndex].status == static_cast<uint32_t>(SparseVoxelTree64TraceStatus::Hit))
      assert(ddaActual[rayIndex].globalCell == actual[rayIndex].globalCell);
  }
  delete ddaCopy;
  delete ddaTrace;
  ddaGraph.deleteBuffer(ddaReadback);
  ddaGraph.deleteBuffer(ddaResultBuffer);
  ddaGraph.deleteBuffer(ddaRayBuffer);
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
  testGPUStaticTraversal(rhi);
  std::cout << "Sparse voxel Tree64 T2 GPU tests passed\n";
  os::Logger::shutdown();
  return 0;
}
