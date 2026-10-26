#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2FineRasterPass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class TerrainGPUV2FineRasterChain final
{
public:
  static constexpr uint32_t kPassCount = 13u;

  struct ColumnBuffers
  {
    ::rendering::Buffer columns;
    ::rendering::Buffer result;
  };

  TerrainGPUV2FineRasterChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const ColumnBuffers columnBuffers,
      const TerrainGPUV2FineRasterConfigGPU capacity,
      const uint32_t workgroup,
      const uint32_t lanes,
      std::string name = "TerrainGPUV2.FineRaster")
      : graph_(&graph), columnBuffers_(columnBuffers), capacity_(capacity), workgroup_(workgroup), lanes_(lanes), name_(std::move(name))
  {
    if (!columnBuffers.columns.isValid() || !columnBuffers.result.isValid() || lanes_ == 0u)
      throw std::invalid_argument("terrain GPU V2 fine raster column buffers are invalid");
    createBuffers();
    const TerrainGPUV2FineRasterPass::Buffers buffers{config_, columnBuffers.columns, columnBuffers.result, scratch_, nodes_, occupancy_, materialWords_, rootIndices_, result_, indirectArgs_};
    for (uint32_t stage = 0u; stage < kPassCount; ++stage)
      passes_[stage] = graph_->registerPass<TerrainGPUV2FineRasterPass>(name_ + ".Stage" + std::to_string(stage), passIndex++, static_cast<TerrainGPUV2FineRasterPass::Stage>(stage), capacity_, workgroup_, lanes_, buffers);
  }

  TerrainGPUV2FineRasterChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const ColumnBuffers columnBuffers,
      const TerrainGPUV2FineRasterConfigGPU capacity,
      const uint32_t workgroup,
      std::string name = "TerrainGPUV2.FineRaster")
      : TerrainGPUV2FineRasterChain(graph, passIndex, columnBuffers, capacity, workgroup, 1u, std::move(name))
  {
  }

  ~TerrainGPUV2FineRasterChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(result_);
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(rootIndices_);
    graph_->deleteBuffer(materialWords_);
    graph_->deleteBuffer(occupancy_);
    graph_->deleteBuffer(nodes_);
    graph_->deleteBuffer(scratch_);
    graph_->deleteBuffer(config_);
  }

  bool update(const TerrainGPUV2FineRasterConfigGPU &config)
  {
    return updateBatch(std::vector<TerrainGPUV2FineRasterConfigGPU>{config});
  }

  bool updateBatch(const std::vector<TerrainGPUV2FineRasterConfigGPU> &configs)
  {
    if (configs.empty() || configs.size() > lanes_)
      return false;
    std::vector<TerrainGPUV2FineRasterConfigGPU> upload = configs;
    TerrainGPUV2FineRasterScratchLayout laneScratch{};
    if (!terrainGPUV2FineRasterScratchLayout(
            capacity_.denseBrickCount, laneScratch))
      return false;
    for (auto &config : upload)
    {
      if (config.denseBrickCount == 0u || config.denseBrickCount > capacity_.denseBrickCount || config.nodeCapacity > capacity_.nodeCapacity || config.materialCapacity > capacity_.materialCapacity ||
          config.materialWordCapacity > capacity_.materialWordCapacity || config.columnsX > capacity_.columnsX || config.columnsZ > capacity_.columnsZ || config.halo > capacity_.halo)
        return false;
      config.reserved[0] = capacity_.nodeCapacity;
      config.reserved[1] = laneScratch.totalWords;
      // This field is unused as a per-request bound in the shader; material
      // count owns that validation. Keep it as the immutable lane stride.
      config.materialWordCapacity = capacity_.materialWordCapacity;
    }
    graph_->bufferWrite(config_, 0u, upload.size() * sizeof(upload[0]), upload.data());
    std::array<IndirectDispatchArgsGPU, kPassCount> indirect{};
    const auto groups = [&](const uint64_t count)
    {
      return static_cast<uint32_t>((count + workgroup_ - 1u) / workgroup_);
    };
    for (auto &args : indirect)
      args = {1u, 1u, static_cast<uint32_t>(configs.size()), 0u};
    for (const auto &config : configs)
    {
      const uint32_t level0 = (config.denseBrickCount + 255u) / 256u;
      const uint32_t level1 = (level0 + 255u) / 256u;
      indirect[1].x = std::max(indirect[1].x, groups(uint64_t{config.denseBrickCount} * kTerrainGPUV2FineRasterWordsPerBrick));
      indirect[2].x = std::max(indirect[2].x, groups(config.denseBrickCount));
      indirect[3].x = std::max(indirect[3].x, level0);
      indirect[4].x = std::max(indirect[4].x, level1);
      indirect[6].x = std::max(indirect[6].x, level1);
      indirect[7].x = std::max(indirect[7].x, level0);
      indirect[9].x = std::max(indirect[9].x, groups(config.denseBrickCount));
      indirect[10].x = std::max(indirect[10].x, groups(config.nodeCapacity));
      indirect[11].x = std::max(indirect[11].x, groups(uint64_t{config.nodeCapacity} * kTerrainGPUV2FineRasterWordsPerBrick));
    }
    indirectCPU_ = indirect;
    if (enabled_)
      graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirectCPU_), indirectCPU_.data());
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
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirect), indirect.data());
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
  const std::array<IndirectDispatchArgsGPU, kPassCount> &indirectDispatchPlan() const
  {
    return indirectCPU_;
  }

  uint64_t scratchBytes() const
  {
    TerrainGPUV2FineRasterScratchLayout scratch{};
    if (!terrainGPUV2FineRasterScratchLayout(capacity_.denseBrickCount, scratch))
      return 0u;
    return uint64_t{lanes_} * scratch.totalWords * sizeof(uint32_t);
  }

  uint64_t outputBytes() const
  {
    return uint64_t{lanes_} *
        (uint64_t{capacity_.nodeCapacity} *
             (sizeof(SparseVoxelBrickNodeGPU) +
              sizeof(SparseVoxelBrickOccupancyGPU) + sizeof(uint32_t)) +
         uint64_t{capacity_.materialWordCapacity} * sizeof(uint32_t) +
         sizeof(TerrainGPUV2FineRasterResultGPU));
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
    TerrainGPUV2FineRasterScratchLayout scratch{};
    std::string error;
    if (!terrainGPUV2FineRasterScratchLayout(capacity_.denseBrickCount, scratch, &error))
      throw std::invalid_argument(error);
    const auto copy = ::rendering::BufferUsage::BufferUsage_CopySrc;
    const uint64_t lanes = lanes_;
    config_ = storage(".Config", lanes * sizeof(TerrainGPUV2FineRasterConfigGPU), ::rendering::BufferUsage::BufferUsage_Push);
    scratch_ = storage(".Scratch", lanes * scratch.totalWords * sizeof(uint32_t));
    nodes_ = storage(".Nodes", lanes * capacity_.nodeCapacity * sizeof(SparseVoxelBrickNodeGPU), copy);
    occupancy_ = storage(".Occupancy", lanes * capacity_.nodeCapacity * sizeof(SparseVoxelBrickOccupancyGPU), copy);
    materialWords_ = storage(".MaterialWords", lanes * capacity_.materialWordCapacity * sizeof(uint32_t), copy);
    rootIndices_ = storage(".Roots", lanes * capacity_.nodeCapacity * sizeof(uint32_t), copy);
    result_ = storage(".Result", lanes * sizeof(TerrainGPUV2FineRasterResultGPU), copy);
    indirectArgs_ = graph_->createBuffer(
        {.name = name_ + ".IndirectArgs",
         .size = kPassCount * sizeof(IndirectDispatchArgsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    std::array<IndirectDispatchArgsGPU, kPassCount> idle{};
    for (auto &args : idle)
      args = {0u, 1u, 1u, 0u};
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(idle), idle.data());
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  ColumnBuffers columnBuffers_{};
  TerrainGPUV2FineRasterConfigGPU capacity_{};
  uint32_t workgroup_ = 64u;
  uint32_t lanes_ = 1u;
  std::string name_{};
  std::array<TerrainGPUV2FineRasterPass *, kPassCount> passes_{};
  ::rendering::Buffer config_{};
  ::rendering::Buffer scratch_{};
  ::rendering::Buffer nodes_{};
  ::rendering::Buffer occupancy_{};
  ::rendering::Buffer materialWords_{};
  ::rendering::Buffer rootIndices_{};
  ::rendering::Buffer result_{};
  ::rendering::Buffer indirectArgs_{};
  std::array<IndirectDispatchArgsGPU, kPassCount> indirectCPU_{};
  bool enabled_ = true;
};

} // namespace voxel::svraster::rendering
