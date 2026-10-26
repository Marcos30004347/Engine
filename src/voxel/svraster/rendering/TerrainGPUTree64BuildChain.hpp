#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/TerrainGPUTree64Build.hpp"
#include "voxel/svraster/rendering/TerrainGPUTree64BuildPass.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace voxel::svraster::rendering
{

// Phase 4 owns an immutable inactive node page and result descriptor. No
// directory or dynamic-model binding is present, so only Phase 5 can make the
// validated page reachable.
class TerrainGPUTree64BuildChain final
{
public:
  static constexpr uint32_t kPassCount = 3u + (kTerrainGPUTree64MaximumLevels - 1u) + 1u;
  using StageMask = uint16_t;
  static constexpr StageMask kAllStages = static_cast<StageMask>((1u << kPassCount) - 1u);

  TerrainGPUTree64BuildChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const TerrainGPUTree64Capacity capacity,
      const uint32_t maximumColumns,
      std::string name = "TerrainGPUBuild.Phase4.Tree64",
      const ::rendering::Buffer gpuColumns = {})
      : graph_(&graph), capacity_(capacity), maximumColumns_(maximumColumns), name_(std::move(name)), columns_(gpuColumns), ownsColumns_(!gpuColumns.isValid())
  {
    if (capacity.masks == 0u || capacity.nodes == 0u || maximumColumns == 0u)
      throw std::invalid_argument("GPU terrain Tree64 chain capacity is zero");
    createBuffers();
    const TerrainGPUTree64BuildPass::Buffers buffers{config_, columns_, masks_, nodes_, maskNodeIndices_, result_, indirectArgs_};
    passes_[0] = graph_->registerPass<TerrainGPUTree64BuildPass>(name_ + ".Reset", passIndex++, 0u, 0u, 0u, capacity_, maximumColumns_, buffers);
    passes_[1] = graph_->registerPass<TerrainGPUTree64BuildPass>(name_ + ".Terminal", passIndex++, 1u, 1u, 0u, capacity_, maximumColumns_, buffers);
    for (uint32_t level = 1u; level < kTerrainGPUTree64MaximumLevels; ++level)
      passes_[level + 1u] = graph_->registerPass<TerrainGPUTree64BuildPass>(name_ + ".Upper" + std::to_string(level), passIndex++, level + 1u, 2u, level, capacity_, maximumColumns_, buffers);
    passes_[kPassCount - 2u] = graph_->registerPass<TerrainGPUTree64BuildPass>(name_ + ".Compact", passIndex++, kPassCount - 2u, 3u, 0u, capacity_, maximumColumns_, buffers);
    passes_[kPassCount - 1u] = graph_->registerPass<TerrainGPUTree64BuildPass>(name_ + ".Validate", passIndex++, kPassCount - 1u, 4u, 0u, capacity_, maximumColumns_, buffers);
  }

  ~TerrainGPUTree64BuildChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(result_);
    graph_->deleteBuffer(nodes_);
    graph_->deleteBuffer(maskNodeIndices_);
    graph_->deleteBuffer(masks_);
    if (ownsColumns_)
      graph_->deleteBuffer(columns_);
    graph_->deleteBuffer(config_);
  }

  bool update(const WorldMapTerrainColumnPage &page, const TerrainGPUTree64ConfigGPU &config, const StageMask stages = kAllStages)
  {
    if (page.columns.size() > maximumColumns_ || page.columns.size() != config.columnCount || config.maskCapacity != capacity_.masks || config.nodeCapacity != capacity_.nodes)
      return false;
    TerrainGPUTree64ConfigGPU configUpload = config;
    auto columnsUpload = page.columns;
    graph_->bufferWrite(config_, 0u, sizeof(configUpload), &configUpload);
    graph_->bufferWrite(columns_, 0u, columnsUpload.size() * sizeof(columnsUpload.front()), columnsUpload.data());
    std::array<IndirectDispatchArgsGPU, kPassCount> indirect{};
    const auto enable = [&](const uint32_t pass, const uint32_t items)
    {
      if ((stages & (StageMask{1u} << pass)) != 0u && items != 0u)
        indirect[pass] = {(items + 63u) / 64u, 1u, 1u, 0u};
    };
    enable(0u, 1u);
    enable(1u, config.levelCounts[0]);
    for (uint32_t level = 1u; level < kTerrainGPUTree64MaximumLevels; ++level)
      enable(level + 1u, level < config.levelCount ? config.levelCounts[level] : 0u);
    enable(kPassCount - 2u, 1u);
    enable(kPassCount - 1u, 1u);
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirect), indirect.data());
    return true;
  }

  bool updateGPUColumns(const TerrainGPUTree64ConfigGPU &config, const StageMask stages = kAllStages)
  {
    if (ownsColumns_ || config.columnCount > maximumColumns_ || config.maskCapacity != capacity_.masks || config.nodeCapacity != capacity_.nodes)
      return false;
    auto configUpload = config;
    graph_->bufferWrite(config_, 0u, sizeof(configUpload), &configUpload);
    std::array<IndirectDispatchArgsGPU, kPassCount> indirect{};
    const auto enable = [&](const uint32_t pass, const uint32_t items)
    {
      if ((stages & (StageMask{1u} << pass)) != 0u && items != 0u)
        indirect[pass] = {(items + 63u) / 64u, 1u, 1u, 0u};
    };
    enable(0u, 1u);
    enable(1u, config.levelCounts[0]);
    for (uint32_t level = 1u; level < kTerrainGPUTree64MaximumLevels; ++level)
      enable(level + 1u, level < config.levelCount ? config.levelCounts[level] : 0u);
    enable(kPassCount - 2u, 1u);
    enable(kPassCount - 1u, 1u);
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirect), indirect.data());
    return true;
  }

  ::rendering::Buffer columnInput() const
  {
    return columns_;
  }
  ::rendering::Buffer configBuffer() const
  {
    return config_;
  }

  ::rendering::Buffer nodes() const
  {
    return nodes_;
  }
  ::rendering::Buffer result() const
  {
    return result_;
  }

  uint64_t scratchBytes() const
  {
    return uint64_t{capacity_.masks} *
        (sizeof(SparseVoxelTree64NodeGPU) + sizeof(uint32_t));
  }

  uint64_t outputBytes() const
  {
    return uint64_t{capacity_.nodes} * sizeof(SparseVoxelTree64NodeGPU) +
        sizeof(TerrainGPUTree64ResultGPU);
  }

private:
  ::rendering::Buffer buffer(const std::string &suffix, const uint64_t size, const ::rendering::BufferUsage usage)
  {
    return graph_->createBuffer({
      .name = name_ + suffix,
      .size = size,
      .usage = usage,
    });
  }

  void createBuffers()
  {
    const auto input = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push;
    const auto output = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_CopySrc;
    config_ = buffer(".Config", sizeof(TerrainGPUTree64ConfigGPU), input);
    if (!columns_.isValid())
      columns_ = buffer(".Columns", uint64_t{maximumColumns_} * sizeof(WorldMapTerrainColumnGPU), input);
    masks_ = buffer(".Masks", uint64_t{capacity_.masks} * sizeof(SparseVoxelTree64NodeGPU), ::rendering::BufferUsage::BufferUsage_Storage);
    nodes_ = buffer(".Nodes", uint64_t{capacity_.nodes} * sizeof(SparseVoxelTree64NodeGPU), output);
    maskNodeIndices_ = buffer(".MaskNodeIndices", uint64_t{capacity_.masks} * sizeof(uint32_t), ::rendering::BufferUsage::BufferUsage_Storage);
    result_ = buffer(".Result", sizeof(TerrainGPUTree64ResultGPU), output);
    indirectArgs_ = buffer(".IndirectArgs", kPassCount * sizeof(IndirectDispatchArgsGPU), ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Push);
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  TerrainGPUTree64Capacity capacity_{};
  uint32_t maximumColumns_ = 0u;
  std::string name_;
  std::array<TerrainGPUTree64BuildPass *, kPassCount> passes_{};
  ::rendering::Buffer config_{};
  ::rendering::Buffer columns_{};
  ::rendering::Buffer masks_{};
  ::rendering::Buffer nodes_{};
  ::rendering::Buffer maskNodeIndices_{};
  ::rendering::Buffer result_{};
  ::rendering::Buffer indirectArgs_{};
  bool ownsColumns_ = true;
};

} // namespace voxel::svraster::rendering
