#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/TerrainGPURasterBuild.hpp"
#include "voxel/svraster/rendering/TerrainGPURasterBuildPass.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

// Fixed graph for one Phase 3 build lane. All outputs are private inactive-page
// storage; this class has no publication binding and therefore cannot expose a
// partially built raster page to the live renderer.
class TerrainGPURasterBuildChain final
{
public:
  static constexpr uint32_t kPassCount = 2u + kTerrainGPURasterMaximumLevels;
  using StageMask = uint16_t;
  static constexpr StageMask kAllStages = static_cast<StageMask>((1u << kPassCount) - 1u);
  TerrainGPURasterBuildChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const TerrainGPURasterCapacity capacity,
      const uint32_t maximumColumns,
      std::string name = "TerrainGPUBuild.Phase3.Raster",
      const ::rendering::Buffer gpuColumns = {},
      const ::rendering::Buffer gpuColumnResult = {})
      : graph_(&graph), capacity_(capacity), maximumColumns_(maximumColumns), name_(std::move(name)), columns_(gpuColumns), columnResult_(gpuColumnResult), ownsColumns_(!gpuColumns.isValid()), ownsColumnResult_(!gpuColumnResult.isValid())
  {
    if (capacity.nodes == 0u || capacity.denseCandidates == 0u || capacity.children == 0u || capacity.materials == 0u || capacity.roots == 0u || maximumColumns == 0u)
      throw std::invalid_argument("GPU terrain raster chain capacity is zero");
    createBuffers();
    const TerrainGPURasterBuildPass::Buffers buffers{config_, columns_, materialTable_, nodes_, occupancy_, materialWords_, microSurfaceData_, childIndices_, rootIndices_, candidates_, scratch_, result_, columnResult_, indirectArgs_};
    passes_[0] = graph_->registerPass<TerrainGPURasterBuildPass>(name_ + ".Reset", passIndex++, 0u, 0u, 0u, capacity_, maximumColumns_, buffers);
    for (uint32_t level = 0u; level < kTerrainGPURasterMaximumLevels; ++level)
      passes_[level + 1u] = graph_->registerPass<TerrainGPURasterBuildPass>(name_ + ".Level" + std::to_string(level), passIndex++, level + 1u, 1u, level, capacity_, maximumColumns_, buffers);
    passes_[kPassCount - 1u] = graph_->registerPass<TerrainGPURasterBuildPass>(name_ + ".Validate", passIndex++, kPassCount - 1u, 2u, 0u, capacity_, maximumColumns_, buffers);
  }

  ~TerrainGPURasterBuildChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(result_);
    if (ownsColumnResult_)
      graph_->deleteBuffer(columnResult_);
    graph_->deleteBuffer(scratch_);
    graph_->deleteBuffer(candidates_);
    graph_->deleteBuffer(rootIndices_);
    graph_->deleteBuffer(childIndices_);
    graph_->deleteBuffer(microSurfaceData_);
    graph_->deleteBuffer(materialWords_);
    graph_->deleteBuffer(occupancy_);
    graph_->deleteBuffer(nodes_);
    graph_->deleteBuffer(materialTable_);
    if (ownsColumns_)
      graph_->deleteBuffer(columns_);
    graph_->deleteBuffer(config_);
  }

  bool update(
      const WorldMapTerrainColumnPage &page,
      const TerrainGPURasterConfigGPU &config,
      const std::array<TerrainGPURasterMaterialGPU, kTerrainGPURasterMaterialTableSize> &materialTable,
      const StageMask stages = kAllStages)
  {
    if (page.columns.size() != page.walls.size() || page.columns.size() > maximumColumns_ || config.nodeCapacity != capacity_.nodes || config.childCapacity != capacity_.children ||
        config.materialCapacity != capacity_.materials || config.rootCapacity != capacity_.roots || config.denseCandidateCapacity != capacity_.denseCandidates)
      return false;
    std::vector<TerrainGPURasterColumnGPU> upload(page.columns.size());
    for (size_t index = 0u; index < upload.size(); ++index)
      upload[index] = {page.columns[index], page.walls[index]};
    TerrainGPURasterConfigGPU configUpload = config;
    auto materialTableUpload = materialTable;
    graph_->bufferWrite(config_, 0u, sizeof(configUpload), &configUpload);
    graph_->bufferWrite(columns_, 0u, upload.size() * sizeof(upload.front()), upload.data());
    graph_->bufferWrite(materialTable_, 0u, sizeof(materialTableUpload), materialTableUpload.data());
    std::array<IndirectDispatchArgsGPU, kPassCount> indirect{};
    for (uint32_t pass = 0u; pass < kPassCount; ++pass)
      if ((stages & (StageMask{1u} << pass)) != 0u)
        indirect[pass] = {1u, 1u, 1u};
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirect), indirect.data());
    return true;
  }

  bool updateGPUColumns(const TerrainGPURasterConfigGPU &config, const std::array<TerrainGPURasterMaterialGPU, kTerrainGPURasterMaterialTableSize> &materialTable, const StageMask stages = kAllStages)
  {
    if (ownsColumns_ || config.columnCount > maximumColumns_ || config.nodeCapacity != capacity_.nodes || config.childCapacity != capacity_.children || config.materialCapacity != capacity_.materials ||
        config.rootCapacity != capacity_.roots || config.denseCandidateCapacity != capacity_.denseCandidates)
      return false;
    auto configUpload = config;
    auto materialTableUpload = materialTable;
    graph_->bufferWrite(config_, 0u, sizeof(configUpload), &configUpload);
    graph_->bufferWrite(materialTable_, 0u, sizeof(materialTableUpload), materialTableUpload.data());
    std::array<IndirectDispatchArgsGPU, kPassCount> indirect{};
    for (uint32_t pass = 0u; pass < kPassCount; ++pass)
      if ((stages & (StageMask{1u} << pass)) != 0u)
        indirect[pass] = {1u, 1u, 1u};
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirect), indirect.data());
    return true;
  }

  ::rendering::Buffer columnInput() const
  {
    return columns_;
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
  ::rendering::Buffer microSurfaceData() const
  {
    return microSurfaceData_;
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

  uint64_t scratchBytes() const
  {
    return uint64_t{capacity_.denseCandidates} * 2u * sizeof(uint32_t) +
        (16u + uint64_t{capacity_.nodes} * 512u) * sizeof(uint32_t);
  }

  uint64_t outputBytes() const
  {
    return uint64_t{capacity_.nodes} * sizeof(SparseVoxelBrickNodeGPU) +
        uint64_t{capacity_.nodes} * sizeof(SparseVoxelBrickOccupancyGPU) +
        ((uint64_t{capacity_.materials} + 3u) / 4u) * sizeof(uint32_t) +
        uint64_t{capacity_.materials} * sizeof(uint64_t) +
        uint64_t{capacity_.children} * sizeof(uint32_t) +
        uint64_t{capacity_.roots} * sizeof(uint32_t) +
        sizeof(TerrainGPURasterResultGPU);
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
    config_ = buffer(".Config", sizeof(TerrainGPURasterConfigGPU), input);
    if (!columns_.isValid())
      columns_ = buffer(".Columns", uint64_t{maximumColumns_} * sizeof(TerrainGPURasterColumnGPU), input);
    if (!columnResult_.isValid())
      columnResult_ = buffer(".ColumnResult", sizeof(TerrainGPUColumnResultGPU), input);
    materialTable_ = buffer(".MaterialTable", kTerrainGPURasterMaterialTableSize * sizeof(TerrainGPURasterMaterialGPU), input);
    nodes_ = buffer(".Nodes", uint64_t{capacity_.nodes} * sizeof(SparseVoxelBrickNodeGPU), output);
    occupancy_ = buffer(".Occupancy", uint64_t{capacity_.nodes} * sizeof(SparseVoxelBrickOccupancyGPU), output);
    materialWords_ = buffer(".MaterialWords", ((uint64_t{capacity_.materials} + 3u) / 4u) * sizeof(uint32_t), output);
    microSurfaceData_ = buffer(".MicroSurfaceData", uint64_t{capacity_.materials} * sizeof(uint64_t), output);
    childIndices_ = buffer(".Children", uint64_t{capacity_.children} * sizeof(uint32_t), output);
    rootIndices_ = buffer(".Roots", uint64_t{capacity_.roots} * sizeof(uint32_t), output);
    candidates_ = buffer(".Candidates", uint64_t{capacity_.denseCandidates} * 2u * sizeof(uint32_t), ::rendering::BufferUsage::BufferUsage_Storage);
    scratch_ = buffer(".Scratch", (16u + uint64_t{capacity_.nodes} * 512u) * sizeof(uint32_t), ::rendering::BufferUsage::BufferUsage_Storage);
    result_ = buffer(".Result", sizeof(TerrainGPURasterResultGPU), output);
    indirectArgs_ = buffer(".IndirectArgs", kPassCount * sizeof(IndirectDispatchArgsGPU), ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Push);
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  TerrainGPURasterCapacity capacity_{};
  uint32_t maximumColumns_ = 0u;
  std::string name_;
  std::array<TerrainGPURasterBuildPass *, kPassCount> passes_{};
  ::rendering::Buffer config_{};
  ::rendering::Buffer columns_{};
  ::rendering::Buffer columnResult_{};
  ::rendering::Buffer materialTable_{};
  ::rendering::Buffer nodes_{};
  ::rendering::Buffer occupancy_{};
  ::rendering::Buffer materialWords_{};
  ::rendering::Buffer microSurfaceData_{};
  ::rendering::Buffer childIndices_{};
  ::rendering::Buffer rootIndices_{};
  ::rendering::Buffer candidates_{};
  ::rendering::Buffer scratch_{};
  ::rendering::Buffer result_{};
  ::rendering::Buffer indirectArgs_{};
  bool ownsColumns_ = true;
  bool ownsColumnResult_ = true;
};

} // namespace voxel::svraster::rendering
