#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2RasterHierarchyPass.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class TerrainGPUV2RasterHierarchyChain final
{
public:
  struct LeafBuffers
  {
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    ::rendering::Buffer materialWords;
    ::rendering::Buffer result;
    ::rendering::Buffer heightSamples;
  };

  TerrainGPUV2RasterHierarchyChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const LeafBuffers leaves,
      const TerrainGPUV2RasterHierarchyConfigGPU capacity,
      const uint32_t workgroup,
      const uint32_t lanes,
      std::string name = "TerrainGPUV2.RasterHierarchy")
      : graph_(&graph), leaves_(leaves), capacity_(capacity), workgroup_(workgroup), lanes_(lanes), name_(std::move(name))
  {
    if (!leaves.nodes.isValid() || !leaves.occupancy.isValid() || !leaves.materialWords.isValid() || !leaves.result.isValid() || lanes == 0u)
      throw std::invalid_argument("terrain GPU V2 raster hierarchy leaf buffers are invalid");
    createBuffers();
    if (!leaves_.heightSamples.isValid())
    {
      const uint32_t halo = capacity_.reserved & 0xffffu;
      const uint32_t stride = capacity_.reserved >> 16u;
      const uint64_t heightSampleCount = std::max<uint64_t>(1u,
          uint64_t{stride} *
              (uint64_t{capacity_.bricksZ} * capacity_.brickSide +
                  2u * halo + 1u));
      dummyHeightSamples_ = graph_->createBuffer(
          {.name = name_ + ".DummyHeightSamples",
           .size = uint64_t{lanes_} * heightSampleCount *
               sizeof(TerrainGPUV2HeightSampleGPU),
           .usage = ::rendering::BufferUsage::BufferUsage_Storage});
    }
    const ::rendering::Buffer heightSamples =
        leaves_.heightSamples.isValid() ? leaves_.heightSamples
                                        : dummyHeightSamples_;
    const TerrainGPUV2RasterHierarchyPass::Buffers buffers{
      config_,
      leaves.nodes,
      leaves.occupancy,
      leaves.materialWords,
      leaves.result,
      scratch_,
      nodes_,
      occupancy_,
      materialWords_,
      childIndices_,
      rootIndices_,
      result_,
      indirectArgs_,
      heightSamples};
    auto add = [&](const TerrainGPUV2RasterHierarchyPass::Stage stage, const uint32_t level = 0u)
    {
      const uint32_t slot = static_cast<uint32_t>(passes_.size());
      stages_.push_back(stage);
      levels_.push_back(level);
      passes_.push_back(graph_->registerPass<TerrainGPUV2RasterHierarchyPass>(name_ + ".Stage" + std::to_string(slot), passIndex++, stage, level, slot, capacity_, workgroup_, lanes_, buffers));
    };
    add(TerrainGPUV2RasterHierarchyPass::Stage::Reset);
    add(TerrainGPUV2RasterHierarchyPass::Stage::ClearMappings);
    add(TerrainGPUV2RasterHierarchyPass::Stage::ClearMaterials);
    add(TerrainGPUV2RasterHierarchyPass::Stage::CopyLeaves);
    add(TerrainGPUV2RasterHierarchyPass::Stage::CopyLeafMaterials);
    for (uint32_t level = 1u; level < capacity_.levelCount; ++level)
    {
      add(TerrainGPUV2RasterHierarchyPass::Stage::Classify, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::Reduce, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::ScanLevel0, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::ScanLevel1, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::ScanLevel2, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::AddLevel1, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::AddLevel0, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::Reserve, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::EmitNodes, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::EmitWords, level);
      add(TerrainGPUV2RasterHierarchyPass::Stage::Link, level);
    }
    add(TerrainGPUV2RasterHierarchyPass::Stage::EmitRoots);
    add(TerrainGPUV2RasterHierarchyPass::Stage::Validate);
    add(TerrainGPUV2RasterHierarchyPass::Stage::Finalize);
  }

  TerrainGPUV2RasterHierarchyChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const LeafBuffers leaves,
      const TerrainGPUV2RasterHierarchyConfigGPU capacity,
      const uint32_t workgroup,
      std::string name = "TerrainGPUV2.RasterHierarchy")
      : TerrainGPUV2RasterHierarchyChain(graph, passIndex, leaves, capacity, workgroup, 1u, std::move(name))
  {
  }

  ~TerrainGPUV2RasterHierarchyChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(result_);
    if (dummyHeightSamples_.isValid())
      graph_->deleteBuffer(dummyHeightSamples_);
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(rootIndices_);
    graph_->deleteBuffer(childIndices_);
    graph_->deleteBuffer(materialWords_);
    graph_->deleteBuffer(occupancy_);
    graph_->deleteBuffer(nodes_);
    graph_->deleteBuffer(scratch_);
    graph_->deleteBuffer(config_);
  }

  bool update(const TerrainGPUV2RasterHierarchyConfigGPU &config)
  {
    return updateBatch(std::vector<TerrainGPUV2RasterHierarchyConfigGPU>{config});
  }

  bool updateBatch(const std::vector<TerrainGPUV2RasterHierarchyConfigGPU> &configs)
  {
    if (configs.empty() || configs.size() > lanes_)
      return false;
    std::vector<TerrainGPUV2RasterHierarchyConfigGPU> uploaded;
    uploaded.reserve(configs.size());
    for (auto config : configs)
    {
      if (config.levelCount != capacity_.levelCount || config.totalDenseCandidates != capacity_.totalDenseCandidates || config.maximumParentCandidates != capacity_.maximumParentCandidates ||
          config.levelDenseOffsets != capacity_.levelDenseOffsets || config.levelDenseCounts != capacity_.levelDenseCounts || config.levelAxesX != capacity_.levelAxesX || config.levelAxesY != capacity_.levelAxesY ||
          config.levelAxesZ != capacity_.levelAxesZ || config.leafNodeCapacity > capacity_.leafNodeCapacity || config.leafMaterialWordCapacity > capacity_.leafMaterialWordCapacity ||
          config.nodeCapacity > capacity_.nodeCapacity || config.materialWordCapacity > capacity_.materialWordCapacity || config.childCapacity > capacity_.childCapacity || config.rootCapacity > capacity_.rootCapacity)
        return false;
      // The shader uses these immutable capacities as lane strides. Per-page
      // counts still come from the fine-raster result and hierarchy result.
      config.leafNodeCapacity = capacity_.leafNodeCapacity;
      config.leafMaterialCapacity = capacity_.leafMaterialCapacity;
      config.leafMaterialWordCapacity = capacity_.leafMaterialWordCapacity;
      config.nodeCapacity = capacity_.nodeCapacity;
      config.materialCapacity = capacity_.materialCapacity;
      config.materialWordCapacity = capacity_.materialWordCapacity;
      config.childCapacity = capacity_.childCapacity;
      config.rootCapacity = capacity_.rootCapacity;
      uploaded.push_back(config);
    }
    graph_->bufferWrite(config_, 0u, uploaded.size() * sizeof(uploaded[0]), uploaded.data());
    std::vector<IndirectDispatchArgsGPU> indirect(passes_.size(), {1u, 1u, 1u, 0u});
    const auto groups = [&](const uint64_t count)
    {
      return static_cast<uint32_t>((count + workgroup_ - 1u) / workgroup_);
    };
    for (size_t index = 0u; index < indirect.size(); ++index)
    {
      const uint32_t level = levels_[index];
      uint32_t candidates = 0u;
      for (const auto &config : configs)
        candidates = std::max(candidates, level < config.levelCount ? config.levelDenseCounts[level] : 0u);
      const uint32_t level0 = (candidates + 255u) / 256u;
      const uint32_t level1 = (level0 + 255u) / 256u;
      switch (stages_[index])
      {
      case TerrainGPUV2RasterHierarchyPass::Stage::ClearMappings:
        indirect[index].x = groups(capacity_.totalDenseCandidates);
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::ClearMaterials:
        indirect[index].x = groups(capacity_.materialWordCapacity);
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::CopyLeaves:
        indirect[index].x = groups(capacity_.leafNodeCapacity);
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::CopyLeafMaterials:
        indirect[index].x = groups(capacity_.leafMaterialWordCapacity);
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::Classify:
      case TerrainGPUV2RasterHierarchyPass::Stage::EmitWords:
        indirect[index].x = groups(uint64_t{candidates} * kTerrainGPUV2FineRasterWordsPerBrick);
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::Reduce:
      case TerrainGPUV2RasterHierarchyPass::Stage::EmitNodes:
      case TerrainGPUV2RasterHierarchyPass::Stage::Link:
        indirect[index].x = groups(candidates);
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::ScanLevel0:
        indirect[index].x = level0;
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::ScanLevel1:
        // A single S0 block has a zero block prefix, so launching 256 threads
        // and walking 18 barriers to scan its sole sum is pure overhead.
        indirect[index].x = level0 > 1u ? level1 : 0u;
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::ScanLevel2:
        indirect[index].x = level1 > 1u ? (level1 + 255u) / 256u : 0u;
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::AddLevel1:
        indirect[index].x = level1 > 1u ? level1 : 0u;
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::AddLevel0:
        indirect[index].x = level0 > 1u ? level0 : 0u;
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::EmitRoots:
        indirect[index].x = groups(capacity_.levelDenseCounts[capacity_.levelCount - 1u]);
        break;
      case TerrainGPUV2RasterHierarchyPass::Stage::Validate:
        indirect[index].x = groups(capacity_.nodeCapacity);
        break;
      default:
        break;
      }
      indirect[index].z = static_cast<uint32_t>(configs.size());
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
    auto idle = indirectCPU_;
    if (!enabled_)
      for (auto &args : idle)
        args = {0u, 1u, 1u, 0u};
    graph_->bufferWrite(indirectArgs_, 0u, idle.size() * sizeof(idle[0]), idle.data());
  }

  ::rendering::Buffer nodes() const
  {
    return nodes_;
  }
  ::rendering::Buffer occupancy() const
  {
    return occupancy_;
  }
  ::rendering::Buffer materialWords() const
  {
    return materialWords_;
  }
  ::rendering::Buffer childIndices() const
  {
    return childIndices_;
  }
  ::rendering::Buffer rootIndices() const
  {
    return rootIndices_;
  }
  ::rendering::Buffer result() const
  {
    return result_;
  }
  ::rendering::Buffer indirectArgs() const
  {
    return indirectArgs_;
  }
  uint32_t passCount() const
  {
    return static_cast<uint32_t>(passes_.size());
  }

  // Exposes the compact indirect plan for budgeting, telemetry, and tests.
  // This is CPU metadata only; no generated terrain payload crosses back from
  // the GPU.
  const std::vector<IndirectDispatchArgsGPU> &indirectDispatchPlan() const
  {
    return indirectCPU_;
  }

  uint64_t scratchBytes() const
  {
    TerrainGPUV2RasterHierarchyScratchLayout scratch{};
    return terrainGPUV2RasterHierarchyScratchLayout(capacity_.maximumParentCandidates, capacity_.totalDenseCandidates, capacity_.levelCount, scratch) ? uint64_t{lanes_} * scratch.totalWords * sizeof(uint32_t) : 0u;
  }

  uint64_t outputBytes() const
  {
    return uint64_t{lanes_} * (uint64_t{capacity_.nodeCapacity} * (sizeof(SparseVoxelBrickNodeGPU) + sizeof(SparseVoxelBrickOccupancyGPU)) +
                               uint64_t{capacity_.materialWordCapacity + capacity_.childCapacity + capacity_.rootCapacity} * sizeof(uint32_t) + sizeof(TerrainGPUV2RasterHierarchyResultGPU));
  }

  uint64_t microGeometryBytes() const
  {
    return 0u;
  }

private:
  ::rendering::Buffer storage(const std::string &suffix, const uint64_t size, const ::rendering::BufferUsage extra = ::rendering::BufferUsage::BufferUsage_None)
  {
    return graph_->createBuffer({.name = name_ + suffix, .size = size, .usage = ::rendering::BufferUsage::BufferUsage_Storage | extra});
  }

  void createBuffers()
  {
    TerrainGPUV2RasterHierarchyScratchLayout scratch{};
    std::string error;
    if (!terrainGPUV2RasterHierarchyScratchLayout(capacity_.maximumParentCandidates, capacity_.totalDenseCandidates, capacity_.levelCount, scratch, &error))
      throw std::invalid_argument(error);
    const auto copy = ::rendering::BufferUsage::BufferUsage_CopySrc;
    config_ = storage(".Config", uint64_t{lanes_} * sizeof(TerrainGPUV2RasterHierarchyConfigGPU), ::rendering::BufferUsage::BufferUsage_Push);
    scratch_ = storage(".Scratch", uint64_t{lanes_} * scratch.totalWords * sizeof(uint32_t));
    nodes_ = storage(".Nodes", uint64_t{lanes_} * capacity_.nodeCapacity * sizeof(SparseVoxelBrickNodeGPU), copy);
    occupancy_ = storage(".Occupancy", uint64_t{lanes_} * capacity_.nodeCapacity * sizeof(SparseVoxelBrickOccupancyGPU), copy);
    materialWords_ = storage(".MaterialWords", uint64_t{lanes_} * capacity_.materialWordCapacity * sizeof(uint32_t), copy);
    childIndices_ = storage(".ChildIndices", uint64_t{lanes_} * capacity_.childCapacity * sizeof(uint32_t), copy);
    rootIndices_ = storage(".Roots", uint64_t{lanes_} * capacity_.rootCapacity * sizeof(uint32_t), copy);
    result_ = storage(".Result", uint64_t{lanes_} * sizeof(TerrainGPUV2RasterHierarchyResultGPU), copy);
    indirectArgs_ = graph_->createBuffer(
        {.name = name_ + ".IndirectArgs",
         .size = passCapacity() * sizeof(IndirectDispatchArgsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    std::vector<IndirectDispatchArgsGPU> idle(passCapacity(), {0u, 1u, 1u, 0u});
    graph_->bufferWrite(indirectArgs_, 0u, idle.size() * sizeof(idle[0]), idle.data());
  }

  uint32_t passCapacity() const
  {
    return 8u + 11u * (capacity_.levelCount - 1u);
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  LeafBuffers leaves_{};
  TerrainGPUV2RasterHierarchyConfigGPU capacity_{};
  uint32_t workgroup_ = 64u;
  uint32_t lanes_ = 1u;
  std::string name_{};
  std::vector<TerrainGPUV2RasterHierarchyPass *> passes_{};
  std::vector<TerrainGPUV2RasterHierarchyPass::Stage> stages_{};
  std::vector<uint32_t> levels_{};
  ::rendering::Buffer config_{};
  ::rendering::Buffer scratch_{};
  ::rendering::Buffer nodes_{};
  ::rendering::Buffer occupancy_{};
  ::rendering::Buffer materialWords_{};
  ::rendering::Buffer childIndices_{};
  ::rendering::Buffer rootIndices_{};
  ::rendering::Buffer result_{};
  ::rendering::Buffer indirectArgs_{};
  ::rendering::Buffer dummyHeightSamples_{};
  std::vector<IndirectDispatchArgsGPU> indirectCPU_{};
  bool enabled_ = true;
};

} // namespace voxel::svraster::rendering
