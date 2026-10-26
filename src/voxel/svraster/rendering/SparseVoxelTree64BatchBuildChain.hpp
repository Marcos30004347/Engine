#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/rendering/SparseVoxelTree64BatchBuildPass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelTree64BatchBuildChain final
{
public:
  struct Capacities
  {
    uint32_t side = 0u;
    uint32_t chunkCount = 0u;
    uint32_t sourceCount = 0u;
    uint32_t nodeSlotCapacity = 0u;
    uint32_t nodeCount = 0u;
  };

  SparseVoxelTree64BatchBuildChain(::rendering::RenderGraph &graph, uint32_t &passIndex, const ::rendering::Buffer nodePool, const Capacities capacities, std::string name = "SparseVoxelTree64Build")
      : graph_(&graph), nodePool_(nodePool), capacities_(capacities), name_(std::move(name))
  {
    if (!nodePool_.isValid() || !sparseVoxelTree64SupportedSide(capacities_.side) || capacities_.chunkCount == 0u || capacities_.nodeSlotCapacity == 0u || capacities_.nodeCount == 0u)
      throw std::invalid_argument("Tree64 batch build chain capacities are invalid");
    capacities_.sourceCount = std::max(1u, capacities_.sourceCount);
    const uint32_t cellCount = capacities_.side * capacities_.side * capacities_.side;
    const uint32_t cellWordCount = (cellCount + 31u) / 32u;
    const uint32_t terminalAxis = capacities_.side / 4u;
    const uint32_t terminalCount = terminalAxis * terminalAxis * terminalAxis;
    const uint32_t middleCount = capacities_.side == 64u ? 64u : 0u;

    jobs_ = createBuffer(".Jobs", uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64BuildJobGPU));
    sources_ = createBuffer(".Sources", uint64_t{capacities_.sourceCount} * sizeof(SparseVoxelTree64BuildSourceGPU));
    cells_ = createBuffer(".Cells", uint64_t{capacities_.chunkCount} * cellWordCount * sizeof(uint32_t));
    terminalMasks_ = createBuffer(".TerminalMasks", uint64_t{capacities_.chunkCount} * terminalCount * sizeof(SparseVoxelTree64NodeGPU));
    middleMasks_ = createBuffer(".MiddleMasks", std::max<uint64_t>(sizeof(SparseVoxelTree64NodeGPU), uint64_t{capacities_.chunkCount} * middleCount * sizeof(SparseVoxelTree64NodeGPU)));
    rootMasks_ = createBuffer(".RootMasks", uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64NodeGPU));
    results_ = graph_->createBuffer({
      .name = name_ + ".Results",
      .size = uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64BuildResultGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push | ::rendering::BufferUsage::BufferUsage_CopySrc,
    });

    const SparseVoxelTree64BatchBuildPass::Buffers buffers{
      jobs_,
      sources_,
      cells_,
      terminalMasks_,
      middleMasks_,
      rootMasks_,
      nodePool_,
      results_,
    };
    const SparseVoxelTree64BatchBuildPass::Capacities passCaps{
      capacities_.side,
      capacities_.chunkCount,
      capacities_.sourceCount,
      capacities_.nodeSlotCapacity,
      capacities_.nodeCount,
    };
    constexpr std::array stages{
      SparseVoxelTree64BatchBuildPass::Stage::ClearCells,
      SparseVoxelTree64BatchBuildPass::Stage::SplatSources,
      SparseVoxelTree64BatchBuildPass::Stage::TerminalMasks,
      SparseVoxelTree64BatchBuildPass::Stage::MiddleMasks,
      SparseVoxelTree64BatchBuildPass::Stage::RootMasks,
      SparseVoxelTree64BatchBuildPass::Stage::Compact,
    };
    for (uint32_t stageIndex = 0u; stageIndex < stages.size(); ++stageIndex)
      passes_[stageIndex] = graph_->registerPass<SparseVoxelTree64BatchBuildPass>(name_ + ".Stage" + std::to_string(stageIndex), passIndex++, stages[stageIndex], buffers, passCaps);
  }

  ~SparseVoxelTree64BatchBuildChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(results_);
    graph_->deleteBuffer(rootMasks_);
    graph_->deleteBuffer(middleMasks_);
    graph_->deleteBuffer(terminalMasks_);
    graph_->deleteBuffer(cells_);
    graph_->deleteBuffer(sources_);
    graph_->deleteBuffer(jobs_);
  }

  bool update(const std::vector<SparseVoxelTree64BuildJobGPU> &jobs, const std::vector<SparseVoxelTree64BuildSourceGPU> &sources)
  {
    if (jobs.size() > capacities_.chunkCount || sources.size() > capacities_.sourceCount)
      return false;
    for (const auto &source : sources)
      if (source.jobIndex >= jobs.size())
        return false;
    if (!jobs.empty())
      graph_->bufferWrite(jobs_, 0u, jobs.size() * sizeof(SparseVoxelTree64BuildJobGPU), const_cast<SparseVoxelTree64BuildJobGPU *>(jobs.data()));
    if (!sources.empty())
      graph_->bufferWrite(sources_, 0u, sources.size() * sizeof(SparseVoxelTree64BuildSourceGPU), const_cast<SparseVoxelTree64BuildSourceGPU *>(sources.data()));
    for (auto *pass : passes_)
      if (!pass->update(static_cast<uint32_t>(jobs.size()), static_cast<uint32_t>(sources.size())))
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

private:
  ::rendering::Buffer createBuffer(const std::string &suffix, const uint64_t size)
  {
    return graph_->createBuffer({
      .name = name_ + suffix,
      .size = size,
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
    });
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  ::rendering::Buffer nodePool_{};
  Capacities capacities_{};
  std::string name_;
  ::rendering::Buffer jobs_{};
  ::rendering::Buffer sources_{};
  ::rendering::Buffer cells_{};
  ::rendering::Buffer terminalMasks_{};
  ::rendering::Buffer middleMasks_{};
  ::rendering::Buffer rootMasks_{};
  ::rendering::Buffer results_{};
  std::array<SparseVoxelTree64BatchBuildPass *, 6u> passes_{};
};

} // namespace voxel::svraster::rendering
