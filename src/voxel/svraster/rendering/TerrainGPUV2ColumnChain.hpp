#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2ColumnPass.hpp"

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

class TerrainGPUV2ColumnChain final
{
public:
  static constexpr uint32_t kPassCount = 5u;

  struct AtlasBuffers
  {
    ::rendering::Buffer metadata;
    ::rendering::Buffer pageTable;
    ::rendering::Buffer physical;
    uint64_t metadataBytes = 0u;
    uint64_t pageTableBytes = 0u;
    uint64_t physicalBytes = 0u;
  };

  struct Capacities
  {
    uint32_t columnsX = 64u;
    uint32_t columnsZ = 64u;
    uint32_t halo = 2u;
    uint32_t faults = 4096u;
    uint32_t workgroupX = 8u;
    uint32_t workgroupZ = 8u;
    uint32_t lanes = 1u;
  };

  TerrainGPUV2ColumnChain(::rendering::RenderGraph &graph, uint32_t &passIndex, const AtlasBuffers atlas, const Capacities capacities, std::string name = "TerrainGPUV2.Columns")
      : graph_(&graph), atlas_(atlas), capacities_(capacities), name_(std::move(name))
  {
    if (!atlas.metadata.isValid() || !atlas.pageTable.isValid() || !atlas.physical.isValid() || capacities.columnsX == 0u || capacities.columnsZ == 0u || capacities.halo == 0u || capacities.faults == 0u || capacities.lanes == 0u)
      throw std::invalid_argument("terrain GPU V2 column chain is invalid");
    createBuffers();
    const TerrainGPUV2ColumnPass::Capacities passCapacities{
      capacities.columnsX, capacities.columnsZ, capacities.halo, capacities.faults, capacities.workgroupX, capacities.workgroupZ, capacities.lanes, atlas.metadataBytes, atlas.pageTableBytes, atlas.physicalBytes};
    const TerrainGPUV2ColumnPass::Buffers buffers{atlas.metadata, atlas.pageTable, atlas.physical, config_, heights_, materials_, columns_, faults_, result_, indirectArgs_};
    for (uint32_t stage = 0u; stage < kPassCount; ++stage)
      passes_[stage] = graph_->registerPass<TerrainGPUV2ColumnPass>(name_ + ".Stage" + std::to_string(stage), passIndex++, static_cast<TerrainGPUV2ColumnPass::Stage>(stage), passCapacities, buffers);
  }

  ~TerrainGPUV2ColumnChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(result_);
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(faults_);
    graph_->deleteBuffer(columns_);
    graph_->deleteBuffer(materials_);
    graph_->deleteBuffer(heights_);
    graph_->deleteBuffer(config_);
  }

  bool update(const TerrainGPUV2ColumnConfigGPU &config)
  {
    return updateBatch(std::vector<TerrainGPUV2ColumnConfigGPU>{config});
  }

  bool updateBatch(const std::vector<TerrainGPUV2ColumnConfigGPU> &configs)
  {
    if (configs.empty() || configs.size() > capacities_.lanes)
      return false;
    std::vector<TerrainGPUV2ColumnConfigGPU> upload = configs;
    for (auto &config : upload)
    {
      if (config.columnsX == 0u || config.columnsX > capacities_.columnsX || config.columnsZ == 0u || config.columnsZ > capacities_.columnsZ || config.halo == 0u || config.halo > capacities_.halo ||
          config.faultCapacity > capacities_.faults)
        return false;
      config.reserved[0] = capacities_.workgroupX | (capacities_.workgroupZ << 16u);
      config.reserved[1] = static_cast<uint32_t>(heightCount());
      config.reserved[2] = static_cast<uint32_t>(columnCount());
      config.reserved[3] = capacities_.faults;
    }
    graph_->bufferWrite(config_, 0u, upload.size() * sizeof(upload[0]), upload.data());
    std::array<IndirectDispatchArgsGPU, kPassCount> indirect{};
    const uint32_t lanes = static_cast<uint32_t>(configs.size());
    indirect[0] = {1u, 1u, lanes, 0u};
    for (const auto &config : configs)
    {
      const uint32_t width = config.columnsX + config.halo * 2u;
      const uint32_t depth = config.columnsZ + config.halo * 2u;
      indirect[1].x = std::max(indirect[1].x, (width + 1u + capacities_.workgroupX - 1u) / capacities_.workgroupX);
      indirect[1].y = std::max(indirect[1].y, (depth + 1u + capacities_.workgroupZ - 1u) / capacities_.workgroupZ);
      indirect[2].x = indirect[3].x = std::max(indirect[2].x, (width + capacities_.workgroupX - 1u) / capacities_.workgroupX);
      indirect[2].y = indirect[3].y = std::max(indirect[2].y, (depth + capacities_.workgroupZ - 1u) / capacities_.workgroupZ);
    }
    for (uint32_t stage = 1u; stage < 4u; ++stage)
      indirect[stage].z = lanes;
    indirect[4] = {1u, 1u, lanes, 0u};
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

  ::rendering::Buffer heights() const
  {
    return heights_;
  }
  ::rendering::Buffer columns() const
  {
    return columns_;
  }
  ::rendering::Buffer faults() const
  {
    return faults_;
  }
  ::rendering::Buffer result() const
  {
    return result_;
  }
  ::rendering::Buffer indirectArgs() const
  {
    return indirectArgs_;
  }

  uint64_t scratchBytes() const
  {
    return uint64_t{capacities_.lanes} *
        (heightCount() * sizeof(TerrainGPUV2HeightSampleGPU) +
         columnCount() * sizeof(uint32_t));
  }

  uint64_t outputBytes() const
  {
    return uint64_t{capacities_.lanes} *
        (columnCount() * sizeof(TerrainGPUV2ColumnGPU) +
         uint64_t{capacities_.faults} * sizeof(TerrainGPUV2MapFaultGPU) +
         sizeof(TerrainGPUV2ColumnResultGPU));
  }

private:
  uint64_t heightCount() const
  {
    return uint64_t{capacities_.columnsX + capacities_.halo * 2u + 1u} * (capacities_.columnsZ + capacities_.halo * 2u + 1u);
  }

  uint64_t columnCount() const
  {
    return uint64_t{capacities_.columnsX + capacities_.halo * 2u} * (capacities_.columnsZ + capacities_.halo * 2u);
  }

  ::rendering::Buffer storage(const std::string &suffix, const uint64_t size, const ::rendering::BufferUsage extra)
  {
    return graph_->createBuffer({.name = name_ + suffix, .size = size, .usage = ::rendering::BufferUsage::BufferUsage_Storage | extra});
  }

  void createBuffers()
  {
    if (heightCount() > std::numeric_limits<uint32_t>::max() ||
        columnCount() > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument(
          "terrain GPU V2 column lane stride exceeds the shader ABI");
    const auto output = ::rendering::BufferUsage::BufferUsage_CopySrc;
    const uint64_t lanes = capacities_.lanes;
    config_ = storage(".Config", lanes * sizeof(TerrainGPUV2ColumnConfigGPU), ::rendering::BufferUsage::BufferUsage_Push);
    heights_ = storage(".Heights", lanes * heightCount() * sizeof(TerrainGPUV2HeightSampleGPU), output);
    materials_ = storage(".Materials", lanes * columnCount() * sizeof(uint32_t), ::rendering::BufferUsage::BufferUsage_None);
    columns_ = storage(".Columns", lanes * columnCount() * sizeof(TerrainGPUV2ColumnGPU), output);
    faults_ = storage(".Faults", lanes * uint64_t{capacities_.faults} * sizeof(TerrainGPUV2MapFaultGPU), output);
    result_ = storage(".Result", lanes * sizeof(TerrainGPUV2ColumnResultGPU), output);
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
  AtlasBuffers atlas_{};
  Capacities capacities_{};
  std::string name_{};
  std::array<TerrainGPUV2ColumnPass *, kPassCount> passes_{};
  ::rendering::Buffer config_{};
  ::rendering::Buffer heights_{};
  ::rendering::Buffer materials_{};
  ::rendering::Buffer columns_{};
  ::rendering::Buffer faults_{};
  ::rendering::Buffer result_{};
  ::rendering::Buffer indirectArgs_{};
  std::array<IndirectDispatchArgsGPU, kPassCount> indirectCPU_{};
  bool enabled_ = true;
};

} // namespace voxel::svraster::rendering
