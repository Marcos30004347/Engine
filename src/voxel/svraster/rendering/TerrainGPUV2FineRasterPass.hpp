#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUV2FineRaster.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class TerrainGPUV2FineRasterPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Reset,
    Classify,
    Reduce,
    ScanLevel0,
    ScanLevel1,
    ScanLevel2,
    AddLevel1,
    AddLevel0,
    Validate,
    Compact,
    EmitNodes,
    EmitWords,
    Finalize,
  };

  struct Buffers
  {
    ::rendering::Buffer config;
    ::rendering::Buffer columns;
    ::rendering::Buffer columnResult;
    ::rendering::Buffer scratch;
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    ::rendering::Buffer materialWords;
    ::rendering::Buffer rootIndices;
    ::rendering::Buffer result;
    ::rendering::Buffer indirectArgs;
  };

  TerrainGPUV2FineRasterPass(const Stage stage, const TerrainGPUV2FineRasterConfigGPU capacity, const uint32_t workgroup, const uint32_t lanes, const Buffers buffers)
      : stage_(stage), capacity_(capacity), workgroup_(workgroup), lanes_(lanes), buffers_(buffers)
  {
    const bool compiled = workgroup == 64u || workgroup == 128u || workgroup == 256u;
    if (!compiled || lanes_ == 0u || capacity.denseBrickCount == 0u || capacity.nodeCapacity == 0u || capacity.materialCapacity == 0u || !buffers.config.isValid() || !buffers.columns.isValid() || !buffers.columnResult.isValid() ||
        !buffers.scratch.isValid() || !buffers.nodes.isValid() || !buffers.occupancy.isValid() || !buffers.materialWords.isValid() || !buffers.rootIndices.isValid() || !buffers.result.isValid() ||
        !buffers.indirectArgs.isValid())
      throw std::invalid_argument("terrain GPU V2 fine raster pass is invalid");
  }

  ~TerrainGPUV2FineRasterPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    commands.cmdBindComputePipeline(pipeline_, enabled_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u, enabled_);
    commands.cmdDispatchIndirect(
        buffers_.indirectArgs,
        static_cast<uint64_t>(stage_) * sizeof(IndirectDispatchArgsGPU),
        enabled_);
  }

  void setEnabled(const bool enabled)
  {
    *enabled_ = enabled;
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  const char *entry() const
  {
    switch (stage_)
    {
    case Stage::Reset:
      return "resetTerrainGPUV2FineRaster";
    case Stage::Classify:
      return "classifyTerrainGPUV2FineRaster";
    case Stage::Reduce:
      return "reduceTerrainGPUV2FineRaster";
    case Stage::ScanLevel0:
      return "scanTerrainGPUV2FineRasterLevel0";
    case Stage::ScanLevel1:
      return "scanTerrainGPUV2FineRasterLevel1";
    case Stage::ScanLevel2:
      return "scanTerrainGPUV2FineRasterLevel2";
    case Stage::AddLevel1:
      return "addTerrainGPUV2FineRasterLevel1";
    case Stage::AddLevel0:
      return "addTerrainGPUV2FineRasterLevel0";
    case Stage::Validate:
      return "validateTerrainGPUV2FineRaster";
    case Stage::Compact:
      return "compactTerrainGPUV2FineRaster";
    case Stage::EmitNodes:
      return "emitTerrainGPUV2FineRasterNodes";
    case Stage::EmitWords:
      return "emitTerrainGPUV2FineRasterWords";
    case Stage::Finalize:
      return "finalizeTerrainGPUV2FineRaster";
    }
    return "";
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    TerrainGPUV2FineRasterScratchLayout scratch{};
    std::string error;
    if (!terrainGPUV2FineRasterScratchLayout(capacity_.denseBrickCount, scratch, &error))
      throw std::runtime_error(error);
    ::rendering::BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 9u; ++binding)
      group.buffers.push_back({
        .name = "buffer" + std::to_string(binding),
        .binding = binding,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    layout_ = renderGraph->createBindingsLayout({.name = passName + ".Layout", .groups = {std::move(group)}});
    const uint64_t columnCount = uint64_t{capacity_.columnsX + 2u * capacity_.halo} * (capacity_.columnsZ + 2u * capacity_.halo);
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto rw = ::rendering::AccessPattern::SHADER_READ_WRITE;
    groups_ = renderGraph->createBindingGroups(
        {.name = passName + ".Groups",
         .layout = layout_,
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.bufferView = view(buffers_.config, uint64_t{lanes_} * sizeof(TerrainGPUV2FineRasterConfigGPU), read), .binding = 0u},
             {.bufferView = view(buffers_.columns, uint64_t{lanes_} * columnCount * sizeof(TerrainGPUV2ColumnGPU), read), .binding = 1u},
             {.bufferView = view(buffers_.columnResult, uint64_t{lanes_} * sizeof(TerrainGPUV2ColumnResultGPU), read), .binding = 2u},
             {.bufferView = view(buffers_.scratch, uint64_t{lanes_} * scratch.totalWords * sizeof(uint32_t), rw), .binding = 3u},
             {.bufferView = view(buffers_.nodes, uint64_t{lanes_} * capacity_.nodeCapacity * sizeof(SparseVoxelBrickNodeGPU), rw), .binding = 4u},
             {.bufferView = view(buffers_.occupancy, uint64_t{lanes_} * capacity_.nodeCapacity * sizeof(SparseVoxelBrickOccupancyGPU), rw), .binding = 5u},
             {.bufferView = view(buffers_.materialWords, uint64_t{lanes_} * capacity_.materialWordCapacity * sizeof(uint32_t), rw), .binding = 6u},
             {.bufferView = view(buffers_.rootIndices, uint64_t{lanes_} * capacity_.nodeCapacity * sizeof(uint32_t), rw), .binding = 7u},
             {.bufferView = view(buffers_.result, uint64_t{lanes_} * sizeof(TerrainGPUV2FineRasterResultGPU), rw), .binding = 8u},
           }}}});
    const std::string path = "assets/shaders/spirv/svraster/terrain-gpu-v2-fine-raster-" + std::to_string(workgroup_) + ".spirv";
    shader_ = renderGraph->createShader({.name = passName + ".Shader", .src = os::io::readRelativeFile(path), .type = ::rendering::ShaderType::SpirV, .layout = layout_});
    pipeline_ = renderGraph->createComputePipeline({.name = passName + ".Pipeline", .shader = shader_, .entry = entry(), .layout = layout_});
  }

  Stage stage_{};
  TerrainGPUV2FineRasterConfigGPU capacity_{};
  uint32_t workgroup_ = 64u;
  uint32_t lanes_ = 1u;
  Buffers buffers_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  std::shared_ptr<bool> enabled_ = std::make_shared<bool>(true);
};

} // namespace voxel::svraster::rendering
