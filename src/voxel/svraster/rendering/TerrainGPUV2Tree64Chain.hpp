#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2Tree64Pass.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class TerrainGPUV2Tree64Chain final
{
public:
  struct RasterBuffers
  {
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    ::rendering::Buffer result;
  };

  TerrainGPUV2Tree64Chain(::rendering::RenderGraph &graph, uint32_t &passIndex, const RasterBuffers raster, const TerrainGPUV2Tree64ConfigGPU capacity, const uint32_t workgroup, const uint32_t lanes, std::string name = "TerrainGPUV2.Tree64")
      : graph_(&graph), raster_(raster), capacity_(capacity), workgroup_(workgroup), lanes_(lanes), name_(std::move(name))
  {
    if (!raster.nodes.isValid() || !raster.occupancy.isValid() || !raster.result.isValid() || lanes_ == 0u)
      throw std::invalid_argument("terrain GPU V2 Tree64 raster buffers are invalid");
    createBuffers();
    const TerrainGPUV2Tree64Pass::Buffers buffers{config_, raster.nodes, raster.occupancy, raster.result, masks_, scratch_, nodes_, result_, indirectArgs_};
    auto add = [&](const TerrainGPUV2Tree64Pass::Stage stage, const uint32_t level = 0u)
    {
      const uint32_t slot = static_cast<uint32_t>(passes_.size());
      stages_.push_back(stage);
      levels_.push_back(level);
      passes_.push_back(graph_->registerPass<TerrainGPUV2Tree64Pass>(name_ + ".Stage" + std::to_string(slot), passIndex++, stage, level, slot, capacity_, workgroup_, lanes_, buffers));
    };
    add(TerrainGPUV2Tree64Pass::Stage::Reset);
    add(TerrainGPUV2Tree64Pass::Stage::ClearMasks);
    add(TerrainGPUV2Tree64Pass::Stage::TerminalMasks);
    for (uint32_t level = 1u; level < capacity_.levelCount; ++level)
      add(TerrainGPUV2Tree64Pass::Stage::UpperMask, level);
    add(TerrainGPUV2Tree64Pass::Stage::Flags);
    add(TerrainGPUV2Tree64Pass::Stage::ScanLevel0);
    add(TerrainGPUV2Tree64Pass::Stage::ScanLevel1);
    add(TerrainGPUV2Tree64Pass::Stage::ScanLevel2);
    add(TerrainGPUV2Tree64Pass::Stage::AddLevel1);
    add(TerrainGPUV2Tree64Pass::Stage::AddLevel0);
    add(TerrainGPUV2Tree64Pass::Stage::ValidateCapacity);
    add(TerrainGPUV2Tree64Pass::Stage::EmitNodes);
    for (uint32_t level = capacity_.levelCount; level-- > 1u;)
      add(TerrainGPUV2Tree64Pass::Stage::Link, level);
    add(TerrainGPUV2Tree64Pass::Stage::ValidateNodes);
    add(TerrainGPUV2Tree64Pass::Stage::Finalize);
  }

  TerrainGPUV2Tree64Chain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const RasterBuffers raster,
      const TerrainGPUV2Tree64ConfigGPU capacity,
      const uint32_t workgroup,
      std::string name = "TerrainGPUV2.Tree64")
      : TerrainGPUV2Tree64Chain(graph, passIndex, raster, capacity, workgroup, 1u, std::move(name))
  {
  }

  ~TerrainGPUV2Tree64Chain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(result_);
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(nodes_);
    graph_->deleteBuffer(scratch_);
    graph_->deleteBuffer(masks_);
    graph_->deleteBuffer(config_);
  }

  bool update(const TerrainGPUV2Tree64ConfigGPU &config)
  {
    return updateBatch(std::vector<TerrainGPUV2Tree64ConfigGPU>{config});
  }

  bool updateBatch(const std::vector<TerrainGPUV2Tree64ConfigGPU> &configs)
  {
    if (configs.empty() || configs.size() > lanes_)
      return false;
    TerrainGPUV2Tree64ScratchLayout scratchLayout{};
    if (!terrainGPUV2Tree64ScratchLayout(capacity_.candidateCount, scratchLayout))
      return false;
    std::vector<TerrainGPUV2Tree64ConfigGPU> upload = configs;
    for (auto &config : upload)
    {
      if (config.modelSide != capacity_.modelSide || config.levelCount != capacity_.levelCount || config.candidateCount != capacity_.candidateCount || config.nodeCapacity > capacity_.nodeCapacity ||
          config.rasterNodeCapacity > capacity_.rasterNodeCapacity || config.voxelsX > capacity_.voxelsX || config.voxelsY > capacity_.voxelsY || config.voxelsZ > capacity_.voxelsZ ||
          config.levelOffsets != capacity_.levelOffsets || config.levelCounts != capacity_.levelCounts)
        return false;
      config.reserved[0] = capacity_.rasterNodeCapacity;
      config.reserved[1] = capacity_.candidateCount;
      config.reserved[2] = scratchLayout.totalWords;
      config.reserved[3] = capacity_.nodeCapacity;
    }
    graph_->bufferWrite(config_, 0u, upload.size() * sizeof(upload[0]), upload.data());
    std::vector<IndirectDispatchArgsGPU> indirect(passes_.size(), {1u, 1u, static_cast<uint32_t>(configs.size()), 0u});
    const auto groups = [&](const uint64_t count)
    {
      return static_cast<uint32_t>((count + workgroup_ - 1u) / workgroup_);
    };
    for (const auto &config : configs)
    {
      const uint32_t level0 = (config.candidateCount + 255u) / 256u;
      const uint32_t level1 = (level0 + 255u) / 256u;
      for (size_t index = 0u; index < indirect.size(); ++index)
      {
        uint32_t groupsForStage = 1u;
        switch (stages_[index])
        {
        case TerrainGPUV2Tree64Pass::Stage::ClearMasks:
        case TerrainGPUV2Tree64Pass::Stage::Flags:
        case TerrainGPUV2Tree64Pass::Stage::EmitNodes:
        case TerrainGPUV2Tree64Pass::Stage::ValidateNodes:
          groupsForStage = groups(config.candidateCount);
          break;
        case TerrainGPUV2Tree64Pass::Stage::TerminalMasks:
          groupsForStage = groups(uint64_t{config.rasterNodeCapacity} * kTerrainGPUV2FineRasterWordsPerBrick);
          break;
        case TerrainGPUV2Tree64Pass::Stage::UpperMask:
        case TerrainGPUV2Tree64Pass::Stage::Link:
          groupsForStage = groups(config.levelCounts[levels_[index]]);
          break;
        case TerrainGPUV2Tree64Pass::Stage::ScanLevel0:
        case TerrainGPUV2Tree64Pass::Stage::AddLevel0:
          groupsForStage = level0;
          break;
        case TerrainGPUV2Tree64Pass::Stage::ScanLevel1:
        case TerrainGPUV2Tree64Pass::Stage::AddLevel1:
          groupsForStage = level1;
          break;
        default:
          break;
        }
        indirect[index].x = std::max(indirect[index].x, groupsForStage);
      }
    }
    indirectCPU_ = indirect;
    if (enabled_)
      graph_->bufferWrite(indirectArgs_, 0u, indirectCPU_.size() * sizeof(indirectCPU_[0]), indirectCPU_.data());
    return true;
  }

  void setEnabled(const bool enabled)
  {
    enabled_ = enabled;
    for (auto *pass : passes_)
      pass->setEnabled(enabled);
    auto indirect = indirectCPU_;
    if (!enabled_)
      for (auto &args : indirect)
        args = {0u, 1u, 1u, 0u};
    graph_->bufferWrite(indirectArgs_, 0u, indirect.size() * sizeof(indirect[0]), indirect.data());
  }

  ::rendering::Buffer nodes() const
  {
    return nodes_;
  }
  ::rendering::Buffer result() const
  {
    return result_;
  }
  ::rendering::Buffer indirectArgs() const
  {
    return indirectArgs_;
  }
  const std::vector<IndirectDispatchArgsGPU> &indirectDispatchPlan() const
  {
    return indirectCPU_;
  }
  uint32_t passCount() const
  {
    return static_cast<uint32_t>(passes_.size());
  }

  uint64_t maskBytes() const
  {
    return uint64_t{lanes_} * capacity_.candidateCount * sizeof(uint64_t);
  }
  uint64_t scratchBytes() const
  {
    TerrainGPUV2Tree64ScratchLayout scratch{};
    return terrainGPUV2Tree64ScratchLayout(capacity_.candidateCount, scratch)
        ? uint64_t{lanes_} * scratch.totalWords * sizeof(uint32_t) : 0u;
  }
  uint64_t outputBytes() const
  {
    return uint64_t{lanes_} *
        (uint64_t{capacity_.nodeCapacity} * sizeof(SparseVoxelTree64NodeGPU) +
         sizeof(TerrainGPUV2Tree64ResultGPU));
  }

private:
  ::rendering::Buffer storage(const std::string &suffix, const uint64_t size, const ::rendering::BufferUsage extra = ::rendering::BufferUsage::BufferUsage_None)
  {
    return graph_->createBuffer({.name = name_ + suffix, .size = size, .usage = ::rendering::BufferUsage::BufferUsage_Storage | extra});
  }

  void createBuffers()
  {
    TerrainGPUV2Tree64ScratchLayout scratch{};
    std::string error;
    if (!terrainGPUV2Tree64ScratchLayout(capacity_.candidateCount, scratch, &error))
      throw std::invalid_argument(error);
    const auto copy = ::rendering::BufferUsage::BufferUsage_CopySrc;
    const uint64_t lanes = lanes_;
    config_ = storage(".Config", lanes * sizeof(TerrainGPUV2Tree64ConfigGPU), ::rendering::BufferUsage::BufferUsage_Push);
    masks_ = storage(".Masks", lanes * capacity_.candidateCount * sizeof(uint64_t));
    scratch_ = storage(".Scratch", lanes * scratch.totalWords * sizeof(uint32_t));
    nodes_ = storage(".Nodes", lanes * capacity_.nodeCapacity * sizeof(SparseVoxelTree64NodeGPU), copy);
    result_ = storage(".Result", lanes * sizeof(TerrainGPUV2Tree64ResultGPU), copy);
    indirectArgs_ = graph_->createBuffer(
        {.name = name_ + ".IndirectArgs",
         .size = passesCapacity() * sizeof(IndirectDispatchArgsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    std::vector<IndirectDispatchArgsGPU> idle(passesCapacity(), {0u, 1u, 1u, 0u});
    graph_->bufferWrite(indirectArgs_, 0u, idle.size() * sizeof(idle[0]), idle.data());
  }

  uint32_t passesCapacity() const
  {
    // reset + clear + terminal + upper levels + flags + six scan/add stages
    // + capacity validation + emit + links + validation + finalize.
    return 13u + 2u * (capacity_.levelCount - 1u);
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  RasterBuffers raster_{};
  TerrainGPUV2Tree64ConfigGPU capacity_{};
  uint32_t workgroup_ = 64u;
  uint32_t lanes_ = 1u;
  std::string name_{};
  std::vector<TerrainGPUV2Tree64Pass *> passes_{};
  std::vector<TerrainGPUV2Tree64Pass::Stage> stages_{};
  std::vector<uint32_t> levels_{};
  ::rendering::Buffer config_{};
  ::rendering::Buffer masks_{};
  ::rendering::Buffer scratch_{};
  ::rendering::Buffer nodes_{};
  ::rendering::Buffer result_{};
  ::rendering::Buffer indirectArgs_{};
  std::vector<IndirectDispatchArgsGPU> indirectCPU_{};
  bool enabled_ = true;
};

} // namespace voxel::svraster::rendering
