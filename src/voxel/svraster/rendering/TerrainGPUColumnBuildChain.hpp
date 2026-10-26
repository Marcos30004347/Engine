#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/TerrainGPUColumnBuild.hpp"
#include "voxel/svraster/rendering/TerrainGPUColumnBuildPass.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace voxel::svraster::rendering
{

// Phase 7 samples a committed map atlas and writes inactive canonical-column
// storage. The atlas buffers are borrowed; all generated buffers are owned.
class TerrainGPUColumnBuildChain final
{
public:
  static constexpr uint32_t kPassCount = 6u;
  using StageMask = uint8_t;
  static constexpr StageMask kAllStages = 0x3fu;

  struct AtlasBuffers
  {
    ::rendering::Buffer metadata;
    ::rendering::Buffer pageTable;
    ::rendering::Buffer physical;
    uint64_t metadataBytes = 0u;
    uint64_t pageTableBytes = 0u;
    uint64_t physicalBytes = 0u;
  };

  TerrainGPUColumnBuildChain(::rendering::RenderGraph &graph, uint32_t &passIndex, AtlasBuffers atlas, uint32_t maximumSide, uint32_t maximumHalo, std::string name = "TerrainGPUBuild.Phase7.Columns")
      : graph_(&graph), atlas_(atlas), maximumSide_(maximumSide), maximumHalo_(maximumHalo), name_(std::move(name))
  {
    if (maximumSide == 0u || maximumHalo == 0u || !atlas.metadata.isValid() || !atlas.pageTable.isValid() || !atlas.physical.isValid())
      throw std::invalid_argument("GPU terrain column chain is invalid");
    createBuffers();
    const TerrainGPUColumnBuildPass::BufferSizes sizes{
      atlas.metadataBytes,
      atlas.pageTableBytes,
      atlas.physicalBytes,
      maximumSide,
      maximumHalo,
    };
    const TerrainGPUColumnBuildPass::Buffers buffers{
      atlas.metadata,
      atlas.pageTable,
      atlas.physical,
      config_,
      cornerHeights_,
      materials_,
      tops_,
      columns_,
      walls_,
      rasterColumns_,
      canonicalCorners_,
      boundaryMinimums_,
      boundaryMaximums_,
      result_,
      indirectArgs_,
    };
    for (uint32_t stage = 0u; stage < kPassCount; ++stage)
      passes_[stage] = graph_->registerPass<TerrainGPUColumnBuildPass>(name_ + ".Stage" + std::to_string(stage), passIndex++, stage, stage, sizes, buffers);
  }

  ~TerrainGPUColumnBuildChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(result_);
    graph_->deleteBuffer(boundaryMaximums_);
    graph_->deleteBuffer(boundaryMinimums_);
    graph_->deleteBuffer(canonicalCorners_);
    graph_->deleteBuffer(rasterColumns_);
    graph_->deleteBuffer(walls_);
    graph_->deleteBuffer(columns_);
    graph_->deleteBuffer(tops_);
    graph_->deleteBuffer(materials_);
    graph_->deleteBuffer(cornerHeights_);
    graph_->deleteBuffer(config_);
  }

  bool update(const TerrainGPUColumnConfigGPU &config, StageMask stages = kAllStages)
  {
    if (config.side == 0u || config.side > maximumSide_ || config.halo == 0u || config.halo > maximumHalo_)
      return false;
    auto configUpload = config;
    graph_->bufferWrite(config_, 0u, sizeof(configUpload), &configUpload);
    std::array<IndirectDispatchArgsGPU, kPassCount> indirect{};
    for (uint32_t stage = 0u; stage < kPassCount; ++stage)
      if ((stages & (StageMask{1u} << stage)) != 0u)
        indirect[stage] = {1u, 1u, 1u, 0u};
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirect), indirect.data());
    return true;
  }

  ::rendering::Buffer columns() const
  {
    return columns_;
  }
  ::rendering::Buffer walls() const
  {
    return walls_;
  }
  ::rendering::Buffer rasterColumns() const
  {
    return rasterColumns_;
  }
  ::rendering::Buffer canonicalCorners() const
  {
    return canonicalCorners_;
  }
  ::rendering::Buffer boundaryMinimums() const
  {
    return boundaryMinimums_;
  }
  ::rendering::Buffer boundaryMaximums() const
  {
    return boundaryMaximums_;
  }
  ::rendering::Buffer result() const
  {
    return result_;
  }

  uint64_t scratchBytes() const
  {
    const uint64_t side = maximumSide_;
    const uint64_t halo = maximumHalo_;
    return (side + halo * 2u + 1u) * (side + halo * 2u + 1u) *
            sizeof(float) +
        side * side * sizeof(uint32_t) +
        (side + halo * 2u) * (side + halo * 2u) * sizeof(uint32_t);
  }

  uint64_t outputBytes() const
  {
    const uint64_t side = maximumSide_;
    const uint64_t count = side * side;
    return count * sizeof(WorldMapTerrainColumnGPU) +
        count * sizeof(WorldMapTerrainColumnWallGPU) +
        count * sizeof(TerrainGPURasterColumnGPU) +
        (side + 1u) * (side + 1u) * sizeof(TerrainGPUHeight64GPU) +
        8u * side * sizeof(TerrainGPUHeight64GPU) +
        sizeof(TerrainGPUColumnResultGPU);
  }

private:
  ::rendering::Buffer buffer(const std::string &suffix, uint64_t size, ::rendering::BufferUsage usage)
  {
    return graph_->createBuffer({.name = name_ + suffix, .size = size, .usage = usage});
  }

  void createBuffers()
  {
    const auto input = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push;
    const auto output = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_CopySrc;
    const uint64_t side = maximumSide_;
    const uint64_t halo = maximumHalo_;
    const uint64_t count = side * side;
    config_ = buffer(".Config", sizeof(TerrainGPUColumnConfigGPU), input);
    cornerHeights_ = buffer(".CornerScratch", (side + halo * 2u + 1u) * (side + halo * 2u + 1u) * sizeof(float), ::rendering::BufferUsage::BufferUsage_Storage);
    materials_ = buffer(".MaterialScratch", count * sizeof(uint32_t), ::rendering::BufferUsage::BufferUsage_Storage);
    tops_ = buffer(".TopScratch", (side + halo * 2u) * (side + halo * 2u) * sizeof(uint32_t), ::rendering::BufferUsage::BufferUsage_Storage);
    columns_ = buffer(".Columns", count * sizeof(WorldMapTerrainColumnGPU), output);
    walls_ = buffer(".Walls", count * sizeof(WorldMapTerrainColumnWallGPU), output);
    rasterColumns_ = buffer(".RasterColumns", count * sizeof(TerrainGPURasterColumnGPU), output);
    canonicalCorners_ = buffer(".CanonicalCorners", (side + 1u) * (side + 1u) * sizeof(TerrainGPUHeight64GPU), output);
    boundaryMinimums_ = buffer(".BoundaryMinimums", 4u * side * sizeof(TerrainGPUHeight64GPU), output);
    boundaryMaximums_ = buffer(".BoundaryMaximums", 4u * side * sizeof(TerrainGPUHeight64GPU), output);
    result_ = buffer(".Result", sizeof(TerrainGPUColumnResultGPU), output);
    indirectArgs_ = buffer(".IndirectArgs", kPassCount * sizeof(IndirectDispatchArgsGPU), ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Push);
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  AtlasBuffers atlas_{};
  uint32_t maximumSide_ = 0u;
  uint32_t maximumHalo_ = 0u;
  std::string name_;
  std::array<TerrainGPUColumnBuildPass *, kPassCount> passes_{};
  ::rendering::Buffer config_{};
  ::rendering::Buffer cornerHeights_{};
  ::rendering::Buffer materials_{};
  ::rendering::Buffer tops_{};
  ::rendering::Buffer columns_{};
  ::rendering::Buffer walls_{};
  ::rendering::Buffer rasterColumns_{};
  ::rendering::Buffer canonicalCorners_{};
  ::rendering::Buffer boundaryMinimums_{};
  ::rendering::Buffer boundaryMaximums_{};
  ::rendering::Buffer result_{};
  ::rendering::Buffer indirectArgs_{};
};

} // namespace voxel::svraster::rendering
