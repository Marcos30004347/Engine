#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelOccupancyVolume.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyMipChain.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyProbePass.hpp"

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
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

std::vector<uint32_t> packedExpected(const SparseVoxelOccupancyVolume &volume)
{
  std::vector<uint32_t> packed(volume.pyramidLayout().wordCount, 0u);
  for (const SparseVoxelOccupancyMipLevel &level : volume.mipLevels())
    std::copy(level.words.begin(), level.words.end(), packed.begin() + level.state.wordOffset);
  return packed;
}

std::array<float, 16> translationMatrix(
    const std::array<float, 3> &translation)
{
  return {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    translation[0], translation[1], translation[2], 1.0f,
  };
}

std::vector<SparseVoxelAABB> translatedCubeBounds(
    const std::vector<SparseVoxelCenterLengthGPU> &centers,
    const std::array<float, 3> &translation)
{
  std::vector<SparseVoxelAABB> bounds;
  bounds.reserve(centers.size());
  for (const SparseVoxelCenterLengthGPU &center : centers)
  {
    SparseVoxelAABB box{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float worldCenter =
          center.center[axis] + translation[axis];
      box.minimum[axis] = worldCenter - center.length * 0.5f;
      box.maximum[axis] = worldCenter + center.length * 0.5f;
    }
    bounds.push_back(box);
  }
  return bounds;
}

SparseVoxelAABB enclosingBounds(
    const std::vector<SparseVoxelAABB> &bounds)
{
  SparseVoxelAABB result = bounds.front();
  for (const SparseVoxelAABB &box : bounds)
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      result.minimum[axis] =
          std::min(result.minimum[axis], box.minimum[axis]);
      result.maximum[axis] =
          std::max(result.maximum[axis], box.maximum[axis]);
    }
  return result;
}

struct OccupancyProbes
{
  std::vector<std::array<float, 4>> positions;
  std::vector<uint32_t> expected;
};

OccupancyProbes makeOccupancyProbes(const SparseVoxelOccupancyVolume &volume)
{
  OccupancyProbes probes;
  const SparseVoxelOccupancyVolumeGPU &state = volume.state();
  const uint64_t cellCount = uint64_t{state.logicalResolution[0]} * state.logicalResolution[1] * state.logicalResolution[2];
  probes.positions.reserve(cellCount + 2u);
  probes.expected.reserve(cellCount + 2u);
  for (uint32_t z = 0u; z < state.logicalResolution[2]; ++z)
    for (uint32_t y = 0u; y < state.logicalResolution[1]; ++y)
      for (uint32_t x = 0u; x < state.logicalResolution[0]; ++x)
      {
        std::array<float, 4> position{
          (static_cast<float>(state.originCell[0]) + static_cast<float>(x) + 0.5f) * state.cellSize[0],
          (static_cast<float>(state.originCell[1]) + static_cast<float>(y) + 0.5f) * state.cellSize[1],
          (static_cast<float>(state.originCell[2]) + static_cast<float>(z) + 0.5f) * state.cellSize[2],
          0.0f,
        };
        probes.positions.push_back(position);
        probes.expected.push_back(volume.occupiedAtWorldPosition({position[0], position[1], position[2]}) ? 1u : 0u);
      }
  for (const float direction : {-1.0f, 1.0f})
  {
    const int32_t globalX = direction < 0.0f ? state.originCell[0] - 1 : state.originCell[0] + static_cast<int32_t>(state.logicalResolution[0]);
    probes.positions.push_back({
      (static_cast<float>(globalX) + 0.5f) * state.cellSize[0],
      0.0f,
      0.0f,
      0.0f,
    });
    probes.expected.push_back(0u);
  }
  return probes;
}

void testFullAndIncrementalPyramid(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelOccupancySettings settings{};
  settings.worldExtent = {129.0f, 17.0f, 9.0f};
  settings.logicalResolution = {129u, 17u, 9u};
  settings.memoryCeilingBytes = 1u << 20u;

  SparseVoxelOccupancyVolume volume;
  std::string error;
  assert(volume.configure(settings, {}, &error));
  const std::vector<SparseVoxelAABB> sources{
    {{-61.1f, -7.1f, -3.1f}, {-56.9f, -4.2f, -1.2f}},
    {{-1.1f, -0.1f, -0.1f}, {1.1f, 1.1f, 1.1f}},
    {{55.2f, 3.2f, -2.9f}, {61.8f, 7.2f, 2.9f}},
    {{10.0f, -4.0f, 2.0f}, {10.0f, -4.0f, 2.0f}},
  };
  assert(volume.update({0.25f, 0.25f, 0.25f}, 1u, sources, &error));

  RenderGraph graph(&rhi);
  uint32_t passIndex = 0u;
  svrRendering::SparseVoxelOccupancyMipChain mipChain(graph, passIndex, volume, sources, static_cast<uint32_t>(sources.size()), "g27.Occupancy");
  const uint32_t probeCapacity = settings.logicalResolution[0] * settings.logicalResolution[1] * settings.logicalResolution[2] + 2u;
  const Buffer probePositions = graph.createBuffer({
    .name = "g24.ProbePositions",
    .size = uint64_t{probeCapacity} * sizeof(std::array<float, 4>),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer probeResults = graph.createBuffer({
    .name = "g24.ProbeResults",
    .size = uint64_t{probeCapacity} * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
  });
  const Buffer probeReadback = graph.createBuffer({
    .name = "g24.ProbeReadback",
    .size = uint64_t{probeCapacity} * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer baseReadback = graph.createBuffer({
    .name = "g24.BaseReadback",
    .size = volume.layout().wordCount * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer readback = graph.createBuffer({
    .name = "g27.Readback",
    .size = volume.pyramidLayout().wordCount * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });

  std::vector<Pass *> ownedPasses;
  auto *probePass = graph.registerPass<svrRendering::SparseVoxelOccupancyProbePass>(
      "g24.ProbeTexture",
      passIndex++,
      mipChain.occupancyTexture(),
      svrRendering::SparseVoxelOccupancyProbePass::Buffers{
        probePositions,
        probeResults,
      },
      probeCapacity);
  ownedPasses.push_back(probePass);
  ownedPasses.push_back(graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g24.CopyProbeResults", passIndex++, probeResults, 0u, uint64_t{probeCapacity} * sizeof(uint32_t), probeReadback, 0u, uint64_t{probeCapacity} * sizeof(uint32_t)));
  ownedPasses.push_back(graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g24.CopyBaseWords", passIndex++, mipChain.baseWords(), 0u, volume.layout().wordCount * sizeof(uint32_t), baseReadback, 0u, volume.layout().wordCount * sizeof(uint32_t)));
  ownedPasses.push_back(graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "g27.CopyMipWords", passIndex++, mipChain.mipWords(), 0u, volume.pyramidLayout().wordCount * sizeof(uint32_t), readback, 0u, volume.pyramidLayout().wordCount * sizeof(uint32_t)));
  graph.compile();

  const std::array<std::array<float, 3>, 10> cameraWalk{{
    {0.25f, 0.25f, 0.25f},
    {1.25f, 0.25f, 0.25f},
    {1.75f, 0.75f, 0.75f},
    {2.25f, 1.25f, 0.25f},
    {-1.25f, 1.25f, -0.75f},
    {-63.25f, -2.25f, 1.25f},
    {63.25f, 2.25f, -1.75f},
    {129.25f, 0.25f, 0.25f},
    {-129.25f, -0.75f, 0.25f},
    {3.25f, 0.25f, 0.25f},
  }};

  std::vector<SparseVoxelAABB> activeSources = sources;
  uint64_t sceneRevision = 1u;
  for (uint32_t iteration = 0u; iteration < cameraWalk.size(); ++iteration)
  {
    if (iteration == 8u)
    {
      activeSources.clear();
      sceneRevision = 2u;
    }
    if (iteration > 0u)
      assert(volume.update(cameraWalk[iteration], sceneRevision, activeSources, &error));

    assert(mipChain.update(volume, activeSources, &error));
    assert(mipChain.publishedState().contentGeneration == volume.state().contentGeneration);
    assert(mipChain.publishedState().baseDirtyWordCount == volume.lastDirtyWords().size());
    assert(mipChain.publishedState().levelCount == volume.mipLevels().size());
    const OccupancyProbes probes = makeOccupancyProbes(volume);
    assert(probes.positions.size() == probeCapacity);
    graph.bufferWrite(probePositions, 0u, probes.positions.size() * sizeof(std::array<float, 4>), const_cast<std::array<float, 4> *>(probes.positions.data()));
    assert(probePass->update(volume.state(), static_cast<uint32_t>(probes.positions.size())));

    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);

    std::vector<uint32_t> actualBase(volume.layout().wordCount);
    graph.bufferRead(
        baseReadback,
        0u,
        actualBase.size() * sizeof(uint32_t),
        [&](const void *data)
        {
          std::memcpy(actualBase.data(), data, actualBase.size() * sizeof(uint32_t));
        });
    assert(actualBase == volume.words());

    std::vector<uint32_t> actualProbes(probeCapacity);
    graph.bufferRead(
        probeReadback,
        0u,
        actualProbes.size() * sizeof(uint32_t),
        [&](const void *data)
        {
          std::memcpy(actualProbes.data(), data, actualProbes.size() * sizeof(uint32_t));
        });
    assert(actualProbes == probes.expected);

    std::vector<uint32_t> actual(volume.pyramidLayout().wordCount);
    graph.bufferRead(
        readback,
        0u,
        actual.size() * sizeof(uint32_t),
        [&](const void *data)
        {
          std::memcpy(actual.data(), data, actual.size() * sizeof(uint32_t));
        });
    const std::vector<uint32_t> expected = packedExpected(volume);
    if (actual != expected)
    {
      std::cerr << "Occupancy mip mismatch at camera-walk iteration " << iteration << '\n';
      for (uint32_t wordIndex = 0u; wordIndex < actual.size(); ++wordIndex)
        if (actual[wordIndex] != expected[wordIndex])
          std::cerr << "  word " << wordIndex << ": expected 0x" << std::hex << expected[wordIndex] << ", actual 0x" << actual[wordIndex] << std::dec << '\n';
    }
    assert(actual == expected);
  }

  // A scene edit at a stationary camera dirties an interior region rather
  // than selecting the camera-scroll/full-rebuild path.
  const SparseVoxelAABB inserted{{9.2f, -1.2f, -1.2f}, {11.8f, 1.2f, 1.2f}};
  activeSources = {inserted};
  sceneRevision = 3u;
  assert(volume.update(cameraWalk.back(), sceneRevision, activeSources, {inserted}, &error));
  assert(volume.state().fullRebuild == 0u);
  assert(!volume.lastDirtyWords().empty());
  assert(mipChain.update(volume, activeSources, &error));
  const OccupancyProbes interiorProbes = makeOccupancyProbes(volume);
  graph.bufferWrite(
      probePositions,
      0u,
      interiorProbes.positions.size() * sizeof(std::array<float, 4>),
      const_cast<std::array<float, 4> *>(interiorProbes.positions.data()));
  assert(probePass->update(volume.state(), static_cast<uint32_t>(interiorProbes.positions.size())));
  RenderGraph::Frame interiorFrame{};
  RenderGraph::Overrides interiorOverrides{};
  graph.run(interiorFrame, interiorOverrides);
  graph.waitFrame(interiorFrame);

  std::vector<uint32_t> actualInteriorBase(volume.layout().wordCount);
  graph.bufferRead(
      baseReadback,
      0u,
      actualInteriorBase.size() * sizeof(uint32_t),
      [&](const void *data)
      {
        std::memcpy(actualInteriorBase.data(), data, actualInteriorBase.size() * sizeof(uint32_t));
      });
  assert(actualInteriorBase == volume.words());
  std::vector<uint32_t> actualInteriorMips(volume.pyramidLayout().wordCount);
  graph.bufferRead(
      readback,
      0u,
      actualInteriorMips.size() * sizeof(uint32_t),
      [&](const void *data)
      {
        std::memcpy(actualInteriorMips.data(), data, actualInteriorMips.size() * sizeof(uint32_t));
      });
  assert(actualInteriorMips == packedExpected(volume));
  std::vector<uint32_t> actualInteriorProbes(probeCapacity);
  graph.bufferRead(
      probeReadback,
      0u,
      actualInteriorProbes.size() * sizeof(uint32_t),
      [&](const void *data)
      {
        std::memcpy(actualInteriorProbes.data(), data, actualInteriorProbes.size() * sizeof(uint32_t));
      });
  assert(actualInteriorProbes == interiorProbes.expected);

  for (auto it = ownedPasses.rbegin(); it != ownedPasses.rend(); ++it)
    delete *it;
  graph.deleteBuffer(readback);
  graph.deleteBuffer(baseReadback);
  graph.deleteBuffer(probeReadback);
  graph.deleteBuffer(probeResults);
  graph.deleteBuffer(probePositions);
}

void testGPUGeneratedDirtyMasksAndSources(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelOccupancySettings settings{};
  settings.worldExtent = {35.0f, 7.0f, 5.0f};
  settings.logicalResolution = {35u, 7u, 5u};
  settings.memoryCeilingBytes = 1u << 20u;
  settings.maintainCPUReference = false;
  const std::vector<SparseVoxelCenterLengthGPU> centers{
    {{{-3.25f, 0.0f, 0.0f}}, 1.5f},
    {{{2.0f, 1.0f, -1.0f}}, 2.0f},
  };
  const std::array<VisibleVoxelGPU, 2u> leaves{{
    {0u, 0u, 0u, 0u},
    {0u, 1u, 0u, 0u},
  }};
  const std::array<float, 3> initialTranslation{0.5f, -0.25f, 0.75f};
  std::vector<SparseVoxelAABB> sources =
      translatedCubeBounds(centers, initialTranslation);

  SparseVoxelOccupancyVolume gpuVolume;
  SparseVoxelOccupancyVolume oracle;
  SparseVoxelOccupancySettings oracleSettings = settings;
  oracleSettings.maintainCPUReference = true;
  std::string error;
  assert(gpuVolume.configure(settings, {}, &error));
  assert(oracle.configure(oracleSettings, {}, &error));
  const std::array<float, 3> initialCamera{0.25f, 0.25f, 0.25f};
  assert(gpuVolume.updateGPU(initialCamera, 1u, {}, &error));
  assert(oracle.update(initialCamera, 1u, sources, &error));

  RenderGraph graph(&rhi);
  const Buffer leafBuffer = graph.createBuffer({
    .name = "g25.gpuGenerated.Leaves",
    .size = sizeof(leaves),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer instanceBuffer = graph.createBuffer({
    .name = "g25.gpuGenerated.Instances",
    .size = sizeof(SparseVoxelInstanceGPU),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer centerBuffer = graph.createBuffer({
    .name = "g25.gpuGenerated.Centers",
    .size = centers.size() * sizeof(SparseVoxelCenterLengthGPU),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer baseReadback = graph.createBuffer({
    .name = "g25.gpuGenerated.BaseReadback",
    .size = gpuVolume.layout().wordCount * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer mipReadback = graph.createBuffer({
    .name = "g25.gpuGenerated.MipReadback",
    .size = gpuVolume.pyramidLayout().wordCount * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = translationMatrix(initialTranslation);
  instance.worldToModel = translationMatrix(
      {-initialTranslation[0],
       -initialTranslation[1],
       -initialTranslation[2]});
  instance.previousModelToWorld = instance.modelToWorld;
  graph.bufferWrite(
      leafBuffer, 0u, sizeof(leaves),
      const_cast<VisibleVoxelGPU *>(leaves.data()));
  graph.bufferWrite(
      instanceBuffer, 0u, sizeof(instance), &instance);
  graph.bufferWrite(
      centerBuffer,
      0u,
      centers.size() * sizeof(SparseVoxelCenterLengthGPU),
      const_cast<SparseVoxelCenterLengthGPU *>(centers.data()));

  uint32_t passIndex = 0u;
  {
    const svrRendering::SparseVoxelOccupancyMipChain::
        GPUGeneratedSources generated{
          leafBuffer,
          instanceBuffer,
          centerBuffer,
          static_cast<uint32_t>(leaves.size()),
          1u,
          static_cast<uint32_t>(centers.size()),
        };
    svrRendering::SparseVoxelOccupancyMipChain mipChain(
        graph,
        passIndex,
        gpuVolume,
        {},
        static_cast<uint32_t>(leaves.size()),
        "g25.GPUGenerated",
        &generated);
    std::vector<Pass *> copyPasses;
    copyPasses.push_back(
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
            "g25.CopyGPUGeneratedBase",
            passIndex++,
            mipChain.baseWords(),
            0u,
            gpuVolume.layout().wordCount * sizeof(uint32_t),
            baseReadback,
            0u,
            gpuVolume.layout().wordCount * sizeof(uint32_t)));
    copyPasses.push_back(
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
            "g25.CopyGPUGeneratedMips",
            passIndex++,
            mipChain.mipWords(),
            0u,
            gpuVolume.pyramidLayout().wordCount * sizeof(uint32_t),
            mipReadback,
            0u,
            gpuVolume.pyramidLayout().wordCount * sizeof(uint32_t)));
    graph.compile();

    const auto verify = [&]()
    {
      RenderGraph::Frame frame{};
      RenderGraph::Overrides overrides{};
      graph.run(frame, overrides);
      graph.waitFrame(frame);
      std::vector<uint32_t> actualBase(gpuVolume.layout().wordCount);
      graph.bufferRead(
          baseReadback,
          0u,
          actualBase.size() * sizeof(uint32_t),
          [&](const void *data)
          {
            std::memcpy(
                actualBase.data(),
                data,
                actualBase.size() * sizeof(uint32_t));
          });
      if (actualBase != oracle.words())
      {
        std::cerr << "GPU-generated occupancy base mismatch\n";
        for (uint32_t wordIndex = 0u;
             wordIndex < actualBase.size();
             ++wordIndex)
          if (actualBase[wordIndex] != oracle.words()[wordIndex])
            std::cerr
                << "  word " << wordIndex
                << ": expected 0x" << std::hex
                << oracle.words()[wordIndex]
                << ", actual 0x" << actualBase[wordIndex]
                << std::dec << '\n';
      }
      assert(actualBase == oracle.words());
      std::vector<uint32_t> actualMips(
          gpuVolume.pyramidLayout().wordCount);
      graph.bufferRead(
          mipReadback,
          0u,
          actualMips.size() * sizeof(uint32_t),
          [&](const void *data)
          {
            std::memcpy(
                actualMips.data(),
                data,
                actualMips.size() * sizeof(uint32_t));
          });
      assert(actualMips == packedExpected(oracle));
    };
    verify();

    const std::array<float, 3> scrolledCamera{
      1.25f, -0.75f, 0.25f};
    assert(gpuVolume.updateGPU(
        scrolledCamera, 1u, {}, &error));
    assert(oracle.update(
        scrolledCamera, 1u, sources, &error));
    assert(mipChain.updateGenerated(
        gpuVolume,
        static_cast<uint32_t>(leaves.size()),
        {},
        &error));
    verify();

    const std::array<float, 3> movedTranslation{
      4.0f, 0.5f, -0.5f};
    const std::vector<SparseVoxelAABB> movedSources =
        translatedCubeBounds(centers, movedTranslation);
    const std::vector<SparseVoxelAABB> invalidations{
      enclosingBounds(sources),
      enclosingBounds(movedSources),
    };
    instance.previousModelToWorld = instance.modelToWorld;
    instance.modelToWorld = translationMatrix(movedTranslation);
    instance.worldToModel = translationMatrix(
        {-movedTranslation[0],
         -movedTranslation[1],
         -movedTranslation[2]});
    graph.bufferWrite(
        instanceBuffer, 0u, sizeof(instance), &instance);
    assert(gpuVolume.updateGPU(
        scrolledCamera, 2u, invalidations, &error));
    assert(oracle.update(
        scrolledCamera,
        2u,
        movedSources,
        invalidations,
        &error));
    assert(mipChain.updateGenerated(
        gpuVolume,
        static_cast<uint32_t>(leaves.size()),
        invalidations,
        &error));
    sources = movedSources;
    verify();

    const std::vector<SparseVoxelAABB> removal{
      enclosingBounds(sources)};
    assert(gpuVolume.updateGPU(
        scrolledCamera, 3u, removal, &error));
    assert(oracle.update(
        scrolledCamera, 3u, {}, removal, &error));
    assert(mipChain.updateGenerated(
        gpuVolume, 0u, removal, &error));
    verify();

    for (auto it = copyPasses.rbegin();
         it != copyPasses.rend();
         ++it)
      delete *it;
  }
  graph.deleteBuffer(mipReadback);
  graph.deleteBuffer(baseReadback);
  graph.deleteBuffer(centerBuffer);
  graph.deleteBuffer(instanceBuffer);
  graph.deleteBuffer(leafBuffer);
}

void testGPUSceneTraversal(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelOccupancySettings settings{};
  settings.worldExtent = {35.0f, 7.0f, 5.0f};
  settings.logicalResolution = {35u, 7u, 5u};
  settings.memoryCeilingBytes = 1u << 20u;
  settings.maintainCPUReference = false;
  const std::vector<SparseVoxelCenterLengthGPU> centers{
    {{{0.0f, 0.0f, 0.0f}}, 10.0f},
    {{{-3.25f, 0.0f, 0.0f}}, 1.5f},
    {{{2.0f, 1.0f, -1.0f}}, 2.0f},
  };
  const std::array<SparseVoxelLODNodeGPU, 3u> nodes{{
    {1u, 2u, 0u, 1u},
    {0u, 0u, 1u, 2u},
    {0u, 0u, 2u, 2u},
  }};
  const std::array<SparseVoxelBLASRootRangeGPU, 1u> roots{{
    {0u, 1u, 2u, 0u},
  }};
  const std::array<float, 3> initialTranslation{
    0.5f, -0.25f, 0.75f};
  const std::vector<SparseVoxelCenterLengthGPU> leafCenters{
    centers[1], centers[2]};
  std::vector<SparseVoxelAABB> sources =
      translatedCubeBounds(leafCenters, initialTranslation);

  SparseVoxelOccupancyVolume gpuVolume;
  SparseVoxelOccupancyVolume oracle;
  SparseVoxelOccupancySettings oracleSettings = settings;
  oracleSettings.maintainCPUReference = true;
  std::string error;
  assert(gpuVolume.configure(settings, {}, &error));
  assert(oracle.configure(oracleSettings, {}, &error));
  const std::array<float, 3> initialCamera{0.25f, 0.25f, 0.25f};
  assert(gpuVolume.updateGPU(initialCamera, 1u, {}, &error));
  assert(oracle.update(initialCamera, 1u, sources, &error));

  RenderGraph graph(&rhi);
  const Buffer instanceBuffer = graph.createBuffer({
    .name = "g25.sceneTraversal.Instances",
    .size = sizeof(SparseVoxelInstanceGPU),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer nodeBuffer = graph.createBuffer({
    .name = "g25.sceneTraversal.Nodes",
    .size = sizeof(nodes),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer centerBuffer = graph.createBuffer({
    .name = "g25.sceneTraversal.Centers",
    .size = centers.size() *
        sizeof(SparseVoxelCenterLengthGPU),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer rootBuffer = graph.createBuffer({
    .name = "g25.sceneTraversal.Roots",
    .size = sizeof(roots),
    .usage = BufferUsage::BufferUsage_Storage |
        BufferUsage::BufferUsage_Push,
    .frameLocal = true,
  });
  const Buffer baseReadback = graph.createBuffer({
    .name = "g25.sceneTraversal.BaseReadback",
    .size = gpuVolume.layout().wordCount * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer mipReadback = graph.createBuffer({
    .name = "g25.sceneTraversal.MipReadback",
    .size = gpuVolume.pyramidLayout().wordCount *
        sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer dirtyCountReadback = graph.createBuffer({
    .name = "g25.sceneTraversal.DirtyCountReadback",
    .size = sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  const Buffer mipDispatchReadback = graph.createBuffer({
    .name = "g25.sceneTraversal.MipDispatchReadback",
    .size = gpuVolume.mipLevels().size() *
        sizeof(IndirectDispatchArgsGPU),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  SparseVoxelInstanceGPU instance{};
  const auto uploadInstance =
      [&](const std::array<float, 3> &translation)
  {
    instance.modelToWorld = translationMatrix(translation);
    instance.worldToModel = translationMatrix({
      -translation[0], -translation[1], -translation[2]});
    instance.previousModelToWorld = instance.modelToWorld;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      instance.worldBoundsMin[axis] =
          translation[axis] - 5.0f;
      instance.worldBoundsMax[axis] =
          translation[axis] + 5.0f;
    }
    graph.bufferWrite(
        instanceBuffer, 0u, sizeof(instance), &instance);
  };
  uploadInstance(initialTranslation);
  graph.bufferWrite(
      nodeBuffer,
      0u,
      sizeof(nodes),
      const_cast<SparseVoxelLODNodeGPU *>(nodes.data()));
  graph.bufferWrite(
      centerBuffer,
      0u,
      centers.size() * sizeof(SparseVoxelCenterLengthGPU),
      const_cast<SparseVoxelCenterLengthGPU *>(centers.data()));
  graph.bufferWrite(
      rootBuffer,
      0u,
      sizeof(roots),
      const_cast<SparseVoxelBLASRootRangeGPU *>(roots.data()));

  uint32_t passIndex = 0u;
  {
    const svrRendering::SparseVoxelOccupancyMipChain::
        GPUSceneTraversal traversal{
          instanceBuffer,
          nodeBuffer,
          centerBuffer,
          rootBuffer,
          1u,
          static_cast<uint32_t>(nodes.size()),
          static_cast<uint32_t>(roots.size()),
          3u,
          static_cast<uint32_t>(nodes.size()),
        };
    svrRendering::SparseVoxelOccupancyMipChain mipChain(
        graph,
        passIndex,
        gpuVolume,
        {},
        static_cast<uint32_t>(nodes.size()),
        "g25.SceneTraversal",
        nullptr,
        &traversal);
    std::vector<Pass *> copyPasses;
    copyPasses.push_back(
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
            "g25.CopySceneTraversalBase",
            passIndex++,
            mipChain.baseWords(),
            0u,
            gpuVolume.layout().wordCount * sizeof(uint32_t),
            baseReadback,
            0u,
            gpuVolume.layout().wordCount * sizeof(uint32_t)));
    copyPasses.push_back(
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
            "g25.CopySceneTraversalMips",
            passIndex++,
            mipChain.mipWords(),
            0u,
            gpuVolume.pyramidLayout().wordCount *
                sizeof(uint32_t),
            mipReadback,
            0u,
            gpuVolume.pyramidLayout().wordCount *
                sizeof(uint32_t)));
    copyPasses.push_back(
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
            "g25.CopySceneTraversalDirtyCount",
            passIndex++,
            mipChain.dirtyWordCount(),
            0u,
            sizeof(uint32_t),
            dirtyCountReadback,
            0u,
            sizeof(uint32_t)));
    copyPasses.push_back(
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
            "g25.CopySceneTraversalMipDispatch",
            passIndex++,
            mipChain.dirtyDispatchArgs(),
            5u * sizeof(IndirectDispatchArgsGPU),
            gpuVolume.mipLevels().size() *
                sizeof(IndirectDispatchArgsGPU),
            mipDispatchReadback,
            0u,
            gpuVolume.mipLevels().size() *
                sizeof(IndirectDispatchArgsGPU)));
    graph.compile();

    const auto verify =
        [&](const uint32_t expectedDirtyWordCount,
            const bool expectMipWork)
    {
      RenderGraph::Frame frame{};
      RenderGraph::Overrides overrides{};
      graph.run(frame, overrides);
      graph.waitFrame(frame);
      std::vector<uint32_t> actualBase(
          gpuVolume.layout().wordCount);
      graph.bufferRead(
          baseReadback,
          0u,
          actualBase.size() * sizeof(uint32_t),
          [&](const void *data)
          {
            std::memcpy(
                actualBase.data(),
                data,
                actualBase.size() * sizeof(uint32_t));
          });
      assert(actualBase == oracle.words());
      std::vector<uint32_t> actualMips(
          gpuVolume.pyramidLayout().wordCount);
      graph.bufferRead(
          mipReadback,
          0u,
          actualMips.size() * sizeof(uint32_t),
          [&](const void *data)
          {
            std::memcpy(
                actualMips.data(),
                data,
                actualMips.size() * sizeof(uint32_t));
          });
      assert(actualMips == packedExpected(oracle));
      uint32_t actualDirtyWordCount = 0u;
      graph.bufferRead(
          dirtyCountReadback,
          0u,
          sizeof(actualDirtyWordCount),
          [&](const void *data)
          {
            std::memcpy(
                &actualDirtyWordCount,
                data,
                sizeof(actualDirtyWordCount));
          });
      assert(actualDirtyWordCount == expectedDirtyWordCount);
      std::vector<IndirectDispatchArgsGPU> mipDispatches(
          gpuVolume.mipLevels().size());
      graph.bufferRead(
          mipDispatchReadback,
          0u,
          mipDispatches.size() *
              sizeof(IndirectDispatchArgsGPU),
          [&](const void *data)
          {
            std::memcpy(
                mipDispatches.data(),
                data,
                mipDispatches.size() *
                    sizeof(IndirectDispatchArgsGPU));
          });
      for (const IndirectDispatchArgsGPU &dispatch :
           mipDispatches)
        assert((dispatch.x != 0u) == expectMipWork);
    };
    verify(0u, true);

    // Moving only along X recycles two slabs that are both outside the
    // instance bounds. Toroidal metadata advances, but mip 0 must perform
    // no dirty-word work.
    const std::array<float, 3> emptyEdgeCamera{
      1.25f, 0.25f, 0.25f};
    assert(gpuVolume.updateGPU(
        emptyEdgeCamera, 1u, {}, &error));
    assert(oracle.update(
        emptyEdgeCamera, 1u, sources, &error));
    assert(mipChain.updateGenerated(
        gpuVolume, 0u, {}, &error));
    verify(0u, false);

    const std::array<float, 3> scrolledCamera{
      1.25f, -0.75f, 0.25f};
    assert(gpuVolume.updateGPU(
        scrolledCamera, 1u, {}, &error));
    assert(oracle.update(
        scrolledCamera, 1u, sources, &error));
    assert(mipChain.updateGenerated(
        gpuVolume, 0u, {}, &error));
    verify(
        static_cast<uint32_t>(
            oracle.lastDirtyWords().size()),
        true);

    const std::array<float, 3> movedTranslation{
      4.0f, 0.5f, -0.5f};
    const std::vector<SparseVoxelAABB> movedSources =
        translatedCubeBounds(leafCenters, movedTranslation);
    const std::vector<SparseVoxelAABB> invalidations{
      enclosingBounds(sources),
      enclosingBounds(movedSources),
    };
    uploadInstance(movedTranslation);
    assert(gpuVolume.updateGPU(
        scrolledCamera, 2u, invalidations, &error));
    assert(oracle.update(
        scrolledCamera,
        2u,
        movedSources,
        invalidations,
        &error));
    assert(mipChain.updateGenerated(
        gpuVolume, 0u, invalidations, &error));
    verify(
        static_cast<uint32_t>(
            oracle.lastDirtyWords().size()),
        true);

    for (auto it = copyPasses.rbegin();
         it != copyPasses.rend();
         ++it)
      delete *it;
  }
  graph.deleteBuffer(mipDispatchReadback);
  graph.deleteBuffer(dirtyCountReadback);
  graph.deleteBuffer(mipReadback);
  graph.deleteBuffer(baseReadback);
  graph.deleteBuffer(rootBuffer);
  graph.deleteBuffer(centerBuffer);
  graph.deleteBuffer(nodeBuffer);
  graph.deleteBuffer(instanceBuffer);
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
  testFullAndIncrementalPyramid(rhi);
  testGPUGeneratedDirtyMasksAndSources(rhi);
  testGPUSceneTraversal(rhi);
  std::cout
      << "Sparse voxel G25/G27/G28 occupancy GPU tests passed\n";
  os::Logger::shutdown();
  return 0;
}
