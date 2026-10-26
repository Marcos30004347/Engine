#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUV2Tree64.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class TerrainGPUV2Tree64Pass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Reset,
    ClearMasks,
    TerminalMasks,
    UpperMask,
    Flags,
    ScanLevel0,
    ScanLevel1,
    ScanLevel2,
    AddLevel1,
    AddLevel0,
    ValidateCapacity,
    EmitNodes,
    Link,
    ValidateNodes,
    Finalize,
  };

  struct Buffers
  {
    ::rendering::Buffer config;
    ::rendering::Buffer rasterNodes;
    ::rendering::Buffer rasterOccupancy;
    ::rendering::Buffer rasterResult;
    ::rendering::Buffer masks;
    ::rendering::Buffer scratch;
    ::rendering::Buffer nodes;
    ::rendering::Buffer result;
    ::rendering::Buffer indirectArgs;
  };

  TerrainGPUV2Tree64Pass(const Stage stage, const uint32_t level, const uint32_t passSlot, const TerrainGPUV2Tree64ConfigGPU capacity, const uint32_t workgroup, const uint32_t lanes, const Buffers buffers)
      : stage_(stage), passSlot_(passSlot), level_(level), capacity_(capacity), workgroup_(workgroup), lanes_(lanes), buffers_(buffers)
  {
    const bool compiled = workgroup == 64u || workgroup == 128u || workgroup == 256u;
    if (!compiled || lanes_ == 0u || capacity.candidateCount == 0u || capacity.nodeCapacity == 0u || capacity.rasterNodeCapacity == 0u ||
        ((stage == Stage::UpperMask || stage == Stage::Link) && (level == 0u || level >= capacity.levelCount)) || !buffers.config.isValid() || !buffers.rasterNodes.isValid() || !buffers.rasterOccupancy.isValid() ||
        !buffers.rasterResult.isValid() || !buffers.masks.isValid() || !buffers.scratch.isValid() || !buffers.nodes.isValid() || !buffers.result.isValid() || !buffers.indirectArgs.isValid())
      throw std::invalid_argument("terrain GPU V2 Tree64 pass is invalid");
  }

  ~TerrainGPUV2Tree64Pass() override
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
        uint64_t{passSlot_} * sizeof(IndirectDispatchArgsGPU), enabled_);
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

  std::string entry() const
  {
    switch (stage_)
    {
    case Stage::Reset:
      return "resetTerrainGPUV2Tree64";
    case Stage::ClearMasks:
      return "clearTerrainGPUV2Tree64Masks";
    case Stage::TerminalMasks:
      return "emitTerrainGPUV2Tree64TerminalMasks";
    case Stage::UpperMask:
      return "buildTerrainGPUV2Tree64Upper" + std::to_string(level_);
    case Stage::Flags:
      return "flagTerrainGPUV2Tree64Candidates";
    case Stage::ScanLevel0:
      return "scanTerrainGPUV2Tree64Level0";
    case Stage::ScanLevel1:
      return "scanTerrainGPUV2Tree64Level1";
    case Stage::ScanLevel2:
      return "scanTerrainGPUV2Tree64Level2";
    case Stage::AddLevel1:
      return "addTerrainGPUV2Tree64Level1";
    case Stage::AddLevel0:
      return "addTerrainGPUV2Tree64Level0";
    case Stage::ValidateCapacity:
      return "validateTerrainGPUV2Tree64Capacity";
    case Stage::EmitNodes:
      return "emitTerrainGPUV2Tree64Nodes";
    case Stage::Link:
      return "linkTerrainGPUV2Tree64Level" + std::to_string(level_);
    case Stage::ValidateNodes:
      return "validateTerrainGPUV2Tree64Nodes";
    case Stage::Finalize:
      return "finalizeTerrainGPUV2Tree64";
    }
    return {};
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    TerrainGPUV2Tree64ScratchLayout scratch{};
    std::string error;
    if (!terrainGPUV2Tree64ScratchLayout(capacity_.candidateCount, scratch, &error))
      throw std::runtime_error(error);
    ::rendering::BindingGroupLayout group{};
    for (uint32_t binding = 0u; binding < 8u; ++binding)
      group.buffers.push_back(
          {.name = "buffer" + std::to_string(binding),
           .binding = binding,
           .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
           .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
           .isDynamic = false});
    layout_ = renderGraph->createBindingsLayout({.name = passName + ".Layout", .groups = {std::move(group)}});
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto rw = ::rendering::AccessPattern::SHADER_READ_WRITE;
    groups_ = renderGraph->createBindingGroups(
        {.name = passName + ".Groups",
         .layout = layout_,
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.bufferView = view(buffers_.config, uint64_t{lanes_} * sizeof(TerrainGPUV2Tree64ConfigGPU), read), .binding = 0u},
             {.bufferView = view(buffers_.rasterNodes, uint64_t{lanes_} * capacity_.rasterNodeCapacity * sizeof(SparseVoxelBrickNodeGPU), read), .binding = 1u},
             {.bufferView = view(buffers_.rasterOccupancy, uint64_t{lanes_} * capacity_.rasterNodeCapacity * sizeof(SparseVoxelBrickOccupancyGPU), read), .binding = 2u},
             {.bufferView = view(buffers_.rasterResult, uint64_t{lanes_} * sizeof(TerrainGPUV2FineRasterResultGPU), read), .binding = 3u},
             {.bufferView = view(buffers_.masks, uint64_t{lanes_} * capacity_.candidateCount * sizeof(uint64_t), rw), .binding = 4u},
             {.bufferView = view(buffers_.scratch, uint64_t{lanes_} * scratch.totalWords * sizeof(uint32_t), rw), .binding = 5u},
             {.bufferView = view(buffers_.nodes, uint64_t{lanes_} * capacity_.nodeCapacity * sizeof(SparseVoxelTree64NodeGPU), rw), .binding = 6u},
             {.bufferView = view(buffers_.result, uint64_t{lanes_} * sizeof(TerrainGPUV2Tree64ResultGPU), rw), .binding = 7u},
           }}}});
    const std::string path = "assets/shaders/spirv/svraster/terrain-gpu-v2-tree64-" + std::to_string(workgroup_) + ".spirv";
    shader_ = renderGraph->createShader({.name = passName + ".Shader", .src = os::io::readRelativeFile(path), .type = ::rendering::ShaderType::SpirV, .layout = layout_});
    const std::string entryPoint = entry();
    pipeline_ = renderGraph->createComputePipeline({.name = passName + ".Pipeline", .shader = shader_, .entry = entryPoint.c_str(), .layout = layout_});
  }

  Stage stage_{};
  uint32_t passSlot_ = 0u;
  uint32_t level_ = 0u;
  TerrainGPUV2Tree64ConfigGPU capacity_{};
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
