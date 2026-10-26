#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64BatchBuildPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64ScenePass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelTree64SceneBuildChain final
{
public:
  struct Capacities
  {
    uint32_t side = 0u;
    uint32_t chunkCount = 0u;
    uint32_t nodeSlotCapacity = 0u;
    uint32_t nodeCount = 0u;
  };

  struct ResidentScene
  {
    ::rendering::Buffer instances;
    ::rendering::Buffer hierarchyNodes;
    ::rendering::Buffer centersLengths;
    ::rendering::Buffer blasRoots;
    uint32_t instanceCount = 0u;
    uint32_t hierarchyNodeCount = 0u;
    uint32_t blasCount = 0u;
    uint32_t traversalLevelCount = 0u;
    uint32_t queueCapacity = 0u;
  };

  SparseVoxelTree64SceneBuildChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const ::rendering::Buffer nodePool,
      const Capacities capacities,
      const ResidentScene scene,
      const std::array<float, 3> cellSize,
      std::string name = "SparseVoxelTree64SceneBuild")
      : graph_(&graph), nodePool_(nodePool), capacities_(capacities), scene_(scene), name_(std::move(name))
  {
    if (!nodePool_.isValid() || !sparseVoxelTree64SupportedSide(capacities_.side) || capacities_.chunkCount == 0u || capacities_.nodeSlotCapacity == 0u || capacities_.nodeCount == 0u ||
        scene_.traversalLevelCount == 0u || scene_.queueCapacity == 0u)
      throw std::invalid_argument("Tree64 scene build chain capacities are invalid");
    for (const float size : cellSize)
      if (!(size > 0.0f))
        throw std::invalid_argument("Tree64 scene build cell size is invalid");

    const uint32_t cellCount = capacities_.side * capacities_.side * capacities_.side;
    const uint32_t cellWordCount = (cellCount + 31u) / 32u;
    if (uint64_t{capacities_.chunkCount} * cellCount > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("Tree64 scene build cell capacity exceeds 32-bit addressing");
    const uint32_t terminalAxis = capacities_.side / 4u;
    const uint32_t terminalCount = terminalAxis * terminalAxis * terminalAxis;
    const uint32_t middleCount = capacities_.side == 64u ? 64u : 0u;

    jobs_ = createBuffer(".Jobs", uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64BuildJobGPU), true);
    dummySources_ = createBuffer(".UnusedSources", sizeof(SparseVoxelTree64BuildSourceGPU));
    cells_ = createBuffer(".Cells", uint64_t{capacities_.chunkCount} * cellWordCount * sizeof(uint32_t));
    terminalMasks_ = createBuffer(".TerminalMasks", uint64_t{capacities_.chunkCount} * terminalCount * sizeof(SparseVoxelTree64NodeGPU));
    middleMasks_ = createBuffer(".MiddleMasks", std::max<uint64_t>(sizeof(SparseVoxelTree64NodeGPU), uint64_t{capacities_.chunkCount} * middleCount * sizeof(SparseVoxelTree64NodeGPU)));
    rootMasks_ = createBuffer(".RootMasks", uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64NodeGPU));
    results_ = graph_->createBuffer({
      .name = name_ + ".Results",
      .size = uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64BuildResultGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push | ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    sceneQueueA_ = createBuffer(".SceneQueueA", uint64_t{scene_.queueCapacity} * sizeof(SparseVoxelLODQueueElementGPU));
    sceneQueueB_ = createBuffer(".SceneQueueB", uint64_t{scene_.queueCapacity} * sizeof(SparseVoxelLODQueueElementGPU));
    sceneQueueCounts_ = createBuffer(".SceneQueueCounts", uint64_t{scene_.traversalLevelCount} * sizeof(uint32_t));
    sceneDispatchArgs_ = graph_->createBuffer({
      .name = name_ + ".SceneDispatchArgs",
      .size = uint64_t{scene_.traversalLevelCount + 1u} * sizeof(IndirectDispatchArgsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Indirect,
    });
    sceneCounters_ = graph_->createBuffer({
      .name = name_ + ".SceneCounters",
      .size = sizeof(SparseVoxelTree64SceneCountersGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_CopySrc,
    });

    const SparseVoxelTree64BatchBuildPass::Buffers buildBuffers{
      jobs_,
      dummySources_,
      cells_,
      terminalMasks_,
      middleMasks_,
      rootMasks_,
      nodePool_,
      results_,
    };
    const SparseVoxelTree64BatchBuildPass::Capacities buildCaps{
      capacities_.side,
      capacities_.chunkCount,
      1u,
      capacities_.nodeSlotCapacity,
      capacities_.nodeCount,
    };
    buildPasses_[0] = graph_->registerPass<SparseVoxelTree64BatchBuildPass>(name_ + ".ClearCells", passIndex++, SparseVoxelTree64BatchBuildPass::Stage::ClearCells, buildBuffers, buildCaps);

    const SparseVoxelTree64ScenePass::Buffers sceneBuffers{
      jobs_,
      cells_,
      scene_.instances,
      scene_.hierarchyNodes,
      scene_.centersLengths,
      scene_.blasRoots,
      sceneQueueA_,
      sceneQueueB_,
      sceneQueueCounts_,
      sceneDispatchArgs_,
      sceneCounters_,
    };
    SparseVoxelTree64SceneTraversalGPU sceneSettings{
      scene_.instanceCount,
      scene_.hierarchyNodeCount,
      scene_.blasCount,
      scene_.traversalLevelCount,
      scene_.queueCapacity,
      0u,
      capacities_.side,
      cellCount,
      capacities_.chunkCount * cellCount,
      0u,
      cellWordCount,
      capacities_.chunkCount * cellWordCount,
      cellSize,
      0u,
    };
    const auto addScenePass = [&](const std::string &suffix, const SparseVoxelTree64ScenePass::Kernel kernel, const uint32_t depth)
    {
      SparseVoxelTree64SceneTraversalGPU settings = sceneSettings;
      settings.depth = depth;
      scenePasses_.push_back(graph_->registerPass<SparseVoxelTree64ScenePass>(name_ + suffix, passIndex++, kernel, sceneBuffers, settings));
    };
    addScenePass(".ClearScene", SparseVoxelTree64ScenePass::Kernel::Clear, 0u);
    addScenePass(".SeedScene", SparseVoxelTree64ScenePass::Kernel::Seed, 0u);
    addScenePass(".FinalizeRoots", SparseVoxelTree64ScenePass::Kernel::Finalize, 0u);
    for (uint32_t depth = 0u; depth < scene_.traversalLevelCount; ++depth)
    {
      addScenePass(".TraverseScene" + std::to_string(depth), SparseVoxelTree64ScenePass::Kernel::Traverse, depth);
      if (depth + 1u < scene_.traversalLevelCount)
        addScenePass(".FinalizeScene" + std::to_string(depth + 1u), SparseVoxelTree64ScenePass::Kernel::Finalize, depth + 1u);
    }
    addScenePass(".PrepareFailClosedScene", SparseVoxelTree64ScenePass::Kernel::PrepareFailClosed, 0u);
    addScenePass(".FailClosedScene", SparseVoxelTree64ScenePass::Kernel::FailClosed, 0u);

    constexpr std::array remainingStages{
      SparseVoxelTree64BatchBuildPass::Stage::TerminalMasks,
      SparseVoxelTree64BatchBuildPass::Stage::MiddleMasks,
      SparseVoxelTree64BatchBuildPass::Stage::RootMasks,
      SparseVoxelTree64BatchBuildPass::Stage::Compact,
    };
    for (uint32_t index = 0u; index < remainingStages.size(); ++index)
      buildPasses_[index + 1u] = graph_->registerPass<SparseVoxelTree64BatchBuildPass>(name_ + ".Build" + std::to_string(index), passIndex++, remainingStages[index], buildBuffers, buildCaps);
  }

  ~SparseVoxelTree64SceneBuildChain()
  {
    for (uint32_t index = static_cast<uint32_t>(buildPasses_.size()); index > 1u; --index)
      delete buildPasses_[index - 1u];
    for (auto iterator = scenePasses_.rbegin(); iterator != scenePasses_.rend(); ++iterator)
      delete *iterator;
    delete buildPasses_[0];
    graph_->deleteBuffer(sceneCounters_);
    graph_->deleteBuffer(sceneDispatchArgs_);
    graph_->deleteBuffer(sceneQueueCounts_);
    graph_->deleteBuffer(sceneQueueB_);
    graph_->deleteBuffer(sceneQueueA_);
    graph_->deleteBuffer(results_);
    graph_->deleteBuffer(rootMasks_);
    graph_->deleteBuffer(middleMasks_);
    graph_->deleteBuffer(terminalMasks_);
    graph_->deleteBuffer(cells_);
    graph_->deleteBuffer(dummySources_);
    graph_->deleteBuffer(jobs_);
  }

  bool update(const std::vector<SparseVoxelTree64BuildJobGPU> &jobs)
  {
    return update(jobs, scene_.instanceCount, scene_.hierarchyNodeCount, scene_.blasCount);
  }

  bool update(const std::vector<SparseVoxelTree64BuildJobGPU> &jobs, const uint32_t instanceCount, const uint32_t hierarchyNodeCount, const uint32_t blasCount)
  {
    if (jobs.size() > capacities_.chunkCount)
      return false;
    if (!jobs.empty())
      graph_->bufferWrite(jobs_, 0u, jobs.size() * sizeof(SparseVoxelTree64BuildJobGPU), const_cast<SparseVoxelTree64BuildJobGPU *>(jobs.data()));
    for (auto *pass : buildPasses_)
      if (!pass->update(static_cast<uint32_t>(jobs.size()), 0u))
        return false;
    for (auto *pass : scenePasses_)
      if (!pass->update(static_cast<uint32_t>(jobs.size()), instanceCount, hierarchyNodeCount, blasCount))
        return false;
    return true;
  }

  ::rendering::Buffer results() const
  {
    return results_;
  }
  ::rendering::Buffer nodePool() const
  {
    return nodePool_;
  }
  ::rendering::Buffer sceneCounters() const
  {
    return sceneCounters_;
  }
  uint64_t scratchBytes() const
  {
    const std::array buffers{
      jobs_,
      dummySources_,
      cells_,
      terminalMasks_,
      middleMasks_,
      rootMasks_,
      results_,
      sceneQueueA_,
      sceneQueueB_,
      sceneQueueCounts_,
      sceneDispatchArgs_,
      sceneCounters_,
    };
    uint64_t bytes = 0u;
    for (const auto buffer : buffers)
      bytes += graph_->getBufferSize(buffer);
    return bytes;
  }

private:
  ::rendering::Buffer createBuffer(const std::string &suffix, const uint64_t size, const bool frameLocal = false)
  {
    return graph_->createBuffer({
      .name = name_ + suffix,
      .size = size,
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = frameLocal,
    });
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  ::rendering::Buffer nodePool_{};
  Capacities capacities_{};
  ResidentScene scene_{};
  std::string name_;
  ::rendering::Buffer jobs_{};
  ::rendering::Buffer dummySources_{};
  ::rendering::Buffer cells_{};
  ::rendering::Buffer terminalMasks_{};
  ::rendering::Buffer middleMasks_{};
  ::rendering::Buffer rootMasks_{};
  ::rendering::Buffer results_{};
  ::rendering::Buffer sceneQueueA_{};
  ::rendering::Buffer sceneQueueB_{};
  ::rendering::Buffer sceneQueueCounts_{};
  ::rendering::Buffer sceneDispatchArgs_{};
  ::rendering::Buffer sceneCounters_{};
  std::array<SparseVoxelTree64BatchBuildPass *, 5u> buildPasses_{};
  std::vector<SparseVoxelTree64ScenePass *> scenePasses_;
};

} // namespace voxel::svraster::rendering
